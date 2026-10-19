/*
 * virtio-gpu 2D (id 16): tela da maquina ARM "virt" com UEFI. O edk2 (VirtioGpuDxe) a usa como GOP
 * e o Linux carrega o virtio_gpu sozinho pelo apelido virtio (o simpledrm do Alpine so tem apelidos de
 * device tree e nao carregaria no modo ACPI). Fila 0 = controle, fila 1 = cursor.
 *
 * Recursos 2D ficam numa copia no host; TRANSFER_TO_HOST_2D copia da memoria do convidado (backing)
 * e RESOURCE_FLUSH copia o recurso do scanout para o framebuffer exibido (vm->fb).
 * Cursor de hardware (fila 1, usado pelo X): a imagem 64x64 fica no host e e sobreposta ao copiar
 * para vm->fb; ao mover, o retangulo antigo e recopiado do recurso (apaga) e o novo recebe o cursor.
 */
#include "devices.h"

#include <stdlib.h>

#define VIRTIO_F_VERSION_1 32

enum {
    CMD_GET_DISPLAY_INFO = 0x0100,
    CMD_RESOURCE_CREATE_2D,
    CMD_RESOURCE_UNREF,
    CMD_SET_SCANOUT,
    CMD_RESOURCE_FLUSH,
    CMD_TRANSFER_TO_HOST_2D,
    CMD_RESOURCE_ATTACH_BACKING,
    CMD_RESOURCE_DETACH_BACKING,
    CMD_GET_CAPSET_INFO,
    CMD_GET_CAPSET,
    CMD_GET_EDID,
    CMD_UPDATE_CURSOR = 0x0300,
    CMD_MOVE_CURSOR,
    RESP_OK_NODATA = 0x1100,
    RESP_OK_DISPLAY_INFO,
    RESP_ERR_UNSPEC = 0x1200,
    RESP_ERR_OUT_OF_MEMORY,
    RESP_ERR_INVALID_SCANOUT_ID,
    RESP_ERR_INVALID_RESOURCE_ID,
    RESP_ERR_INVALID_CONTEXT_ID,
    RESP_ERR_INVALID_PARAMETER,
};

#define FLAG_FENCE 1
#define MAX_RES 64
#define MAX_BACKING 16384
#define CUR_SZ 64

/* formatos: os de ordem B,G,R,X na memoria nao precisam de conversao */
#define FMT_B8G8R8A8 1
#define FMT_B8G8R8X8 2
#define FMT_A8R8G8B8 3
#define FMT_X8R8G8B8 4
#define FMT_R8G8B8A8 67
#define FMT_X8B8G8R8 68
#define FMT_A8B8G8R8 121
#define FMT_R8G8B8X8 134

typedef struct {
    uint64_t addr;
    uint32_t len;
} gpu_ent;

typedef struct {
    uint32_t id, format, w, h;
    uint8_t *px; /* w * h * 4 */
    gpu_ent *ents;
    uint32_t nents;
} gpu_res;

typedef struct {
    mvm_vm *vm;
    gpu_res res[MAX_RES];
    uint32_t scanout_res;            /* 0 = desligado */
    uint32_t sx, sy, sw, sh;         /* retangulo do scanout dentro do recurso */
    uint32_t pref_w, pref_h;
    uint32_t events;
    uint8_t *cmd;
    size_t cmd_cap;
    /* cursor: BGRA com alfa pre-multiplicado, posicao do ponto quente na tela do scanout */
    uint8_t cur_px[CUR_SZ * CUR_SZ * 4];
    int32_t cur_x, cur_y, cur_hx, cur_hy;
    bool cur_on;
} vgpu;

static gpu_res *find_res(vgpu *g, uint32_t id)
{
    if (!id)
        return NULL;
    for (int i = 0; i < MAX_RES; i++)
        if (g->res[i].id == id)
            return &g->res[i];
    return NULL;
}

static void res_free(gpu_res *r)
{
    free(r->px);
    free(r->ents);
    memset(r, 0, sizeof(*r));
}

static uint32_t gpu_cfg_read(virtio_dev *d, uint32_t off, unsigned size)
{
    vgpu *g = d->priv;
    uint8_t cfg[16] = {0};
    st_le(cfg, g->events, 4);
    st_le(cfg + 8, 1, 4); /* num_scanouts */
    uint32_t r = 0;
    for (unsigned i = 0; i < size && off + i < sizeof(cfg); i++)
        r |= (uint32_t)cfg[off + i] << (8 * i);
    return r;
}

