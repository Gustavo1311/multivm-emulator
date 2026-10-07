/*
 * Servidor VNC (RFB 3.3/3.7/3.8) para ver e controlar a tela da VM de outro
 * aparelho. Uma thread propria atende ate VNC_MAX_CLIENTS clientes com poll().
 *
 * Tela: copia o framebuffer (mvm_fb_copy) quando a geracao muda e manda so os
 * blocos de 64x64 que mudaram desde o ultimo envio a cada cliente, em ZRLE
 * (blocos deflate "armazenados" num fluxo zlib continuo; a compressao vem do
 * RLE/paleta do proprio ZRLE), Hextile ou Raw. Troca de resolucao vai pela
 * pseudo-codificacao DesktopSize.
 *
 * Entrada: keysym X11 -> codigo evdev (teclado americano); o ponteiro absoluto
 * vira deslocamentos para o mouse PS/2 relativo (o cursor do convidado pode se
 * desalinhar se ele acelerar o mouse).
 */
#include "internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

void des_encrypt_block(const uint8_t key[8], const uint8_t in[8], uint8_t out[8]);
size_t jpeg_encode(const uint32_t *xrgb, uint32_t stride, int w, int h, int quality, uint8_t *out, size_t cap);
int64_t host_clock_ns(void);

static uint32_t rd_be32_(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

#define VNC_MAX_CLIENTS 4
#define TILE 64
#define OUT_HIGH (256 * 1024) /* nao gera atualizacao enquanto houver isso por enviar */
#define FB_MAX_W 2560
#define FB_MAX_H 1600

enum { ST_VERSION, ST_SECTYPE, ST_AUTH, ST_INIT, ST_NORMAL };

enum {
    ENC_RAW = 0,
    ENC_HEXTILE = 5,
    ENC_TIGHT = 7,
    ENC_ZRLE = 16,
    ENC_DESKTOP_SIZE = -223,
};

typedef struct {
    uint8_t bpp, depth, big, truecolor;
    uint16_t rmax, gmax, bmax;
    uint8_t rsh, gsh, bsh;
} pixfmt;

typedef struct {
    int fd;
    int state;
    int minor;
    uint8_t challenge[16];
    uint8_t in[8192];
    size_t inlen;
    uint8_t *out;
    size_t outlen, outpos, outcap;
    pixfmt pf;
    int enc;        /* codificacao escolhida (a primeira suportada na ordem do cliente) */
    int jpeg;       /* qualidade JPEG pedida pelo cliente (0-9) ou -1: Tight sem JPEG */
    bool desktop;
    bool want, incremental;
    uint16_t rx, ry, rw, rh;
    uint32_t *last; /* ultimo quadro enviado */
    uint32_t lw, lh;
    bool full;      /* manda tudo na proxima atualizacao */
    bool zhdr;      /* cabecalho zlib ja enviado (ZRLE sem zlib do sistema) */
    zdl_stream *zrle_z;     /* fluxo deflate do ZRLE */
    zdl_stream *tight_z[4]; /* os quatro fluxos do Tight */
    uint32_t tile[TILE * TILE];  /* pixels do bloco ja no formato do cliente */
    uint8_t *zbuf;          /* saida comprimida */
    size_t zcap;
    bool ptr_synced;
    int px, py;
    uint8_t buttons;
    uint8_t *tmp;   /* bloco codificado */
    size_t tmpcap;
} vnc_client;

struct mvm_vnc {
    mvm_vm *vm;
    int lfd;
    int wake[2];
    pthread_t thread;
    _Atomic int quit;
    _Atomic int nclients;
    char password[9];
    vnc_client *c[VNC_MAX_CLIENTS];
    uint32_t *fb;
    uint32_t w, h, gen;
    bool have_fb;
};

/* ---------------------------------------------------------------- saida */

static void out_reserve(vnc_client *c, size_t n)
{
    if (c->outlen + n <= c->outcap)
        return;
    if (c->outpos) { /* compacta o que ja foi enviado */
        memmove(c->out, c->out + c->outpos, c->outlen - c->outpos);
        c->outlen -= c->outpos;
        c->outpos = 0;
        if (c->outlen + n <= c->outcap)
            return;
    }
    size_t cap = c->outcap ? c->outcap : 65536;
    while (cap < c->outlen + n)
        cap *= 2;
    c->out = realloc(c->out, cap);
    c->outcap = cap;
}

static void put(vnc_client *c, const void *p, size_t n)
{
    out_reserve(c, n);
    memcpy(c->out + c->outlen, p, n);
    c->outlen += n;
}

static void put8(vnc_client *c, uint8_t v) { put(c, &v, 1); }
static void put16(vnc_client *c, uint16_t v)
{
    uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    put(c, b, 2);
}
static void put32(vnc_client *c, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    put(c, b, 4);
}

static bool flush_out(vnc_client *c)
{
    while (c->outpos < c->outlen) {
        ssize_t w = send(c->fd, c->out + c->outpos, c->outlen - c->outpos, MSG_NOSIGNAL);
        if (w < 0)
            return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
        c->outpos += (size_t)w;
    }
    c->outpos = c->outlen = 0;
    return true;
}

/* ---------------------------------------------------------------- pixels */

static uint32_t conv(const pixfmt *pf, uint32_t xrgb)
{
    uint32_t r = (xrgb >> 16) & 255, g = (xrgb >> 8) & 255, b = xrgb & 255;
    return ((r * pf->rmax + 127) / 255) << pf->rsh | ((g * pf->gmax + 127) / 255) << pf->gsh |
           ((b * pf->bmax + 127) / 255) << pf->bsh;
}

/* bytes de um pixel no formato do cliente */
static int pixel_bytes(const pixfmt *pf, uint32_t v, uint8_t *o)
{
    int n = pf->bpp / 8;
    for (int i = 0; i < n; i++) {
        int sh = pf->big ? 8 * (n - 1 - i) : 8 * i;
        o[i] = (uint8_t)(v >> sh);
    }
    return n;
}

/* CPIXEL do ZRLE: 3 bytes quando 32 bpp e as cores cabem em 24 bits */
static int cpixel_bytes(const pixfmt *pf, uint32_t v, uint8_t *o)
{
    uint8_t p[4];
    int n = pixel_bytes(pf, v, p);
    if (pf->bpp == 32 && pf->depth <= 24 && pf->truecolor) {
        uint32_t maxv = ((uint32_t)pf->rmax << pf->rsh) | ((uint32_t)pf->gmax << pf->gsh) | ((uint32_t)pf->bmax << pf->bsh);
        if (maxv < (1u << 24)) { /* nos 3 bytes menos significativos */
            memcpy(o, pf->big ? p + 1 : p, 3);
            return 3;
        }
        if (!(maxv & 0xff)) { /* nos 3 mais significativos */
            memcpy(o, pf->big ? p : p + 1, 3);
            return 3;
        }
    }
    memcpy(o, p, (size_t)n);
    return n;
}

/* ---------------------------------------------------------------- codificacoes */

static void tmp_reserve(vnc_client *c, size_t n)
{
    if (c->tmpcap < n) {
        c->tmpcap = n;
        c->tmp = realloc(c->tmp, n);
    }
}

static uint8_t *zbuf_reserve(vnc_client *c, size_t n)
{
    if (c->zcap < n) {
        c->zcap = n;
        c->zbuf = realloc(c->zbuf, n);
    }
    return c->zbuf;
}

/* converte o bloco (ate 64x64) uma vez para o formato do cliente; devolve true se tem uma cor so */
static bool load_tile(vnc_client *c, const uint32_t *fb, uint32_t stride, int x, int y, int w, int h)
{
    uint32_t *t = c->tile;
    uint32_t first = fb[(size_t)y * stride + (size_t)x];
    bool solid = true;
    uint32_t last_src = first, last_dst = conv(&c->pf, first);
    for (int j = 0; j < h; j++) {
        const uint32_t *row = fb + (size_t)(y + j) * stride + (size_t)x;
        for (int i = 0; i < w; i++) {
            uint32_t v = row[i];
            if (v != last_src) { /* pixels repetidos sao comuns: evita reconverter */
                last_src = v;
                last_dst = conv(&c->pf, v);
            }
            if (v != first)
                solid = false;
            *t++ = last_dst;
        }
    }
    return solid;
}

static void enc_raw(vnc_client *c, int w, int h)
{
    uint8_t px[4];
    int bpp = c->pf.bpp / 8;
    out_reserve(c, (size_t)w * (size_t)h * (size_t)bpp);
    for (int k = 0; k < w * h; k++) {
        pixel_bytes(&c->pf, c->tile[k], px);
        memcpy(c->out + c->outlen, px, (size_t)bpp);
        c->outlen += (size_t)bpp;
    }
}

/* Hextile: subblocos de 16x16 solidos ou brutos (sobre o bloco carregado) */
static void enc_hextile(vnc_client *c, int w, int h)
{
    uint8_t px[4];
    int bpp = c->pf.bpp / 8;
    for (int ty = 0; ty < h; ty += 16)
        for (int tx = 0; tx < w; tx += 16) {
            int tw = w - tx < 16 ? w - tx : 16, th = h - ty < 16 ? h - ty : 16;
            uint32_t first = c->tile[ty * w + tx];
            bool solid = true;
            for (int j = 0; j < th && solid; j++)
                for (int i = 0; i < tw; i++)
                    if (c->tile[(ty + j) * w + tx + i] != first) {
                        solid = false;
                        break;
                    }
            if (solid) { /* BackgroundSpecified */
                put8(c, 2);
                pixel_bytes(&c->pf, first, px);
                put(c, px, (size_t)bpp);
            } else {
                put8(c, 1); /* Raw */
                out_reserve(c, (size_t)(tw * th * bpp));
                for (int j = 0; j < th; j++)
                    for (int i = 0; i < tw; i++) {
                        pixel_bytes(&c->pf, c->tile[(ty + j) * w + tx + i], px);
                        memcpy(c->out + c->outlen, px, (size_t)bpp);
                        c->outlen += (size_t)bpp;
                    }
            }
        }
}

/* paleta do bloco (ate max cores); devolve o numero de cores ou -1 se passar de max */
static int tile_palette(vnc_client *c, int n, uint32_t *pal, int max)
{
    int np = 0;
    uint32_t prev = ~c->tile[0];
    for (int k = 0; k < n; k++) {
        uint32_t v = c->tile[k];
        if (v == prev)
            continue;
        prev = v;
        int i;
        for (i = 0; i < np && pal[i] != v; i++)
            ;
        if (i == np) {
            if (np == max)
                return -1;
            pal[np++] = v;
        }
    }
    return np;
}

static int pal_index(const uint32_t *pal, int np, uint32_t v)
{
    for (int i = 0; i < np; i++)
        if (pal[i] == v)
            return i;
    return 0;
}

/* um bloco ZRLE (ate 64x64, ja carregado) em c->tmp; devolve o tamanho */
static size_t zrle_tile(vnc_client *c, int w, int h)
{
    const pixfmt *pf = &c->pf;
    size_t npix = (size_t)w * (size_t)h;
    uint32_t pal[127];
    int npal = tile_palette(c, (int)npix, pal, 127);
    size_t runs = 1;
    for (size_t k = 1; k < npix; k++)
        runs += c->tile[k] != c->tile[k - 1];
    uint8_t cp[4];
    int cpb = cpixel_bytes(pf, 0, cp);
    tmp_reserve(c, 1 + npix * 4 + 127 * 4 + runs * 6 + 16);
    uint8_t *o = c->tmp;
    size_t n = 0;
    if (npal == 1) { /* cor solida */
        o[n++] = 1;
        n += (size_t)cpixel_bytes(pf, pal[0], o + n);
        return n;
    }
    bool many = npal < 0;
    size_t raw_size = npix * (size_t)cpb;
    size_t rle_size = runs * ((size_t)cpb + 2);
    int bits = npal <= 2 ? 1 : npal <= 4 ? 2 : 4;
    size_t packed_size = !many && npal <= 16 ? (size_t)npal * (size_t)cpb + ((size_t)w * (size_t)bits + 7) / 8 * (size_t)h : (size_t)-1;
    size_t prle_size = !many ? (size_t)npal * (size_t)cpb + runs * 2 : (size_t)-1;
    size_t best = raw_size;
    int mode = 0;
    if (rle_size < best) { best = rle_size; mode = 128; }
    if (packed_size < best) { best = packed_size; mode = 2; }
    if (prle_size < best) { best = prle_size; mode = 130; }
    if (mode == 0) {
        o[n++] = 0;
        for (size_t k = 0; k < npix; k++)
            n += (size_t)cpixel_bytes(pf, c->tile[k], o + n);
        return n;
    }
    if (mode == 2) { /* paleta compactada */
        o[n++] = (uint8_t)npal;
        for (int k = 0; k < npal; k++)
            n += (size_t)cpixel_bytes(pf, pal[k], o + n);
        for (int j = 0; j < h; j++) {
            uint8_t acc = 0;
            int nb = 0;
            for (int i = 0; i < w; i++) {
                acc = (uint8_t)(acc | (pal_index(pal, npal, c->tile[j * w + i]) << (8 - bits - nb)));
                nb += bits;
                if (nb == 8) {
                    o[n++] = acc;
                    acc = 0;
                    nb = 0;
                }
            }
            if (nb)
                o[n++] = acc;
        }
        return n;
    }
    /* RLE simples (128) ou com paleta (130 + cores) */
    if (mode == 130) {
        o[n++] = (uint8_t)(128 + npal);
        for (int k = 0; k < npal; k++)
            n += (size_t)cpixel_bytes(pf, pal[k], o + n);
    } else {
        o[n++] = 128;
    }
    size_t i = 0;
    while (i < npix) {
        uint32_t v = c->tile[i];
        size_t len = 1;
        while (i + len < npix && c->tile[i + len] == v)
            len++;
        if (mode == 130) {
            int k = pal_index(pal, npal, v);
            o[n++] = (uint8_t)(len > 1 ? 128 | k : k);
        } else {
            n += (size_t)cpixel_bytes(pf, v, o + n);
        }
        if (mode == 128 || len > 1) {
            size_t r = len - 1;
            while (r >= 255) {
                o[n++] = 255;
                r -= 255;
            }
            o[n++] = (uint8_t)r;
        }
        i += len;
    }
    return n;
}

/* retangulo ZRLE: dados comprimidos pela zlib do sistema (ou em blocos deflate armazenados) */
static void enc_zrle(vnc_client *c, int w, int h)
{
    size_t n = zrle_tile(c, w, h);
    if (!c->zrle_z && zdl_available())
        c->zrle_z = zdl_new(1);
    if (c->zrle_z) {
        uint8_t *z = zbuf_reserve(c, zdl_bound(n));
        long zn = zdl_compress(c->zrle_z, c->tmp, n, z, c->zcap);
        if (zn >= 0) {
            put32(c, (uint32_t)zn);
            put(c, z, (size_t)zn);
            return;
        }
    }
    /* sem zlib: o fluxo zlib vai com blocos "armazenados" (sem compressao) */
    size_t zlen = (c->zhdr ? 0 : 2) + n + 5 * ((n + 65534) / 65535);
    put32(c, (uint32_t)zlen);
    if (!c->zhdr) {
        put8(c, 0x78);
        put8(c, 0x01);
        c->zhdr = true;
    }
    size_t off = 0;
    while (off < n) {
        size_t len = n - off > 65535 ? 65535 : n - off;
        uint8_t hdr[5] = {0x00, (uint8_t)len, (uint8_t)(len >> 8), (uint8_t)~len, (uint8_t)(~len >> 8)};
        put(c, hdr, 5);
        put(c, c->tmp + off, len);
        off += len;
    }
}

/* ---- Tight ---- */

/* TPIXEL: R, G, B quando o formato e de 24 bits em 32; senao o pixel inteiro */
static bool tight_tpixel(const pixfmt *pf)
{
    return pf->bpp == 32 && pf->depth == 24 && pf->truecolor && pf->rmax == 255 && pf->gmax == 255 && pf->bmax == 255;
}

static int tight_pixel(const pixfmt *pf, uint32_t v, uint8_t *o)
{
    if (tight_tpixel(pf)) {
        o[0] = (uint8_t)(v >> pf->rsh);
        o[1] = (uint8_t)(v >> pf->gsh);
        o[2] = (uint8_t)(v >> pf->bsh);
        return 3;
    }
    return pixel_bytes(pf, v, o);
}

static void put_compact_len(vnc_client *c, size_t len)
{
    uint8_t b[3];
    int n = 0;
    b[n++] = (uint8_t)(len & 0x7f);
    if (len > 0x7f) {
        b[0] |= 0x80;
        b[n++] = (uint8_t)((len >> 7) & 0x7f);
        if (len > 0x3fff) {
            b[1] |= 0x80;
            b[n++] = (uint8_t)(len >> 14);
        }
    }
    put(c, b, (size_t)n);
}

/* dados do Tight: abaixo de 12 bytes vao crus; senao comprimidos no fluxo 'stream' */
static bool tight_data(vnc_client *c, int stream, const uint8_t *d, size_t n)
{
    if (n < 12) {
        put(c, d, n);
        return true;
    }
    if (!c->tight_z[stream])
        c->tight_z[stream] = zdl_new(1);
    if (!c->tight_z[stream])
        return false;
    uint8_t *z = zbuf_reserve(c, zdl_bound(n));
    long zn = zdl_compress(c->tight_z[stream], d, n, z, c->zcap);
    if (zn < 0)
        return false;
    put_compact_len(c, (size_t)zn);
    put(c, z, (size_t)zn);
    return true;
}

/* qualidade JPEG para cada nivel 0-9 do cliente (a mesma escala do TigerVNC) */
static const uint8_t jpeg_quality[10] = {15, 29, 41, 42, 62, 77, 79, 86, 92, 100};

static void enc_tight(vnc_client *c, const uint32_t *fb, uint32_t stride, int x, int y, int w, int h, bool solid)
{
    const pixfmt *pf = &c->pf;
    size_t npix = (size_t)w * (size_t)h;
    uint8_t px[4];
    if (solid) { /* preenchimento */
        put8(c, 0x80);
        put(c, px, (size_t)tight_pixel(pf, c->tile[0], px));
        return;
    }
    uint32_t pal[256];
    int np = tile_palette(c, (int)npix, pal, 256);
    tmp_reserve(c, npix * 4 + 1024);
    if (np < 0 && c->jpeg >= 0) { /* muitas cores (foto, gradiente): JPEG */
        size_t jn = jpeg_encode(fb + (size_t)y * stride + (size_t)x, stride, w, h, jpeg_quality[c->jpeg], c->tmp,
                                c->tmpcap);
        if (jn) {
            put8(c, 0x90);
            put_compact_len(c, jn);
            put(c, c->tmp, jn);
            return;
        }
    }
    uint8_t *o = c->tmp;
    size_t n = 0;
    if (np >= 2 && np <= 256 && (size_t)np * 8 < npix) { /* filtro de paleta, fluxo 1 */
        put8(c, 0x40 | 0x10); /* filtro explicito, fluxo 1 */
        put8(c, 1);           /* paleta */
        put8(c, (uint8_t)(np - 1));
        for (int k = 0; k < np; k++)
            put(c, px, (size_t)tight_pixel(pf, pal[k], px));
        if (np == 2) { /* 1 bit por pixel, linhas completadas ate o byte */
            for (int j = 0; j < h; j++) {
                uint8_t acc = 0;
                int nb = 0;
                for (int i = 0; i < w; i++) {
                    acc = (uint8_t)(acc | ((c->tile[j * w + i] == pal[1]) << (7 - nb)));
                    if (++nb == 8) {
                        o[n++] = acc;
                        acc = 0;
                        nb = 0;
                    }
                }
                if (nb)
                    o[n++] = acc;
            }
        } else {
            uint32_t pv = ~c->tile[0];
            uint8_t pi = 0;
            for (size_t k = 0; k < npix; k++) {
                if (c->tile[k] != pv) {
                    pv = c->tile[k];
                    pi = (uint8_t)pal_index(pal, np, pv);
                }
                o[n++] = pi;
            }
        }
        if (tight_data(c, 1, o, n))
            return;
        LOGW("vnc: tight sem zlib");
        return;
    }
    /* copia simples (sem filtro), fluxo 0 */
    put8(c, 0x00);
    for (size_t k = 0; k < npix; k++)
        n += (size_t)tight_pixel(pf, c->tile[k], o + n);
    if (!tight_data(c, 0, o, n))
        LOGW("vnc: tight sem zlib");
}

/* ---------------------------------------------------------------- atualizacoes */

static void send_update(mvm_vnc *v, vnc_client *c)
{
    if (!v->have_fb)
        return;
    uint32_t W = v->w, H = v->h;
    bool resized = c->lw != W || c->lh != H;
    if (resized) {
        free(c->last);
        c->last = calloc((size_t)W * H, 4);
        c->lw = W;
        c->lh = H;
        c->full = true;
    }
    int nt_x = (int)((W + TILE - 1) / TILE), nt_y = (int)((H + TILE - 1) / TILE);
    /* regiao pedida (o cliente sem DesktopSize continua pedindo o tamanho antigo) */
    int x0 = c->rx, y0 = c->ry, x1 = c->rx + c->rw, y1 = c->ry + c->rh;
    if (resized && c->desktop) {
        x0 = y0 = 0;
        x1 = (int)W;
        y1 = (int)H;
    }
    if (x1 > (int)W) x1 = (int)W;
    if (y1 > (int)H) y1 = (int)H;
    bool all = c->full || !c->incremental;
    static uint8_t dirty[(FB_MAX_W / TILE + 1) * (FB_MAX_H / TILE + 1)];
    int count = 0;
    for (int ty = 0; ty < nt_y; ty++)
        for (int tx = 0; tx < nt_x; tx++) {
            int x = tx * TILE, y = ty * TILE;
            int w = (int)W - x < TILE ? (int)W - x : TILE, h = (int)H - y < TILE ? (int)H - y : TILE;
            bool d = false;
            if (x + w > x0 && x < x1 && y + h > y0 && y < y1) {
                if (all) {
                    d = true;
                } else {
                    for (int j = 0; j < h && !d; j++)
                        d = memcmp(&v->fb[(size_t)(y + j) * W + (size_t)x], &c->last[(size_t)(y + j) * W + (size_t)x],
                                   (size_t)w * 4) != 0;
                }
            }
            dirty[ty * nt_x + tx] = d;
            count += d;
        }
    if (!count && !(resized && c->desktop))
        return; /* nada novo: o pedido fica pendente */
    static int dbg = -1;
    if (dbg < 0)
        dbg = getenv("MVM_VNC_DEBUG") != NULL;
    int64_t t0 = dbg ? host_clock_ns() : 0;
    size_t out0 = c->outlen;
    put8(c, 0); /* FramebufferUpdate */
    put8(c, 0);
    put16(c, (uint16_t)(count + (resized && c->desktop ? 1 : 0)));
    if (resized && c->desktop) {
        put16(c, 0);
        put16(c, 0);
        put16(c, (uint16_t)W);
        put16(c, (uint16_t)H);
        put32(c, (uint32_t)ENC_DESKTOP_SIZE);
    }
    for (int ty = 0; ty < nt_y; ty++)
        for (int tx = 0; tx < nt_x; tx++) {
            if (!dirty[ty * nt_x + tx])
                continue;
            int x = tx * TILE, y = ty * TILE;
            int w = (int)W - x < TILE ? (int)W - x : TILE, h = (int)H - y < TILE ? (int)H - y : TILE;
            put16(c, (uint16_t)x);
            put16(c, (uint16_t)y);
            put16(c, (uint16_t)w);
            put16(c, (uint16_t)h);
            bool solid = load_tile(c, v->fb, W, x, y, w, h);
            put32(c, (uint32_t)c->enc);
            switch (c->enc) {
            case ENC_TIGHT: enc_tight(c, v->fb, W, x, y, w, h, solid); break;
            case ENC_ZRLE: enc_zrle(c, w, h); break;
            case ENC_HEXTILE: enc_hextile(c, w, h); break;
            default: enc_raw(c, w, h); break;
            }
            for (int j = 0; j < h; j++)
                memcpy(&c->last[(size_t)(y + j) * W + (size_t)x], &v->fb[(size_t)(y + j) * W + (size_t)x], (size_t)w * 4);
        }
    c->full = false;
    c->want = false;
    if (dbg)
        LOGI("vnc: %d blocos, %zu bytes, %.1f ms (codificacao %d)", count, c->outlen - out0,
             (double)(host_clock_ns() - t0) / 1e6, c->enc);
}

/* ---------------------------------------------------------------- entrada */

static int keysym_to_evdev(uint32_t ks)
{
    static const char *row_num = "1234567890"; /* KEY_1..KEY_0 = 2..11 */
    if (ks >= 'a' && ks <= 'z')
        ks -= 32;
    if (ks >= 'A' && ks <= 'Z') {
        static const uint8_t letters[26] = {30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50,
                                            49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44};
        return letters[ks - 'A'];
    }
    for (int i = 0; i < 10; i++)
        if (ks == (uint32_t)row_num[i])
            return 2 + i;
    switch (ks) {
    /* simbolos do teclado americano (o Shift vem do cliente) */
    case '!': return 2;  case '@': return 3;  case '#': return 4;  case '$': return 5;
    case '%': return 6;  case '^': return 7;  case '&': return 8;  case '*': return 9;
    case '(': return 10; case ')': return 11;
    case '-': case '_': return 12;
    case '=': case '+': return 13;
    case '[': case '{': return 26;
    case ']': case '}': return 27;
    case ';': case ':': return 39;
    case '\'': case '"': return 40;
    case '`': case '~': return 41;
    case '\\': case '|': return 43;
    case ',': case '<': return 51;
    case '.': case '>': return 52;
    case '/': case '?': return 53;
    case ' ': return 57;
    case 0xe7: case 0xc7: return 39; /* c cedilha (posicao do ABNT2) */
    /* teclas de funcao (keysyms 0xffxx) */
    case 0xff08: return 14;  /* BackSpace */
    case 0xff09: return 15;  /* Tab */
    case 0xff0d: return 28;  /* Return */
    case 0xff1b: return 1;   /* Escape */
    case 0xffff: return 111; /* Delete */
    case 0xff50: return 102; /* Home */
    case 0xff51: return 105; /* Left */
    case 0xff52: return 103; /* Up */
    case 0xff53: return 106; /* Right */
    case 0xff54: return 108; /* Down */
    case 0xff55: return 104; /* Page_Up */
    case 0xff56: return 109; /* Page_Down */
    case 0xff57: return 107; /* End */
    case 0xff63: return 110; /* Insert */
    case 0xff61: return 99;  /* Print */
    case 0xff13: return 119; /* Pause */
    case 0xff14: return 70;  /* Scroll_Lock */
    case 0xff7f: return 69;  /* Num_Lock */
    case 0xffe5: return 58;  /* Caps_Lock */
    case 0xffe1: return 42;  /* Shift_L */
    case 0xffe2: return 54;  /* Shift_R */
    case 0xffe3: return 29;  /* Control_L */
    case 0xffe4: return 97;  /* Control_R */
    case 0xffe9: case 0xffe7: return 56;  /* Alt_L, Meta_L */
    case 0xffea: case 0xfe03: return 100; /* Alt_R, AltGr */
    case 0xffeb: return 125; /* Super_L */
    case 0xffec: return 126; /* Super_R */
    case 0xff67: return 127; /* Menu */
    case 0xff8d: return 96;  /* KP_Enter */
    case 0xffaa: return 55;  /* KP_Multiply */
    case 0xffab: return 78;  /* KP_Add */
    case 0xffad: return 74;  /* KP_Subtract */
    case 0xffae: case 0xff9f: return 83; /* KP_Decimal, KP_Delete */
    case 0xffaf: return 98;  /* KP_Divide */
    default: break;
    }
    if (ks >= 0xffbe && ks <= 0xffc7) /* F1-F10 */
        return 59 + (int)(ks - 0xffbe);
    if (ks == 0xffc8) return 87;  /* F11 */
    if (ks == 0xffc9) return 88;  /* F12 */
    if (ks >= 0xffb0 && ks <= 0xffb9) { /* KP_0..KP_9 */
        static const uint8_t kp[10] = {82, 79, 80, 81, 75, 76, 77, 71, 72, 73};
        return kp[ks - 0xffb0];
    }
    return 0;
}

static void pointer(mvm_vnc *v, vnc_client *c, uint8_t mask, int x, int y)
{
    /* botoes RFB: 1 esquerdo, 2 meio, 4 direito, 8/16 roda; PS/2: 1 esquerdo, 2 direito, 4 meio */
    uint32_t b = (mask & 1 ? 1u : 0) | (mask & 4 ? 2u : 0) | (mask & 2 ? 4u : 0);
    if (!c->ptr_synced) { /* leva o cursor do convidado ao canto e parte dali */
        for (int i = 0; i < 20; i++)
            mvm_pointer_event(v->vm, -127, -127, 0, 0);
        c->px = c->py = 0;
        c->ptr_synced = true;
    }
    int dz = (mask & 8) && !(c->buttons & 8) ? -1 : (mask & 16) && !(c->buttons & 16) ? 1 : 0;
    /* no canto, reancora: corrige o desalinhamento causado pela aceleracao do convidado */
    if (x == 0 || y == 0) {
        mvm_pointer_event(v->vm, x == 0 ? -400 : 0, y == 0 ? -400 : 0, 0, b);
        if (x == 0) c->px = 0;
        if (y == 0) c->py = 0;
    }
    mvm_pointer_event(v->vm, x - c->px, y - c->py, dz, b);
    c->px = x;
    c->py = y;
    c->buttons = mask;
}

/* ---------------------------------------------------------------- protocolo */

static const pixfmt default_pf = {32, 24, 0, 1, 255, 255, 255, 16, 8, 0};

static void send_server_init(mvm_vnc *v, vnc_client *c)
{
    uint32_t w = v->have_fb ? v->w : 640, h = v->have_fb ? v->h : 480;
    put16(c, (uint16_t)w);
    put16(c, (uint16_t)h);
    const pixfmt *p = &c->pf;
    uint8_t pf[16] = {p->bpp, p->depth, p->big, p->truecolor, (uint8_t)(p->rmax >> 8), (uint8_t)p->rmax,
                      (uint8_t)(p->gmax >> 8), (uint8_t)p->gmax, (uint8_t)(p->bmax >> 8), (uint8_t)p->bmax,
                      p->rsh, p->gsh, p->bsh, 0, 0, 0};
    put(c, pf, 16);
    static const char name[] = "MultiVM";
    put32(c, (uint32_t)strlen(name));
    put(c, name, strlen(name));
    c->lw = c->lh = 0; /* primeiro envio: tudo */
}

/* processa o que chegou; devolve false para desconectar */
static bool handle_input(mvm_vnc *v, vnc_client *c)
{
    for (;;) {
        uint8_t *p = c->in;
        size_t n = c->inlen, used = 0;
        switch (c->state) {
        case ST_VERSION:
            if (n < 12)
                return true;
            if (memcmp(p, "RFB 003.", 8))
                return false;
            c->minor = atoi((const char *)p + 8);
            used = 12;
            if (c->minor >= 7) {
                put8(c, 1);
                put8(c, v->password[0] ? 2 : 1);
                c->state = ST_SECTYPE;
            } else { /* 3.3: o servidor escolhe */
                put32(c, v->password[0] ? 2 : 1);
                if (v->password[0]) {
                    for (int i = 0; i < 16; i++)
                        c->challenge[i] = (uint8_t)rand();
                    put(c, c->challenge, 16);
                    c->state = ST_AUTH;
                } else {
                    c->state = ST_INIT;
                }
            }
            break;
        case ST_SECTYPE:
            if (n < 1)
                return true;
            used = 1;
            if (p[0] != (v->password[0] ? 2 : 1))
                return false;
            if (v->password[0]) {
                for (int i = 0; i < 16; i++)
                    c->challenge[i] = (uint8_t)rand();
                put(c, c->challenge, 16);
                c->state = ST_AUTH;
            } else {
                if (c->minor >= 8)
                    put32(c, 0); /* SecurityResult OK */
                c->state = ST_INIT;
            }
            break;
        case ST_AUTH: {
            if (n < 16)
                return true;
            used = 16;
            uint8_t key[8] = {0}, expect[16];
            for (int i = 0; i < 8 && v->password[i]; i++) { /* bits de cada byte invertidos (padrao do VNC) */
                uint8_t b = (uint8_t)v->password[i], r = 0;
                for (int k = 0; k < 8; k++)
                    r = (uint8_t)(r | (((b >> k) & 1) << (7 - k)));
                key[i] = r;
            }
            des_encrypt_block(key, c->challenge, expect);
            des_encrypt_block(key, c->challenge + 8, expect + 8);
            if (memcmp(expect, p, 16)) {
                put32(c, 1);
                if (c->minor >= 8) {
                    static const char why[] = "senha incorreta";
                    put32(c, (uint32_t)strlen(why));
                    put(c, why, strlen(why));
                }
                flush_out(c);
                LOGW("vnc: senha incorreta");
                return false;
            }
            put32(c, 0);
            c->state = ST_INIT;
            break;
        }
        case ST_INIT:
            if (n < 1)
                return true;
            used = 1; /* ClientInit (compartilhado): todos podem ver */
            c->pf = default_pf;
            send_server_init(v, c);
            c->state = ST_NORMAL;
            LOGI("vnc: cliente conectado");
            break;
        case ST_NORMAL:
            if (n < 1)
                return true;
            switch (p[0]) {
            case 0: /* SetPixelFormat */
                if (n < 20)
                    return true;
                used = 20;
                c->pf.bpp = p[4];
                c->pf.depth = p[5];
                c->pf.big = p[6];
                c->pf.truecolor = p[7];
                c->pf.rmax = (uint16_t)(p[8] << 8 | p[9]);
                c->pf.gmax = (uint16_t)(p[10] << 8 | p[11]);
                c->pf.bmax = (uint16_t)(p[12] << 8 | p[13]);
                c->pf.rsh = p[14];
                c->pf.gsh = p[15];
                c->pf.bsh = p[16];
                if (c->pf.bpp != 8 && c->pf.bpp != 16 && c->pf.bpp != 32)
                    return false;
                if (!c->pf.truecolor) { /* sem mapa de cores: usa BGR233 */
                    c->pf.truecolor = 1;
                    c->pf.rmax = 7; c->pf.gmax = 7; c->pf.bmax = 3;
                    c->pf.rsh = 0; c->pf.gsh = 3; c->pf.bsh = 6;
                }
                c->full = true;
                break;
            case 2: { /* SetEncodings */
                if (n < 4)
                    return true;
                size_t cnt = (size_t)(p[2] << 8 | p[3]);
                if (n < 4 + 4 * cnt)
                    return true;
                used = 4 + 4 * cnt;
                /* a primeira codificacao suportada na ordem de preferencia do cliente */
                c->enc = -1;
                c->jpeg = -1;
                c->desktop = false;
                for (size_t i = 0; i < cnt; i++) {
                    int32_t e = (int32_t)rd_be32_(p + 4 + 4 * i);
                    if (e == ENC_DESKTOP_SIZE)
                        c->desktop = true;
                    else if (e >= -32 && e <= -23) /* nivel de qualidade JPEG */
                        c->jpeg = e + 32;
                    else if (c->enc < 0 && (e == ENC_ZRLE || e == ENC_HEXTILE || e == ENC_RAW ||
                                            (e == ENC_TIGHT && zdl_available())))
                        c->enc = e;
                }
                if (c->enc < 0)
                    c->enc = ENC_RAW;
                break;
            }
            case 3: /* FramebufferUpdateRequest */
                if (n < 10)
                    return true;
                used = 10;
                c->incremental = p[1];
                c->rx = (uint16_t)(p[2] << 8 | p[3]);
                c->ry = (uint16_t)(p[4] << 8 | p[5]);
                c->rw = (uint16_t)(p[6] << 8 | p[7]);
                c->rh = (uint16_t)(p[8] << 8 | p[9]);
                c->want = true;
                break;
            case 4: { /* KeyEvent */
                if (n < 8)
                    return true;
                used = 8;
                int code = keysym_to_evdev(rd_be32_(p + 4));
                if (code)
                    mvm_key_event(v->vm, (uint32_t)code, p[1] != 0);
                break;
            }
            case 5: /* PointerEvent */
                if (n < 6)
                    return true;
                used = 6;
                pointer(v, c, p[1], p[2] << 8 | p[3], p[4] << 8 | p[5]);
                break;
            case 6: { /* ClientCutText: ignorado */
                if (n < 8)
                    return true;
                size_t len = rd_be32_(p + 4);
                if (len > 1 << 20)
                    return false;
                if (n < 8 + len)
                    return true;
                used = 8 + len;
                break;
            }
            default:
                LOGW("vnc: mensagem desconhecida %u", p[0]);
                return false;
            }
            break;
        }
        memmove(c->in, c->in + used, n - used);
        c->inlen -= used;
    }
}

/* ---------------------------------------------------------------- thread */

static void client_free(mvm_vnc *v, int i)
{
    vnc_client *c = v->c[i];
    close(c->fd);
    free(c->out);
    free(c->last);
    free(c->tmp);
    free(c->zbuf);
    zdl_free(c->zrle_z);
    for (int k = 0; k < 4; k++)
        zdl_free(c->tight_z[k]);
    free(c);
    v->c[i] = NULL;
    atomic_fetch_sub(&v->nclients, 1);
    LOGI("vnc: cliente desconectado");
}

static void accept_client(mvm_vnc *v)
{
    int fd = accept(v->lfd, NULL, NULL);
    if (fd < 0)
        return;
    int slot = -1;
    for (int i = 0; i < VNC_MAX_CLIENTS; i++)
        if (!v->c[i])
            slot = i;
    if (slot < 0) {
        close(fd);
        return;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    vnc_client *c = calloc(1, sizeof(*c));
    c->fd = fd;
    c->pf = default_pf;
    c->jpeg = -1;
    v->c[slot] = c;
    atomic_fetch_add(&v->nclients, 1);
    put(c, "RFB 003.008\n", 12);
}

/* MVM_VNC_FAKEFB=arquivo.ppm: serve a imagem no lugar da tela (medicao do codificador) */
static bool fake_fb(mvm_vnc *v)
{
    static int checked;
    static const char *path;
    if (!checked) {
        checked = 1;
        path = getenv("MVM_VNC_FAKEFB");
    }
    if (!path)
        return false;
    if (v->have_fb)
        return true;
    FILE *f = fopen(path, "rb");
    unsigned w, h, maxv;
    if (!f || fscanf(f, "P6 %u %u %u", &w, &h, &maxv) != 3 || w > FB_MAX_W || h > FB_MAX_H) {
        if (f)
            fclose(f);
        return false;
    }
    (void)!fgetc(f);
    if (!v->fb)
        v->fb = malloc((size_t)FB_MAX_W * FB_MAX_H * 4);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint8_t rgb[3] = {0, 0, 0};
        if (fread(rgb, 1, 3, f) != 3)
            break;
        v->fb[i] = (uint32_t)rgb[0] << 16 | (uint32_t)rgb[1] << 8 | rgb[2];
    }
    fclose(f);
    v->w = w;
    v->h = h;
    v->have_fb = true;
    return true;
}

static void snapshot(mvm_vnc *v)
{
    if (fake_fb(v))
        return;
    mvm_fb_info fi;
    if (!mvm_fb_info_get(v->vm, &fi) || !fi.width || !fi.height || fi.width > FB_MAX_W || fi.height > FB_MAX_H)
        return;
    uint32_t g = mvm_fb_generation(v->vm);
    if (v->have_fb && g == v->gen && fi.width == v->w && fi.height == v->h)
        return;
    if (!v->fb)
        v->fb = malloc((size_t)FB_MAX_W * FB_MAX_H * 4);
    if (mvm_fb_copy(v->vm, v->fb, (size_t)FB_MAX_W * FB_MAX_H * 4, &fi)) {
        v->w = fi.width;
        v->h = fi.height;
        v->gen = g;
        v->have_fb = true;
    }
}

static void *vnc_thread(void *opaque)
{
    mvm_vnc *v = opaque;
    while (!atomic_load(&v->quit)) {
        struct pollfd fds[2 + VNC_MAX_CLIENTS];
        int idx[2 + VNC_MAX_CLIENTS];
        int nf = 0;
        fds[nf] = (struct pollfd){v->lfd, POLLIN, 0};
        idx[nf++] = -1;
        fds[nf] = (struct pollfd){v->wake[0], POLLIN, 0};
        idx[nf++] = -2;
        for (int i = 0; i < VNC_MAX_CLIENTS; i++) {
            vnc_client *c = v->c[i];
            if (!c)
                continue;
            fds[nf] = (struct pollfd){c->fd, (short)(POLLIN | (c->outpos < c->outlen ? POLLOUT : 0)), 0};
            idx[nf++] = i;
        }
        poll(fds, (nfds_t)nf, 30);
        for (int k = 0; k < nf; k++) {
            if (!fds[k].revents)
                continue;
            if (idx[k] == -1) {
                accept_client(v);
                continue;
            }
            if (idx[k] == -2) {
                char b[16];
                ssize_t r = read(v->wake[0], b, sizeof(b));
                (void)r;
                continue;
            }
            vnc_client *c = v->c[idx[k]];
            bool alive = true;
            if (fds[k].revents & (POLLIN | POLLHUP | POLLERR)) {
                ssize_t r = recv(c->fd, c->in + c->inlen, sizeof(c->in) - c->inlen, 0);
                if (r > 0) {
                    c->inlen += (size_t)r;
                    alive = handle_input(v, c);
                } else if (r == 0 || (errno != EAGAIN && errno != EINTR)) {
                    alive = false;
                }
            }
            if (alive)
                alive = flush_out(c);
            if (!alive)
                client_free(v, idx[k]);
        }
        /* atualizacoes de tela */
        bool any = false;
        for (int i = 0; i < VNC_MAX_CLIENTS; i++)
            if (v->c[i] && v->c[i]->state == ST_NORMAL && v->c[i]->want)
                any = true;
        if (!any)
            continue;
        snapshot(v);
        for (int i = 0; i < VNC_MAX_CLIENTS; i++) {
            vnc_client *c = v->c[i];
            if (!c || c->state != ST_NORMAL || !c->want || c->outlen - c->outpos > OUT_HIGH)
                continue;
            send_update(v, c);
            if (!flush_out(c))
                client_free(v, i);
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------- API */

mvm_vnc *mvm_vnc_start(mvm_vm *vm, const char *bind_addr, int port, const char *password, char *err, size_t errlen)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(err, errlen, "vnc: socket: %s", strerror(errno));
        return NULL;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    if (!bind_addr || inet_pton(AF_INET, bind_addr, &a.sin_addr) != 1)
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 4) < 0) {
        snprintf(err, errlen, "vnc: nao consegui escutar na porta %d (%s)", port, strerror(errno));
        close(fd);
        return NULL;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    mvm_vnc *v = calloc(1, sizeof(*v));
    v->vm = vm;
    v->lfd = fd;
    if (password)
        snprintf(v->password, sizeof(v->password), "%s", password);
    if (pipe(v->wake) < 0) {
        snprintf(err, errlen, "vnc: pipe falhou");
        close(fd);
        free(v);
        return NULL;
    }
    if (pthread_create(&v->thread, NULL, vnc_thread, v) != 0) {
        snprintf(err, errlen, "vnc: nao consegui criar a thread");
        close(fd);
        close(v->wake[0]);
        close(v->wake[1]);
        free(v);
        return NULL;
    }
    LOGI("vnc: escutando em %s:%d%s", bind_addr ? bind_addr : "127.0.0.1", port, v->password[0] ? " (com senha)" : "");
    return v;
}

void mvm_vnc_stop(mvm_vnc *v)
{
    if (!v)
        return;
    atomic_store(&v->quit, 1);
    ssize_t r = write(v->wake[1], "q", 1);
    (void)r;
    pthread_join(v->thread, NULL);
    for (int i = 0; i < VNC_MAX_CLIENTS; i++)
        if (v->c[i])
            client_free(v, i);
    close(v->lfd);
    close(v->wake[0]);
    close(v->wake[1]);
    free(v->fb);
    free(v);
}

int mvm_vnc_clients(mvm_vnc *v) { return v ? atomic_load(&v->nclients) : 0; }
