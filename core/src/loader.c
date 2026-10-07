/* Carregadores: arquivos, gzip (inflate), ELF e construtor de device tree. */
#include "internal.h"

#include <errno.h>
#include <stdlib.h>

uint8_t *load_file(const char *path, size_t *size, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "nao foi possivel abrir '%s': %s", path, strerror(errno));
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) {
        snprintf(err, errlen, "erro lendo '%s'", path);
        fclose(f);
        return NULL;
    }
    uint8_t *buf = malloc((size_t)len + 1);
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        snprintf(err, errlen, "erro lendo '%s'", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (size_t)len;
    return buf;
}

/* ------------------------------------------------------------ inflate */

typedef struct {
    const uint8_t *in;
    size_t in_len, in_pos;
    uint32_t bitbuf;
    int bitcnt;
    uint8_t *out;
    size_t out_len, out_cap;
    int error;
} inflate_state;

typedef struct {
    uint16_t count[16];
    uint16_t symbol[288];
} huffman;

static int getbit_n(inflate_state *s, int n)
{
    while (s->bitcnt < n) {
        if (s->in_pos >= s->in_len) {
            s->error = 1;
            return 0;
        }
        s->bitbuf |= (uint32_t)s->in[s->in_pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    int v = (int)(s->bitbuf & ((1u << n) - 1));
    s->bitbuf >>= n;
    s->bitcnt -= n;
    return v;
}

static void out_byte(inflate_state *s, uint8_t b)
{
    if (s->out_len == s->out_cap) {
        s->out_cap = s->out_cap ? s->out_cap * 2 : 1 << 20;
        s->out = realloc(s->out, s->out_cap);
    }
    s->out[s->out_len++] = b;
}

static void huff_build(huffman *h, const uint8_t *lens, int n)
{
    uint16_t offs[16];
    memset(h->count, 0, sizeof(h->count));
    for (int i = 0; i < n; i++)
        h->count[lens[i]]++;
    h->count[0] = 0;
    offs[1] = 0;
    for (int i = 1; i < 15; i++)
        offs[i + 1] = offs[i] + h->count[i];
    for (int i = 0; i < n; i++)
        if (lens[i])
            h->symbol[offs[lens[i]]++] = (uint16_t)i;
}

static int huff_decode(inflate_state *s, const huffman *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= getbit_n(s, 1);
        int count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
        if (s->error)
            return -1;
    }
    s->error = 1;
    return -1;
}

static const uint16_t len_base[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t len_extra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                     193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                     6145, 8193, 12289, 16385, 24577};
static const uint8_t dist_extra[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                     6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int inflate_codes(inflate_state *s, const huffman *lh, const huffman *dh)
{
    for (;;) {
        int sym = huff_decode(s, lh);
        if (sym < 0 || s->error)
            return -1;
        if (sym < 256) {
            out_byte(s, (uint8_t)sym);
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29)
                return -1;
            int len = len_base[sym] + getbit_n(s, len_extra[sym]);
            int ds = huff_decode(s, dh);
            if (ds < 0 || ds >= 30)
                return -1;
            size_t dist = dist_base[ds] + (size_t)getbit_n(s, dist_extra[ds]);
            if (dist > s->out_len)
                return -1;
            for (int i = 0; i < len; i++)
                out_byte(s, s->out[s->out_len - dist]);
        }
    }
}

static int inflate_block_dynamic(inflate_state *s)
{
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int nlen = getbit_n(s, 5) + 257;
    int ndist = getbit_n(s, 5) + 1;
    int ncode = getbit_n(s, 4) + 4;
    uint8_t lens[320] = {0};
    for (int i = 0; i < ncode; i++)
        lens[order[i]] = (uint8_t)getbit_n(s, 3);
    huffman ch, lh, dh;
    huff_build(&ch, lens, 19);
    int idx = 0;
    memset(lens, 0, sizeof(lens));
    while (idx < nlen + ndist) {
        int sym = huff_decode(s, &ch);
        if (sym < 0 || s->error)
            return -1;
        if (sym < 16) {
            lens[idx++] = (uint8_t)sym;
        } else {
            int rep, val = 0;
            if (sym == 16) {
                if (idx == 0)
                    return -1;
                val = lens[idx - 1];
                rep = 3 + getbit_n(s, 2);
            } else if (sym == 17) {
                rep = 3 + getbit_n(s, 3);
            } else {
                rep = 11 + getbit_n(s, 7);
            }
            if (idx + rep > nlen + ndist)
                return -1;
            while (rep--)
                lens[idx++] = (uint8_t)val;
        }
    }
    huff_build(&lh, lens, nlen);
    huff_build(&dh, lens + nlen, ndist);
    return inflate_codes(s, &lh, &dh);
}

static int inflate_run(inflate_state *s)
{
    int last;
    do {
        last = getbit_n(s, 1);
        int type = getbit_n(s, 2);
        if (s->error)
            return -1;
        if (type == 0) {
            s->bitbuf = 0;
            s->bitcnt = 0;
            if (s->in_pos + 4 > s->in_len)
                return -1;
            unsigned len = s->in[s->in_pos] | (s->in[s->in_pos + 1] << 8);
            s->in_pos += 4;
            if (s->in_pos + len > s->in_len)
                return -1;
            for (unsigned i = 0; i < len; i++)
                out_byte(s, s->in[s->in_pos++]);
        } else if (type == 1) {
            static huffman lh, dh;
            static int built;
            if (!built) {
                uint8_t l[288];
                for (int i = 0; i < 144; i++) l[i] = 8;
                for (int i = 144; i < 256; i++) l[i] = 9;
                for (int i = 256; i < 280; i++) l[i] = 7;
                for (int i = 280; i < 288; i++) l[i] = 8;
                huff_build(&lh, l, 288);
                for (int i = 0; i < 30; i++) l[i] = 5;
                huff_build(&dh, l, 30);
                built = 1;
            }
            if (inflate_codes(s, &lh, &dh) < 0)
                return -1;
        } else if (type == 2) {
            if (inflate_block_dynamic(s) < 0)
                return -1;
        } else {
            return -1;
        }
    } while (!last);
    return 0;
}

bool is_gzip(const uint8_t *p, size_t len) { return len > 18 && p[0] == 0x1f && p[1] == 0x8b && p[2] == 8; }

uint8_t *gunzip(const uint8_t *in, size_t len, size_t *out_len)
{
    if (!is_gzip(in, len))
        return NULL;
    uint8_t flg = in[3];
    size_t pos = 10;
    if (flg & 4) { /* FEXTRA */
        if (pos + 2 > len) return NULL;
        pos += 2 + (in[pos] | (in[pos + 1] << 8));
    }
    if (flg & 8) while (pos < len && in[pos++]) {}
    if (flg & 16) while (pos < len && in[pos++]) {}
    if (flg & 2) pos += 2;
    if (pos >= len)
        return NULL;
    inflate_state s = {.in = in + pos, .in_len = len - pos};
    if (inflate_run(&s) < 0) {
        free(s.out);
        return NULL;
    }
    *out_len = s.out_len;
    return s.out;
}

/* ---------------------------------------------------------------- ELF */

bool elf_probe(const uint8_t *img, size_t len)
{
    return len > 52 && img[0] == 0x7f && img[1] == 'E' && img[2] == 'L' && img[3] == 'F' && img[5] == 1;
}

bool elf_load(mvm_space *s, const uint8_t *img, size_t len, elf_info *info, char *err, size_t errlen)
{
    int is64 = img[4] == 2;
    uint64_t phoff, entry;
    unsigned phentsize, phnum;
    if (is64) {
        entry = ld_le(img + 24, 8);
        phoff = ld_le(img + 32, 8);
        phentsize = (unsigned)ld_le(img + 54, 2);
        phnum = (unsigned)ld_le(img + 56, 2);
    } else {
        entry = ld_le(img + 24, 4);
        phoff = ld_le(img + 28, 4);
        phentsize = (unsigned)ld_le(img + 42, 2);
        phnum = (unsigned)ld_le(img + 44, 2);
    }
    info->entry = entry;
    info->is64 = is64;
    info->low = UINT64_MAX;
    info->high = 0;
    for (unsigned i = 0; i < phnum; i++) {
        const uint8_t *ph = img + phoff + (uint64_t)i * phentsize;
        if (ph + phentsize > img + len) {
            snprintf(err, errlen, "ELF truncado");
            return false;
        }
        uint32_t type = (uint32_t)ld_le(ph, 4);
        if (type != 1)
            continue;
        uint64_t off, paddr, filesz, memsz;
        if (is64) {
            off = ld_le(ph + 8, 8);
            paddr = ld_le(ph + 24, 8);
            filesz = ld_le(ph + 32, 8);
            memsz = ld_le(ph + 40, 8);
        } else {
            off = ld_le(ph + 4, 4);
            paddr = ld_le(ph + 12, 4);
            filesz = ld_le(ph + 16, 4);
            memsz = ld_le(ph + 20, 4);
        }
        if (off + filesz > len) {
            snprintf(err, errlen, "ELF: segmento fora do arquivo");
            return false;
        }
        space_memwrite(s, paddr, img + off, filesz);
        for (uint64_t z = filesz; z < memsz; z++)
            space_write(s, paddr + z, 0, 1);
        if (paddr < info->low)
            info->low = paddr;
        if (paddr + memsz > info->high)
            info->high = paddr + memsz;
    }
    return true;
}

/* ---------------------------------------------------------------- FDT */

#define FDT_BEGIN_NODE 1
#define FDT_END_NODE 2
#define FDT_PROP 3
#define FDT_END 9

static void st_grow(fdt_builder *f, size_t n)
{
    if (f->st_len + n > f->st_cap) {
        f->st_cap = (f->st_cap + n) * 2;
        f->st = realloc(f->st, f->st_cap);
    }
}

static void st_u32(fdt_builder *f, uint32_t v)
{
    st_grow(f, 4);
    v = bswap32(v);
    memcpy(f->st + f->st_len, &v, 4);
    f->st_len += 4;
}

static void st_bytes(fdt_builder *f, const void *d, size_t n)
{
    size_t padded = (n + 3) & ~(size_t)3;
    st_grow(f, padded);
    if (n)
        memcpy(f->st + f->st_len, d, n);
    memset(f->st + f->st_len + n, 0, padded - n);
    f->st_len += padded;
}

static uint32_t str_off(fdt_builder *f, const char *name)
{
    size_t n = strlen(name) + 1;
    for (size_t i = 0; i + n <= f->str_len; i++)
        if (!memcmp(f->str + i, name, n) && (i == 0 || f->str[i - 1] == 0))
            return (uint32_t)i;
    if (f->str_len + n > f->str_cap) {
        f->str_cap = (f->str_cap + n) * 2;
        f->str = realloc(f->str, f->str_cap);
    }
    memcpy(f->str + f->str_len, name, n);
    uint32_t off = (uint32_t)f->str_len;
    f->str_len += n;
    return off;
}

void fdt_begin(fdt_builder *f) { memset(f, 0, sizeof(*f)); }

void fdt_node(fdt_builder *f, const char *name)
{
    st_u32(f, FDT_BEGIN_NODE);
    st_bytes(f, name, strlen(name) + 1);
    f->depth++;
}

void fdt_end_node(fdt_builder *f)
{
    st_u32(f, FDT_END_NODE);
    f->depth--;
}

void fdt_prop(fdt_builder *f, const char *name, const void *data, size_t len)
{
    st_u32(f, FDT_PROP);
    st_u32(f, (uint32_t)len);
    st_u32(f, str_off(f, name));
    st_bytes(f, data, len);
}

void fdt_prop_u32(fdt_builder *f, const char *name, uint32_t v)
{
    v = bswap32(v);
    fdt_prop(f, name, &v, 4);
}

void fdt_prop_u64(fdt_builder *f, const char *name, uint64_t v)
{
    v = bswap64(v);
    fdt_prop(f, name, &v, 8);
}

void fdt_prop_str(fdt_builder *f, const char *name, const char *s) { fdt_prop(f, name, s, strlen(s) + 1); }

void fdt_prop_strs(fdt_builder *f, const char *name, const char *const *s, int n)
{
    char buf[512];
    size_t len = 0;
    for (int i = 0; i < n; i++) {
        size_t l = strlen(s[i]) + 1;
        if (len + l > sizeof(buf))
            break;
        memcpy(buf + len, s[i], l);
        len += l;
    }
    fdt_prop(f, name, buf, len);
}

void fdt_prop_cells(fdt_builder *f, const char *name, const uint32_t *cells, int n)
{
    uint32_t tmp[64];
    for (int i = 0; i < n && i < 64; i++)
        tmp[i] = bswap32(cells[i]);
    fdt_prop(f, name, tmp, (size_t)n * 4);
}

void fdt_prop_empty(fdt_builder *f, const char *name) { fdt_prop(f, name, NULL, 0); }

uint8_t *fdt_finish(fdt_builder *f, size_t *len)
{
    st_u32(f, FDT_END);
    size_t hdr = 40, rsv = 16;
    size_t off_rsv = hdr, off_st = off_rsv + rsv, off_str = off_st + f->st_len;
    size_t total = (off_str + f->str_len + 7) & ~(size_t)7;
    uint8_t *b = calloc(1, total);
    uint32_t h[10] = {0xd00dfeed, (uint32_t)total, (uint32_t)off_st, (uint32_t)off_str,
                      (uint32_t)off_rsv, 17, 16, 0, (uint32_t)f->str_len, (uint32_t)f->st_len};
    for (int i = 0; i < 10; i++) {
        uint32_t v = bswap32(h[i]);
        memcpy(b + i * 4, &v, 4);
    }
    memcpy(b + off_st, f->st, f->st_len);
    memcpy(b + off_str, f->str, f->str_len);
    free(f->st);
    free(f->str);
    *len = total;
    return b;
}