static void gpu_cfg_write(virtio_dev *d, uint32_t off, uint32_t val, unsigned size)
{
    vgpu *g = d->priv;
    if (off == 4 && size == 4)
        g->events &= ~val; /* events_clear */
}

/* copia do backing (memoria do convidado) a partir do deslocamento off */
static void backing_read(vgpu *g, gpu_res *r, uint64_t off, uint8_t *dst, uint32_t len)
{
    for (uint32_t i = 0; i < r->nents && len; i++) {
        gpu_ent *e = &r->ents[i];
        if (off >= e->len) {
            off -= e->len;
            continue;
        }
        uint32_t n = e->len - (uint32_t)off < len ? e->len - (uint32_t)off : len;
        space_memread(&g->vm->mem, e->addr + off, dst, n);
        dst += n;
        len -= n;
        off = 0;
    }
    if (len)
        memset(dst, 0, len);
}

static void convert_row(uint32_t format, uint8_t *p, uint32_t w)
{
    if (format == FMT_R8G8B8A8 || format == FMT_R8G8B8X8 || format == FMT_X8B8G8R8 || format == FMT_A8B8G8R8) {
        /* R e B trocados em relacao a B,G,R,X (nos de ordem X,B,G,R, os bytes estao deslocados) */
        bool shifted = format == FMT_X8B8G8R8 || format == FMT_A8B8G8R8;
        for (uint32_t x = 0; x < w; x++, p += 4) {
            uint8_t r = shifted ? p[3] : p[0], gg = shifted ? p[2] : p[1], b = shifted ? p[1] : p[2];
            p[0] = b; p[1] = gg; p[2] = r; p[3] = 0;
        }
    } else if (format == FMT_A8R8G8B8 || format == FMT_X8R8G8B8) {
        for (uint32_t x = 0; x < w; x++, p += 4) {
            uint8_t b = p[3], gg = p[2], r = p[1];
            p[0] = b; p[1] = gg; p[2] = r; p[3] = 0;
        }
    }
}

/* sobrepoe o cursor na linha yy (da tela) ja copiada em dst = vm->fb na coluna x0..x1 (da tela) */
static void cursor_row(vgpu *g, uint8_t *row, int32_t yy, int32_t x0, int32_t x1)
{
    int32_t cx = g->cur_x - g->cur_hx, cy = g->cur_y - g->cur_hy;
    if (!g->cur_on || yy < cy || yy >= cy + CUR_SZ)
        return;
    int32_t a0 = x0 > cx ? x0 : cx, a1 = x1 < cx + CUR_SZ ? x1 : cx + CUR_SZ;
    const uint8_t *src = g->cur_px + ((size_t)(yy - cy) * CUR_SZ + (size_t)(a0 - cx)) * 4;
    for (int32_t x = a0; x < a1; x++, src += 4) {
        unsigned a = src[3];
        if (!a)
            continue;
        uint8_t *d = row + (size_t)x * 4;
        for (int k = 0; k < 3; k++) {
            unsigned v = src[k] + d[k] * (255 - a) / 255;
            d[k] = (uint8_t)(v > 255 ? 255 : v);
        }
    }
}

/* recopia o retangulo [x0,x1)x[y0,y1) (coordenadas da tela do scanout) do recurso para vm->fb,
 * com o cursor por cima. Chamar com fb_lock. */
static void blit(vgpu *g, gpu_res *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    mvm_vm *vm = g->vm;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int32_t)g->sw) x1 = (int32_t)g->sw;
    if (y1 > (int32_t)g->sh) y1 = (int32_t)g->sh;
    if ((int64_t)g->sx + x1 > (int64_t)r->w) x1 = (int32_t)(r->w - g->sx);
    if ((int64_t)g->sy + y1 > (int64_t)r->h) y1 = (int32_t)(r->h - g->sy);
    if (x0 >= x1 || y0 >= y1)
        return;
    for (int32_t yy = y0; yy < y1; yy++) {
        uint8_t *row = vm->fb + (size_t)yy * vm->fb_stride;
        memcpy(row + (size_t)x0 * 4, r->px + ((size_t)(g->sy + yy) * r->w + g->sx + x0) * 4, (size_t)(x1 - x0) * 4);
        convert_row(r->format, row + (size_t)x0 * 4, (uint32_t)(x1 - x0));
        cursor_row(g, row, yy, x0, x1);
    }
    vm_fb_rows_changed(vm, (uint32_t)y0, (uint32_t)y1);
}

/* copia um retangulo do recurso do scanout (coordenadas do recurso) para a tela exibida */
static void present(vgpu *g, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    mvm_vm *vm = g->vm;
    gpu_res *r = find_res(g, g->scanout_res);
    if (!r || !vm->fb || !r->px)
        return;
    pthread_mutex_lock(&vm->fb_lock);
    if (vm->fb_w != g->sw || vm->fb_h != g->sh) {
        if ((uint64_t)g->sw * g->sh * 4 > vm->fb_size) {
            pthread_mutex_unlock(&vm->fb_lock);
            return;
        }
        vm->fb_w = g->sw;
        vm->fb_h = g->sh;
        vm->fb_stride = g->sw * 4;
        x = g->sx;
        y = g->sy;
        w = g->sw;
        h = g->sh;
    }
    int64_t x0 = (int64_t)x - g->sx, y0 = (int64_t)y - g->sy;
    int64_t x1 = x0 + w, y1 = y0 + h;
    if (x1 > 0 && y1 > 0 && x0 < (int64_t)g->sw && y0 < (int64_t)g->sh)
        blit(g, r, (int32_t)(x0 < 0 ? 0 : x0), (int32_t)(y0 < 0 ? 0 : y0), (int32_t)(x1 > g->sw ? g->sw : x1),
             (int32_t)(y1 > g->sh ? g->sh : y1));
    pthread_mutex_unlock(&vm->fb_lock);
}

/* redesenha a area do cursor (posicao atual) a partir do recurso */
static void cursor_area(vgpu *g)
{
    mvm_vm *vm = g->vm;
    gpu_res *r = find_res(g, g->scanout_res);
    if (!r || !vm->fb || !r->px || vm->fb_w != g->sw || vm->fb_h != g->sh)
        return;
    int32_t cx = g->cur_x - g->cur_hx, cy = g->cur_y - g->cur_hy;
    pthread_mutex_lock(&vm->fb_lock);
    blit(g, r, cx, cy, cx + CUR_SZ, cy + CUR_SZ);
    pthread_mutex_unlock(&vm->fb_lock);
}

/* imagem do cursor a partir de um recurso: o byte X dos formatos sem alfa e tratado como alfa */
static void cursor_load(vgpu *g, gpu_res *r)
{
    memset(g->cur_px, 0, sizeof(g->cur_px));
    if (!r || !r->px)
        return;
    uint32_t w = r->w < CUR_SZ ? r->w : CUR_SZ, h = r->h < CUR_SZ ? r->h : CUR_SZ;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *p = r->px + ((size_t)y * r->w + x) * 4;
            uint8_t *d = g->cur_px + ((size_t)y * CUR_SZ + x) * 4;
            uint8_t b, gg, rr, a;
            switch (r->format) {
            case FMT_B8G8R8A8:
            case FMT_B8G8R8X8: /* o Linux cria o cursor como XRGB mas o byte X leva o alfa (como no QEMU) */
                b = p[0]; gg = p[1]; rr = p[2]; a = p[3]; break;
            case FMT_A8R8G8B8: a = p[0]; rr = p[1]; gg = p[2]; b = p[3]; break;
            case FMT_R8G8B8A8: rr = p[0]; gg = p[1]; b = p[2]; a = p[3]; break;
            case FMT_A8B8G8R8: a = p[0]; b = p[1]; gg = p[2]; rr = p[3]; break;
            case FMT_X8R8G8B8: a = p[0]; rr = p[1]; gg = p[2]; b = p[3]; break;
            case FMT_X8B8G8R8: a = p[0]; b = p[1]; gg = p[2]; rr = p[3]; break;
            case FMT_R8G8B8X8: rr = p[0]; gg = p[1]; b = p[2]; a = p[3]; break;
            default: b = p[0]; gg = p[1]; rr = p[2]; a = 255; break;
            }
            /* o X manda pre-multiplicado; se a cor passa do alfa, nao era: multiplica */
            if (b > a || gg > a || rr > a) {
                b = (uint8_t)(b * a / 255);
                gg = (uint8_t)(gg * a / 255);
                rr = (uint8_t)(rr * a / 255);
            }
            d[0] = b; d[1] = gg; d[2] = rr; d[3] = a;
        }
}

/* fila 1: UPDATE_CURSOR / MOVE_CURSOR (cabecalho 24, pos {scanout, x, y, pad}, recurso, hot_x, hot_y) */
static uint32_t cursor_cmd(vgpu *g, const uint8_t *c, size_t len)
{
    if (len < 56)
        return RESP_ERR_INVALID_PARAMETER;
    uint32_t type = (uint32_t)ld_le(c, 4);
    if ((uint32_t)ld_le(c + 24, 4) != 0)
        return RESP_ERR_INVALID_SCANOUT_ID;
    int32_t x = (int32_t)ld_le(c + 28, 4), y = (int32_t)ld_le(c + 32, 4);
    bool was = g->cur_on;
    if (was) {
        g->cur_on = false; /* apaga na posicao antiga */
        cursor_area(g);
    }
    if (type == CMD_UPDATE_CURSOR) {
        uint32_t id = (uint32_t)ld_le(c + 40, 4);
        gpu_res *r = find_res(g, id);
        g->cur_hx = (int32_t)ld_le(c + 44, 4);
        g->cur_hy = (int32_t)ld_le(c + 48, 4);
        cursor_load(g, r);
        g->cur_on = r != NULL;
        if (r && getenv("MVM_GPU_DEBUG")) {
            unsigned zero = 0;
            for (unsigned i = 0; i < CUR_SZ * CUR_SZ; i++)
                zero += g->cur_px[i * 4 + 3] == 0;
            LOGI("virtio-gpu: cursor recurso %u formato %u %ux%u hot %d,%d px0=%08x px[32,32]=%08x alfa0=%u backing=%u",
                 id, r->format, r->w, r->h, g->cur_hx, g->cur_hy, (unsigned)ld_le(r->px, 4),
                 (unsigned)ld_le(r->px + (32 * r->w + 32) * 4, 4), zero, r->nents);
        }
    } else if (type == CMD_MOVE_CURSOR) {
        g->cur_on = was;
    } else {
        g->cur_on = was;
        return RESP_ERR_UNSPEC;
    }
    g->cur_x = x;
    g->cur_y = y;
    atomic_store(&g->vm->cursor_x, x);
    atomic_store(&g->vm->cursor_y, y);
    atomic_store(&g->vm->cursor_on, g->cur_on);
    if (g->cur_on)
        cursor_area(g);
    return RESP_OK_NODATA;
}

static uint32_t do_cmd(vgpu *g, const uint8_t *c, size_t len, uint8_t *resp, size_t *rlen)
{
    uint32_t type = (uint32_t)ld_le(c, 4);
    *rlen = 24;
    static int dbg = -1;
    if (dbg < 0)
        dbg = getenv("MVM_GPU_DEBUG") != NULL;
    if (dbg)
        LOGI("virtio-gpu: comando 0x%x (%zu bytes) %u %u %u %u %u %u", type, len, len >= 28 ? (uint32_t)ld_le(c + 24, 4) : 0,
             len >= 32 ? (uint32_t)ld_le(c + 28, 4) : 0, len >= 36 ? (uint32_t)ld_le(c + 32, 4) : 0,
             len >= 40 ? (uint32_t)ld_le(c + 36, 4) : 0, len >= 44 ? (uint32_t)ld_le(c + 40, 4) : 0,
             len >= 52 ? (uint32_t)ld_le(c + 48, 4) : 0);
#define NEED(n) do { if (len < (n)) return RESP_ERR_INVALID_PARAMETER; } while (0)
    switch (type) {
    case CMD_GET_DISPLAY_INFO:
        memset(resp + 24, 0, 16 * 24);
        st_le(resp + 24 + 8, g->pref_w, 4);
        st_le(resp + 24 + 12, g->pref_h, 4);
        st_le(resp + 24 + 16, 1, 4); /* habilitado */
        *rlen = 24 + 16 * 24;
        return RESP_OK_DISPLAY_INFO;
    case CMD_RESOURCE_CREATE_2D: {
        NEED(40);
        uint32_t id = (uint32_t)ld_le(c + 24, 4), fmt = (uint32_t)ld_le(c + 28, 4);
        uint32_t w = (uint32_t)ld_le(c + 32, 4), h = (uint32_t)ld_le(c + 36, 4);
        if (!id || find_res(g, id))
            return RESP_ERR_INVALID_RESOURCE_ID;
        if (!w || !h || w > 8192 || h > 8192)
            return RESP_ERR_INVALID_PARAMETER;
        for (int i = 0; i < MAX_RES; i++)
            if (!g->res[i].id) {
                gpu_res *r = &g->res[i];
                r->px = calloc((size_t)w * h, 4);
                if (!r->px)
                    return RESP_ERR_OUT_OF_MEMORY;
                r->id = id;
                r->format = fmt;
                r->w = w;
                r->h = h;
                return RESP_OK_NODATA;
            }
        return RESP_ERR_OUT_OF_MEMORY;
    }
    case CMD_RESOURCE_UNREF: {
        NEED(32);
        uint32_t id = (uint32_t)ld_le(c + 24, 4);
        gpu_res *r = find_res(g, id);
        if (!r)
            return RESP_ERR_INVALID_RESOURCE_ID;
        if (g->scanout_res == id)
            g->scanout_res = 0;
        res_free(r);
        return RESP_OK_NODATA;
    }
    case CMD_SET_SCANOUT: {
        NEED(48);
        uint32_t x = (uint32_t)ld_le(c + 24, 4), y = (uint32_t)ld_le(c + 28, 4);
        uint32_t w = (uint32_t)ld_le(c + 32, 4), h = (uint32_t)ld_le(c + 36, 4);
        uint32_t sid = (uint32_t)ld_le(c + 40, 4), id = (uint32_t)ld_le(c + 44, 4);
        if (sid != 0)
            return RESP_ERR_INVALID_SCANOUT_ID;
        if (!id) {
            g->scanout_res = 0;
            return RESP_OK_NODATA;
        }
        gpu_res *r = find_res(g, id);
        if (!r)
            return RESP_ERR_INVALID_RESOURCE_ID;
        if (!w || !h || x + w > r->w || y + h > r->h || (uint64_t)w * h * 4 > g->vm->fb_size)
            return RESP_ERR_INVALID_PARAMETER;
        g->scanout_res = id;
        g->sx = x; g->sy = y; g->sw = w; g->sh = h;
        LOGD("virtio-gpu: scanout %ux%u (recurso %u, formato %u)", w, h, id, r->format);
        present(g, x, y, w, h);
        return RESP_OK_NODATA;
    }
    case CMD_RESOURCE_FLUSH: {
        NEED(48);
        uint32_t x = (uint32_t)ld_le(c + 24, 4), y = (uint32_t)ld_le(c + 28, 4);
        uint32_t w = (uint32_t)ld_le(c + 32, 4), h = (uint32_t)ld_le(c + 36, 4);
        uint32_t id = (uint32_t)ld_le(c + 40, 4);
        if (!find_res(g, id))
            return RESP_ERR_INVALID_RESOURCE_ID;
        if (id == g->scanout_res)
            present(g, x, y, w, h);
        return RESP_OK_NODATA;
    }
    case CMD_TRANSFER_TO_HOST_2D: {
        NEED(56);
        uint32_t x = (uint32_t)ld_le(c + 24, 4), y = (uint32_t)ld_le(c + 28, 4);
        uint32_t w = (uint32_t)ld_le(c + 32, 4), h = (uint32_t)ld_le(c + 36, 4);
        uint64_t off = ld_le(c + 40, 8);
        uint32_t id = (uint32_t)ld_le(c + 48, 4);
        gpu_res *r = find_res(g, id);
        if (!r)
            return RESP_ERR_INVALID_RESOURCE_ID;
        if (x + w > r->w || y + h > r->h || x + w < x || y + h < y)
            return RESP_ERR_INVALID_PARAMETER;
        uint32_t stride = r->w * 4;
        for (uint32_t row = 0; row < h; row++)
            backing_read(g, r, off + (uint64_t)stride * row, r->px + (size_t)(y + row) * stride + (size_t)x * 4,
                         w * 4);
        return RESP_OK_NODATA;
    }
    case CMD_RESOURCE_ATTACH_BACKING: {
        NEED(32);
        uint32_t id = (uint32_t)ld_le(c + 24, 4), n = (uint32_t)ld_le(c + 28, 4);
        gpu_res *r = find_res(g, id);
        if (!r)
            return RESP_ERR_INVALID_RESOURCE_ID;
        if (n > MAX_BACKING || len < 32 + (size_t)n * 16)
            return RESP_ERR_INVALID_PARAMETER;
        free(r->ents);
        r->ents = calloc(n ? n : 1, sizeof(gpu_ent));
        r->nents = n;
        for (uint32_t i = 0; i < n; i++) {
            r->ents[i].addr = ld_le(c + 32 + 16 * i, 8);
            r->ents[i].len = (uint32_t)ld_le(c + 32 + 16 * i + 8, 4);
        }
        return RESP_OK_NODATA;
    }
    case CMD_RESOURCE_DETACH_BACKING: {
        NEED(32);
        gpu_res *r = find_res(g, (uint32_t)ld_le(c + 24, 4));
        if (!r)
            return RESP_ERR_INVALID_RESOURCE_ID;
        free(r->ents);
        r->ents = NULL;
        r->nents = 0;
        return RESP_OK_NODATA;
    }
    default:
        LOGD("virtio-gpu: comando 0x%x nao suportado", type);
        return RESP_ERR_UNSPEC;
    }
#undef NEED
}

static void gpu_ctrl(virtio_dev *d, int q)
{
    vgpu *g = d->priv;
    virtq_elem e;
    bool any = false;
    while (virtq_pop(d, q, &e)) {
        size_t in = 0;
        for (int i = 0; i < e.nseg; i++)
            if (!e.seg[i].write)
                in += e.seg[i].len;
        if (in > g->cmd_cap) {
            g->cmd_cap = in + 4096;
            g->cmd = realloc(g->cmd, g->cmd_cap);
        }
        size_t n = virtq_read(d, &e, 0, g->cmd, in);
        uint8_t resp[24 + 16 * 24];
        size_t rlen = 24;
        uint32_t rt;
        if (n < 24) {
            rt = RESP_ERR_INVALID_PARAMETER;
        } else if (q == 1) {
            rt = cursor_cmd(g, g->cmd, n);
        } else {
            rt = do_cmd(g, g->cmd, n, resp, &rlen);
        }
        memset(resp, 0, 24);
        st_le(resp, rt, 4);
        if (n >= 24 && (ld_le(g->cmd + 4, 4) & FLAG_FENCE)) { /* devolve a cerca */
            st_le(resp + 4, FLAG_FENCE, 4);
            memcpy(resp + 8, g->cmd + 8, 8);
            memcpy(resp + 16, g->cmd + 16, 4);
        }
        size_t w = virtq_write(d, &e, 0, resp, rlen);
        virtq_push(d, q, &e, (uint32_t)w);
        any = true;
    }
    if (any)
        virtio_notify_irq(d);
}

static void gpu_reset(virtio_dev *d)
{
    vgpu *g = d->priv;
    for (int i = 0; i < MAX_RES; i++)
        res_free(&g->res[i]);
    g->scanout_res = 0;
    g->events = 0;
    g->cur_on = false;
    atomic_store(&g->vm->cursor_on, false);
}

static void gpu_destroy(virtio_dev *d)
{
    vgpu *g = d->priv;
    for (int i = 0; i < MAX_RES; i++)
        res_free(&g->res[i]);
    free(g->cmd);
    g->cmd = NULL;
}

void virtio_gpu_set_preferred(virtio_dev *d, uint32_t w, uint32_t h)
{
    vgpu *g = d->priv;
    /* largura multipla de 8; limitado ao buffer da tela */
    w &= ~7u;
    if (w < 640) w = 640;
    if (h < 480) h = 480;
    while ((uint64_t)w * h * 4 > g->vm->fb_size) {
        w = (w * 7 / 8) & ~7u;
        h = h * 7 / 8;
    }
    if (w == g->pref_w && h == g->pref_h)
        return;
    LOGI("virtio-gpu: resolucao preferida %ux%u", w, h);
    g->pref_w = w;
    g->pref_h = h;
    if (!(d->status & 4)) /* driver ainda nao pronto: ele le ao consultar a tela */
        return;
    g->events |= 1; /* VIRTIO_GPU_EVENT_DISPLAY */
    d->cfg_gen++;
    d->isr |= 2;    /* mudanca de configuracao */
    virtio_update_irq(d);
}

virtio_dev *virtio_gpu_new(mvm_vm *vm, uint32_t pref_w, uint32_t pref_h)
{
    virtio_dev *d = calloc(1, sizeof(*d));
    vgpu *g = calloc(1, sizeof(*g));
    g->vm = vm;
    g->pref_w = pref_w;
    g->pref_h = pref_h;
    d->vm = vm;
    d->priv = g;
    d->device_id = 16;
    d->host_features = 1ULL << VIRTIO_F_VERSION_1;
    d->nvq = 2;
    d->vq[0].num_max = 256;
    d->vq[1].num_max = 16;
    d->cfg_read = gpu_cfg_read;
    d->cfg_write = gpu_cfg_write;
    d->notify = gpu_ctrl;
    d->reset = gpu_reset;
    d->destroy = gpu_destroy;
    return d;
}
