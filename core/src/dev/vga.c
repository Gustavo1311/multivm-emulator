/*
 * VGA padrao (compativel com o "std VGA" do QEMU / Bochs, PCI 1234:1111):
 * registradores VGA completos, memoria planar (modos de escrita 0-3, chain4,
 * par/impar), modo texto, 16 e 256 cores, e extensoes VBE (Bochs DISPI) com
 * framebuffer linear no BAR0. A imagem e renderizada no framebuffer da VM.
 */
#include "vga.h"

#include <stdlib.h>

#define VBE_ID 0xb0c5
#define VBE_MAX_X 2560
#define VBE_MAX_Y 1600
#define VBE_ENABLED 0x01
#define VBE_GETCAPS 0x02
#define VBE_8BIT_DAC 0x20
#define VBE_LFB 0x40
#define VBE_NOCLEARMEM 0x80

enum { VBE_IDX_ID, VBE_IDX_XRES, VBE_IDX_YRES, VBE_IDX_BPP, VBE_IDX_ENABLE, VBE_IDX_BANK,
       VBE_IDX_VIRT_W, VBE_IDX_VIRT_H, VBE_IDX_XOFF, VBE_IDX_YOFF, VBE_IDX_MEM64K, VBE_NREGS };

#define REFRESH_NS 20000000LL /* 50 Hz */

struct vga {
    mvm_vm *vm;
    uint8_t *vram;
    uint32_t vram_size;
    _Atomic uint32_t vram_gen;
    uint32_t drawn_gen;
    bool regs_dirty;
    /* paginas (4 KiB) da VRAM escritas desde o ultimo desenho; o LFB marca pelo caminho lento
     * do TLB e depois escreve direto ate o desenho zerar o bit (tlb_protect) */
    uint64_t *dirty;
    uint32_t dirty_words;
    uint64_t *snap; /* copia de dirty durante o desenho */
    int drawn_mode; /* modo do ultimo desenho (-1: nenhum) */
    bool resized;

    uint8_t misc, fcr;
    uint8_t sr_idx, sr[8];
    uint8_t gr_idx, gr[16];
    uint8_t ar_idx, ar[0x15];
    bool ar_flip;
    uint8_t cr_idx, cr[0x40];
    uint8_t dac_read, dac_write, dac_sub, dac_state, dac_cache[3], pel_mask;
    uint8_t palette[768];
    uint32_t latch;
    uint32_t st01_toggle;

    uint16_t vbe_idx, vbe[VBE_NREGS];
    uint32_t vbe_start, vbe_line;
    uint32_t bank_offset;

    mvm_timer refresh;
    int64_t last_blink;
    bool blink_on;
    char text[80 * 60 + 60];
    bool text_mode;
    uint64_t dbg_writes;
    uint32_t dbg_last;
};

static const uint32_t mask16[16] = {
    0x00000000, 0x000000ff, 0x0000ff00, 0x0000ffff, 0x00ff0000, 0x00ff00ff, 0x00ffff00, 0x00ffffff,
    0xff000000, 0xff0000ff, 0xff00ff00, 0xff00ffff, 0xffff0000, 0xffff00ff, 0xffffff00, 0xffffffff,
};

static inline bool vbe_on(vga *v) { return v->vbe[VBE_IDX_ENABLE] & VBE_ENABLED; }

/* -------------------------------------------------------------- memoria */

static int map_addr(vga *v, uint32_t *addr)
{
    uint32_t a = *addr & 0x1ffff;
    switch ((v->gr[6] >> 2) & 3) {
    case 0: break;
    case 1:
        if (a >= 0x10000) return -1;
        a += v->bank_offset;
        break;
    case 2:
        a -= 0x10000;
        if (a >= 0x8000) return -1;
        break;
    default:
        a -= 0x18000;
        if (a >= 0x8000) return -1;
        break;
    }
    *addr = a;
    return 0;
}

static uint8_t mem_readb(vga *v, uint32_t addr)
{
    if (map_addr(v, &addr) < 0)
        return 0xff;
    if (v->sr[4] & 0x08) { /* chain4 */
        return addr < v->vram_size ? v->vram[addr] : 0xff;
    }
    if (v->gr[5] & 0x10) { /* par/impar */
        uint32_t plane = (v->gr[4] & 2) | (addr & 1);
        uint32_t off = ((addr & ~1u) << 1) | plane;
        return off < v->vram_size ? v->vram[off] : 0xff;
    }
    if (addr * 4 + 4 > v->vram_size)
        return 0xff;
    uint32_t latch;
    memcpy(&latch, v->vram + addr * 4, 4);
    v->latch = latch;
    if (!(v->gr[5] & 0x08))
        return (uint8_t)(latch >> ((v->gr[4] & 3) * 8));
    uint32_t r = (latch ^ mask16[v->gr[2] & 0xf]) & mask16[v->gr[7] & 0xf];
    r |= r >> 16;
    r |= r >> 8;
    return (uint8_t)(~r);
}

static inline void mark_dirty(vga *v, uint32_t off)
{
    uint32_t pg = off >> 12;
    __atomic_fetch_or(&v->dirty[pg >> 6], 1ULL << (pg & 63), __ATOMIC_RELAXED);
}

static void mem_writeb(vga *v, uint32_t addr, uint8_t val)
{
    if (map_addr(v, &addr) < 0)
        return;
    v->vram_gen++;
    if (v->sr[4] & 0x08) {
        uint32_t mask = 1u << (addr & 3);
        if ((v->sr[2] & mask) && addr < v->vram_size) {
            v->vram[addr] = val;
            mark_dirty(v, addr);
        }
        return;
    }
    if (v->gr[5] & 0x10) {
        uint32_t plane = (v->gr[4] & 2) | (addr & 1);
        if (v->sr[2] & (1u << plane)) {
            uint32_t off = ((addr & ~1u) << 1) | plane;
            if (off < v->vram_size) {
                v->vram[off] = val;
                mark_dirty(v, off);
            }
        }
        return;
    }
    if (addr * 4 + 4 > v->vram_size)
        return;
    mark_dirty(v, addr * 4);
    uint32_t w, bit_mask, b;
    switch (v->gr[5] & 3) {
    case 0:
        b = v->gr[3] & 7;
        w = ((uint32_t)(val >> b) | ((uint32_t)val << (8 - b))) & 0xff;
        w |= w << 8;
        w |= w << 16;
        {
            uint32_t set_mask = mask16[v->gr[1] & 0xf];
            w = (w & ~set_mask) | (mask16[v->gr[0] & 0xf] & set_mask);
        }
        bit_mask = v->gr[8];
        break;
    case 1:
        w = v->latch;
        goto write;
    case 2:
        w = mask16[val & 0xf];
        bit_mask = v->gr[8];
        break;
    default:
        b = v->gr[3] & 7;
        w = ((uint32_t)(val >> b) | ((uint32_t)val << (8 - b))) & 0xff;
        bit_mask = v->gr[8] & w;
        w = mask16[v->gr[0] & 0xf];
        break;
    }
    switch ((v->gr[3] >> 3) & 3) {
    case 1: w &= v->latch; break;
    case 2: w |= v->latch; break;
    case 3: w ^= v->latch; break;
    default: break;
    }
    bit_mask |= bit_mask << 8;
    bit_mask |= bit_mask << 16;
    w = (w & bit_mask) | (v->latch & ~bit_mask);
write:
    {
        uint32_t wm = mask16[v->sr[2] & 0xf], old;
        memcpy(&old, v->vram + addr * 4, 4);
        old = (old & ~wm) | (w & wm);
        memcpy(v->vram + addr * 4, &old, 4);
    }
}

static uint64_t vgamem_read(void *opaque, uint64_t off, unsigned size)
{
    vga *v = opaque;
    uint64_t r = 0;
    for (unsigned i = 0; i < size; i++)
        r |= (uint64_t)mem_readb(v, (uint32_t)(off + i)) << (8 * i);
    return r;
}

static void vgamem_write(void *opaque, uint64_t off, uint64_t val, unsigned size)
{
    vga *v = opaque;
    v->dbg_writes++;
    v->dbg_last = (uint32_t)off;
    for (unsigned i = 0; i < size; i++)
        mem_writeb(v, (uint32_t)(off + i), (uint8_t)(val >> (8 * i)));
}

const mvm_io_ops vga_mem_ops = {vgamem_read, vgamem_write};

void vga_debug_dump(vga *v)
{
    char hex[3 * 32 + 1];
    for (int i = 0; i < 32; i++)
        snprintf(hex + 3 * i, 4, "%02x ", v->vram[i]);
    LOGI("vga: misc=%02x sr=%02x %02x %02x %02x %02x gr5=%02x gr6=%02x ar_idx=%02x flip=%d ar10=%02x cr_start=%04x cr09=%02x cr17=%02x vbe=%04x texto=%d escritas=%llu ultima=%x vram: %s",
         v->misc, v->sr[0], v->sr[1], v->sr[2], v->sr[3], v->sr[4], v->gr[5], v->gr[6], v->ar_idx, v->ar_flip, v->ar[0x10],
         (v->cr[0x0c] << 8) | v->cr[0x0d], v->cr[9], v->cr[0x17], v->vbe[VBE_IDX_ENABLE], v->text_mode, (unsigned long long)v->dbg_writes, v->dbg_last, hex);
}

/* -------------------------------------------------------- portas VGA */

static bool mono(vga *v) { return !(v->misc & 1); }

static uint8_t port_read(vga *v, uint32_t port)
{
    /* portas de CRTC/status do modo inativo (mono vs cor) retornam 0xff */
    if ((port >= 0x3b0 && port <= 0x3bf && !mono(v)) || (port >= 0x3d0 && port <= 0x3df && mono(v)))
        return 0xff;
    switch (port) {
    case 0x3c0: return v->ar_flip ? 0 : v->ar_idx;
    case 0x3c1: return (v->ar_idx & 0x1f) < 0x15 ? v->ar[v->ar_idx & 0x1f] : 0;
    case 0x3c2: return 0x10; /* status 0 */
    case 0x3c3: return 1;
    case 0x3c4: return v->sr_idx;
    case 0x3c5: return v->sr[v->sr_idx & 7];
    case 0x3c6: return v->pel_mask;
    case 0x3c7: return v->dac_state;
    case 0x3c8: return v->dac_write;
    case 0x3c9: {
        uint8_t r = v->palette[v->dac_read * 3 + v->dac_sub];
        if (++v->dac_sub == 3) {
            v->dac_sub = 0;
            v->dac_read++;
        }
        return r;
    }
    case 0x3ca: return v->fcr;
    case 0x3cc: return v->misc;
    case 0x3ce: return v->gr_idx;
    case 0x3cf: return v->gr[v->gr_idx & 0xf];
    case 0x3b4: case 0x3d4: return v->cr_idx;
    case 0x3b5: case 0x3d5: return v->cr[v->cr_idx & 0x3f];
    case 0x3ba: case 0x3da: {
        v->ar_flip = false;
        /* simula retrace vertical (~1 ms a cada 16.7 ms) e horizontal */
        int64_t t = mvm_now(v->vm) % 16666667;
        uint8_t st = (uint8_t)((v->st01_toggle++ & 1) ? 1 : 0);
        if (t < 1000000)
            st |= 0x09;
        return st;
    }
    default: return 0xff;
    }
}

static void port_write(vga *v, uint32_t port, uint8_t val)
{
    if ((port >= 0x3b0 && port <= 0x3bf && !mono(v)) || (port >= 0x3d0 && port <= 0x3df && mono(v)))
        return;
    v->regs_dirty = true;
    switch (port) {
    case 0x3c0:
        if (!v->ar_flip) {
            v->ar_idx = val & 0x3f;
        } else {
            unsigned i = v->ar_idx & 0x1f;
            if (i <= 0x0f) v->ar[i] = val & 0x3f;
            else if (i == 0x10) v->ar[i] = val & ~0x10;
            else if (i == 0x11) v->ar[i] = val;
            else if (i == 0x12) v->ar[i] = val & 0x3f;
            else if (i == 0x13) v->ar[i] = val & 0x0f;
            else if (i == 0x14) v->ar[i] = val & 0x0f;
        }
        v->ar_flip = !v->ar_flip;
        break;
    case 0x3c2: v->misc = val & ~0x10; break;
    case 0x3c4: v->sr_idx = val & 7; break;
    case 0x3c5: {
        static const uint8_t srmask[8] = {0x03, 0x3d, 0x0f, 0x3f, 0x0e, 0x00, 0x00, 0xff};
        v->sr[v->sr_idx & 7] = val & srmask[v->sr_idx & 7];
        break;
    }
    case 0x3c6: v->pel_mask = val; break;
    case 0x3c7: v->dac_read = val; v->dac_sub = 0; v->dac_state = 3; break;
    case 0x3c8: v->dac_write = val; v->dac_sub = 0; v->dac_state = 0; break;
    case 0x3c9:
        v->dac_cache[v->dac_sub++] = val;
        if (v->dac_sub == 3) {
            memcpy(&v->palette[v->dac_write * 3], v->dac_cache, 3);
            v->dac_sub = 0;
            v->dac_write++;
        }
        break;
    case 0x3ce: v->gr_idx = val & 0xf; break;
    case 0x3cf: {
        static const uint8_t grmask[16] = {0x0f, 0x0f, 0x0f, 0x1f, 0x03, 0x7b, 0x0f, 0x0f, 0xff};
        v->gr[v->gr_idx & 0xf] = val & grmask[v->gr_idx & 0xf];
        break;
    }
    case 0x3b4: case 0x3d4: v->cr_idx = val; break;
    case 0x3b5: case 0x3d5: {
        unsigned i = v->cr_idx & 0x3f;
        if ((v->cr[0x11] & 0x80) && i <= 7) { /* protecao de escrita */
            if (i == 7)
                v->cr[7] = (uint8_t)((v->cr[7] & ~0x10) | (val & 0x10));
            break;
        }
        v->cr[i] = val;
        break;
    }
    case 0x3ba: case 0x3da: v->fcr = val & 0x10; break;
    default: break;
    }
}

static uint64_t vgaio_read(void *opaque, uint64_t off, unsigned size)
{
    vga *v = opaque;
    uint64_t r = 0;
    for (unsigned i = 0; i < size; i++)
        r |= (uint64_t)port_read(v, 0x3b0 + (uint32_t)off + i) << (8 * i);
    return r;
}

static void vgaio_write(void *opaque, uint64_t off, uint64_t val, unsigned size)
{
    vga *v = opaque;
    for (unsigned i = 0; i < size; i++)
        port_write(v, 0x3b0 + (uint32_t)off + i, (uint8_t)(val >> (8 * i)));
}

const mvm_io_ops vga_io_ops = {vgaio_read, vgaio_write};

/* ------------------------------------------------------------- VBE */

static void vbe_update_offsets(vga *v)
{
    unsigned bpp = v->vbe[VBE_IDX_BPP];
    unsigned vw = v->vbe[VBE_IDX_VIRT_W];
    v->vbe_line = bpp == 4 ? vw / 2 : vw * ((bpp + 7) / 8);
    uint32_t off = (uint32_t)v->vbe[VBE_IDX_YOFF] * v->vbe_line;
    if (bpp == 4)
        off += v->vbe[VBE_IDX_XOFF] / 2;
    else
        off += v->vbe[VBE_IDX_XOFF] * ((bpp + 7) / 8);
    v->vbe_start = off;
}

static uint64_t vbe_read(void *opaque, uint64_t off, unsigned size)
{
    vga *v = opaque;
    (void)size;
    if (off == 0)
        return v->vbe_idx;
    unsigned i = v->vbe_idx;
    if (i >= VBE_NREGS)
        return 0;
    if (v->vbe[VBE_IDX_ENABLE] & VBE_GETCAPS) {
        if (i == VBE_IDX_XRES) return VBE_MAX_X;
        if (i == VBE_IDX_YRES) return VBE_MAX_Y;
        if (i == VBE_IDX_BPP) return 32;
    }
    if (i == VBE_IDX_MEM64K)
        return v->vram_size >> 16;
    return v->vbe[i];
}

static void vbe_write(void *opaque, uint64_t off, uint64_t val64, unsigned size)
{
    vga *v = opaque;
    (void)size;
    uint16_t val = (uint16_t)val64;
    if (off == 0) {
        v->vbe_idx = val;
        return;
    }
    unsigned i = v->vbe_idx;
    if (i >= VBE_NREGS)
        return;
    v->regs_dirty = true;
    switch (i) {
    case VBE_IDX_ID:
        if (val >= 0xb0c0 && val <= VBE_ID)
            v->vbe[i] = val;
        break;
    case VBE_IDX_XRES:
        if (val <= VBE_MAX_X && !(val & 7) && !vbe_on(v))
            v->vbe[i] = val;
        break;
    case VBE_IDX_YRES:
        if (val <= VBE_MAX_Y && !vbe_on(v))
            v->vbe[i] = val;
        break;
    case VBE_IDX_BPP:
        if (val == 0) val = 8;
        if ((val == 4 || val == 8 || val == 15 || val == 16 || val == 24 || val == 32) && !vbe_on(v))
            v->vbe[i] = val;
        break;
    case VBE_IDX_BANK: {
        uint32_t banks = v->vram_size >> 16;
        if (val < banks) {
            v->vbe[i] = val;
            v->bank_offset = (uint32_t)val << 16;
        }
        break;
    }
    case VBE_IDX_ENABLE:
        if ((val & VBE_ENABLED) && !vbe_on(v)) {
            v->vbe[VBE_IDX_VIRT_W] = v->vbe[VBE_IDX_XRES];
            v->vbe[VBE_IDX_XOFF] = 0;
            v->vbe[VBE_IDX_YOFF] = 0;
            v->vbe[VBE_IDX_ENABLE] = val;
            vbe_update_offsets(v);
            v->vbe[VBE_IDX_VIRT_H] = (uint16_t)(v->vbe_line ? v->vram_size / v->vbe_line : 0);
            if (!(val & VBE_NOCLEARMEM)) {
                uint64_t clr = (uint64_t)v->vbe_line * v->vbe[VBE_IDX_YRES];
                memset(v->vram, 0, clr < v->vram_size ? clr : v->vram_size);
            }
            /* modo grafico com mapa de memoria de 64K (como o QEMU) */
            v->gr[6] = (uint8_t)((v->gr[6] & ~0x0c) | 0x05);
            v->cr[0x17] |= 3;
            v->cr[0x13] = (uint8_t)(v->vbe_line >> 3);
            v->cr[0x01] = (uint8_t)((v->vbe[VBE_IDX_XRES] >> 3) - 1);
            unsigned shift = v->vbe[VBE_IDX_BPP] == 4 ? 0 : 2;
            v->sr[1] &= ~8;
            v->gr[5] = (uint8_t)((v->gr[5] & ~0x60) | (shift << 5));
            v->cr[0x09] &= ~0x9f;
        } else if (!(val & VBE_ENABLED)) {
            v->bank_offset = 0;
            v->vbe[VBE_IDX_ENABLE] = val;
        } else {
            v->vbe[VBE_IDX_ENABLE] = val;
        }
        break;
    case VBE_IDX_VIRT_W: {
        unsigned bpp = v->vbe[VBE_IDX_BPP];
        uint32_t line = bpp == 4 ? val / 2u : (uint32_t)val * ((bpp + 7) / 8);
        if (val < v->vbe[VBE_IDX_XRES] || !line || (uint64_t)line * v->vbe[VBE_IDX_YRES] > v->vram_size)
            break;
        v->vbe[i] = val;
        vbe_update_offsets(v);
        v->vbe[VBE_IDX_VIRT_H] = (uint16_t)(v->vram_size / v->vbe_line);
        break;
    }
    case VBE_IDX_XOFF:
    case VBE_IDX_YOFF:
        v->vbe[i] = val;
        vbe_update_offsets(v);
        break;
    default:
        break;
    }
}

const mvm_io_ops vga_vbe_ops = {vbe_read, vbe_write};

/* ----------------------------------------------------------- renderizacao */

static inline uint32_t dac_rgb(vga *v, unsigned idx)
{
    const uint8_t *p = &v->palette[(idx & 0xff) * 3];
    if (v->vbe[VBE_IDX_ENABLE] & VBE_8BIT_DAC)
        return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    uint32_t r = (p[0] & 0x3f), g = (p[1] & 0x3f), b = (p[2] & 0x3f);
    r = (r << 2) | (r >> 4);
    g = (g << 2) | (g >> 4);
    b = (b << 2) | (b >> 4);
    return (r << 16) | (g << 8) | b;
}

/* indice de 16 cores -> DAC via registradores de paleta do controlador de atributos */
static inline unsigned ar_color(vga *v, unsigned c)
{
    unsigned p = v->ar[c & 0xf];
    if (v->ar[0x10] & 0x80)
        return (p & 0x0f) | ((v->ar[0x14] & 0x0f) << 4);
    return (p & 0x3f) | ((v->ar[0x14] & 0x0c) << 4);
}

static bool set_size(vga *v, uint32_t w, uint32_t h)
{
    mvm_vm *vm = v->vm;
    if (w == 0 || h == 0 || (uint64_t)w * h * 4 > vm->fb_size)
        return false;
    v->resized = vm->fb_w != w || vm->fb_h != h;
    vm->fb_w = w;
    vm->fb_h = h;
    vm->fb_stride = w * 4;
    return true;
}

static void draw_text(vga *v)
{
    unsigned cols = v->cr[1] + 1u;
    unsigned ch = (v->cr[9] & 0x1f) + 1u;
    unsigned vde = v->cr[0x12] | ((v->cr[7] & 0x02u) << 7) | ((v->cr[7] & 0x40u) << 3);
    unsigned rows = (vde + 1) / ch;
    unsigned cw = (v->sr[1] & 1) ? 8 : 9;
    if (cols > 160) cols = 160;
    if (rows > 100) rows = 100;
    if (!rows || !set_size(v, cols * cw, rows * ch))
        return;
    uint32_t *out = (uint32_t *)v->vm->fb;
    unsigned stride = cols * cw;
    uint32_t start = ((uint32_t)v->cr[0xc] << 8) | v->cr[0xd];
    uint32_t line_words = (uint32_t)v->cr[0x13] * 2;
    uint32_t cursor = ((uint32_t)v->cr[0xe] << 8) | v->cr[0xf];
    unsigned cs = v->cr[0xa] & 0x1f, ce = v->cr[0xb] & 0x1f;
    bool cursor_on = !(v->cr[0xa] & 0x20) && v->blink_on;
    uint8_t sr3 = v->sr[3];
    uint32_t font_a = ((((sr3 >> 5) & 1) | ((sr3 >> 1) & 6)) * 8192u) * 4 + 2;
    uint32_t font_b = ((((sr3 >> 4) & 1) | ((sr3 << 1) & 6)) * 8192u) * 4 + 2;
    bool blink_attr = v->ar[0x10] & 0x08;
    bool line_graphics = v->ar[0x10] & 0x04;
    char *txt = v->text;
    size_t tn = 0;

    for (unsigned r = 0; r < rows; r++) {
        size_t row_start = tn;
        for (unsigned c = 0; c < cols; c++) {
            uint32_t addr = (start + r * line_words + c) & 0xffff;
            uint32_t off = addr * 4;
            uint8_t code = off + 1 < v->vram_size ? v->vram[off] : 0;
            uint8_t attr = off + 1 < v->vram_size ? v->vram[off + 1] : 0;
            unsigned fg = attr & 0xf, bg = attr >> 4;
            if (blink_attr) {
                bg &= 7;
                if ((attr & 0x80) && !v->blink_on)
                    fg = bg;
            }
            uint32_t fgc = dac_rgb(v, ar_color(v, fg)), bgc = dac_rgb(v, ar_color(v, bg));
            uint32_t font = ((attr & 8) ? font_a : font_b) + (uint32_t)code * 32 * 4;
            bool cur = cursor_on && addr == (cursor & 0xffff);
            for (unsigned y = 0; y < ch; y++) {
                uint32_t fo = font + y * 4;
                uint8_t bits = fo < v->vram_size ? v->vram[fo] : 0;
                if (cur && y >= cs && y <= ce)
                    bits = 0xff;
                uint32_t *px = out + (size_t)(r * ch + y) * stride + c * cw;
                for (unsigned x = 0; x < 8; x++)
                    px[x] = (bits & (0x80 >> x)) ? fgc : bgc;
                if (cw == 9)
                    px[8] = (line_graphics && code >= 0xc0 && code <= 0xdf && (bits & 1)) ? fgc : bgc;
            }
            if (tn + 2 < sizeof(v->text) && r < 60 && c < 80)
                txt[tn++] = (code >= 0x20 && code < 0x7f) ? (char)code : (code ? '?' : ' ');
        }
        if (tn + 2 < sizeof(v->text) && r < 60) {
            while (tn > row_start && txt[tn - 1] == ' ')
                tn--;
            txt[tn++] = '\n';
        }
    }
    txt[tn] = 0;
}

/* alguma pagina de [lo, hi) da VRAM foi escrita? (snap NULL: desenho completo) */
static bool range_dirty(const vga *v, const uint64_t *snap, uint64_t lo, uint64_t hi)
{
    if (!snap)
        return true;
    if (hi > v->vram_size)
        hi = v->vram_size;
    if (lo >= hi)
        return false;
    for (uint64_t pg = lo >> 12; pg <= (hi - 1) >> 12; pg++)
        if ((snap[pg >> 6] >> (pg & 63)) & 1)
            return true;
    return false;
}

static inline void row_done(uint32_t y, uint32_t *y0, uint32_t *y1)
{
    if (y < *y0)
        *y0 = y;
    if (y + 1 > *y1)
        *y1 = y + 1;
}

static void draw_vbe(vga *v, const uint64_t *snap, uint32_t *y0, uint32_t *y1)
{
    unsigned w = v->vbe[VBE_IDX_XRES], h = v->vbe[VBE_IDX_YRES], bpp = v->vbe[VBE_IDX_BPP];
    if (!set_size(v, w, h))
        return;
    if (v->resized)
        snap = NULL;
    uint32_t *out = (uint32_t *)v->vm->fb;
    for (unsigned y = 0; y < h; y++) {
        uint64_t lo = v->vbe_start + (uint64_t)y * v->vbe_line;
        uint32_t *d = out + (size_t)y * w;
        const uint8_t *s = v->vram + lo;
        uint64_t need = bpp == 4 ? w / 2 : (uint64_t)w * ((bpp + 7) / 8);
        if (!range_dirty(v, snap, lo, lo + need))
            continue;
        row_done(y, y0, y1);
        if (lo + need > v->vram_size) {
            memset(d, 0, (size_t)w * 4);
            continue;
        }
        switch (bpp) {
        case 32:
            for (unsigned x = 0; x < w; x++) d[x] = (uint32_t)ld_le(s + 4 * x, 4) & 0xffffff;
            break;
        case 24:
            for (unsigned x = 0; x < w; x++) d[x] = (uint32_t)s[3 * x] | ((uint32_t)s[3 * x + 1] << 8) | ((uint32_t)s[3 * x + 2] << 16);
            break;
        case 16:
            for (unsigned x = 0; x < w; x++) {
                uint32_t p = (uint32_t)ld_le(s + 2 * x, 2);
                uint32_t r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
                d[x] = (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
            }
            break;
        case 15:
            for (unsigned x = 0; x < w; x++) {
                uint32_t p = (uint32_t)ld_le(s + 2 * x, 2);
                uint32_t r = (p >> 10) & 31, g = (p >> 5) & 31, b = p & 31;
                d[x] = (((r << 3) | (r >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) | ((b << 3) | (b >> 2));
            }
            break;
        case 8:
            for (unsigned x = 0; x < w; x++) d[x] = dac_rgb(v, s[x]);
            break;
        default: /* 4 bpp empacotado */
            for (unsigned x = 0; x < w; x++) d[x] = dac_rgb(v, (s[x / 2] >> ((x & 1) ? 0 : 4)) & 0xf);
            break;
        }
    }
}

static void draw_graphics(vga *v, const uint64_t *snap, uint32_t *y0, uint32_t *y1)
{
    unsigned shift = (v->gr[5] >> 5) & 3;
    unsigned hde = v->cr[1] + 1u;
    unsigned vde = v->cr[0x12] | ((v->cr[7] & 0x02u) << 7) | ((v->cr[7] & 0x40u) << 3);
    unsigned ms = (v->cr[9] & 0x1f) + 1u;
    if (v->cr[9] & 0x80) ms *= 2;
    unsigned w, h;
    if (shift == 2) w = hde * 4;
    else w = hde * 8;
    if (shift == 1) {
        ms = (v->cr[9] & 0x80) ? 2 : 1;
    }
    h = (vde + 1) / ms;
    if (!set_size(v, w, h))
        return;
    if (v->resized || shift == 1)
        snap = NULL;
    uint32_t *out = (uint32_t *)v->vm->fb;
    uint32_t start = (((uint32_t)v->cr[0xc] << 8) | v->cr[0xd]) * 4;
    uint32_t line = (uint32_t)v->cr[0x13] << 3;
    uint32_t pal16[16];
    for (unsigned i = 0; i < 16; i++)
        pal16[i] = dac_rgb(v, ar_color(v, i));
    uint8_t plane_mask = v->ar[0x12] & 0xf;

    for (unsigned y = 0; y < h; y++) {
        uint32_t *d = out + (size_t)y * w;
        uint32_t la;
        if (shift == 1) /* CGA: linhas pares/impares em bancos de 8K */
            la = start + (y >> 1) * line + ((y & 1) ? 0x2000 * 4 : 0);
        else
            la = start + y * line;
        if (!range_dirty(v, snap, la, la + (shift == 2 ? w : (w / 8) * 4)))
            continue;
        row_done(y, y0, y1);
        for (unsigned x = 0; x < w; x++) {
            uint32_t c;
            if (shift == 2) {
                uint32_t o = la + x;
                c = o < v->vram_size ? dac_rgb(v, v->vram[o]) : 0;
            } else if (shift == 1) {
                uint32_t a = la + (x >> 3) * 4;
                if (a + 2 > v->vram_size) { d[x] = 0; continue; }
                /* 2 bpp: bytes pares/impares nos planos 0/1 */
                uint8_t byte = v->vram[a + ((x >> 2) & 1)];
                unsigned sh = (3 - (x & 3)) * 2;
                c = pal16[(byte >> sh) & 3];
            } else {
                uint32_t a = la + (x >> 3) * 4;
                if (a + 4 > v->vram_size) { d[x] = 0; continue; }
                unsigned bit = 7 - (x & 7), idx = 0;
                for (unsigned p = 0; p < 4; p++)
                    idx |= ((v->vram[a + p] >> bit) & 1u) << p;
                c = pal16[idx & plane_mask];
            }
            d[x] = c;
        }
    }
}

enum { MODE_BLANK, MODE_VBE, MODE_TEXT, MODE_GRAPHICS };

static int cur_mode(vga *v)
{
    if (vbe_on(v))
        return MODE_VBE;
    if (!(v->ar_idx & 0x20))
        return MODE_BLANK; /* PAS = 0: tela apagada; mantem a ultima imagem */
    return (v->gr[6] & 1) ? MODE_GRAPHICS : MODE_TEXT;
}

/* Desenha as linhas cujas paginas de VRAM mudaram (ou tudo, se full). Thread da VM. */
static void render(vga *v, bool full)
{
    mvm_vm *vm = v->vm;
    int mode = cur_mode(v);
    bool any = false;
    for (uint32_t i = 0; i < v->dirty_words; i++) {
        v->snap[i] = __atomic_exchange_n(&v->dirty[i], 0, __ATOMIC_RELAXED);
        any |= v->snap[i] != 0;
    }
    /* as proximas escritas nessas paginas precisam passar de novo pelo caminho lento */
    if (any && vm->cpu_ops && vm->cpu_ops->tlb_protect)
        vm->cpu_ops->tlb_protect(vm->cpu, v->vram, v->vram_size);
    if (mode != v->drawn_mode)
        full = true;
    const uint64_t *snap = full ? NULL : v->snap;
    uint32_t y0 = UINT32_MAX, y1 = 0;
    v->resized = false;
    pthread_mutex_lock(&vm->fb_lock);
    v->text_mode = false;
    switch (mode) {
    case MODE_VBE:
        draw_vbe(v, snap, &y0, &y1);
        break;
    case MODE_TEXT:
        v->text_mode = true;
        draw_text(v);
        y0 = 0;
        y1 = vm->fb_h;
        break;
    case MODE_GRAPHICS:
        draw_graphics(v, snap, &y0, &y1);
        break;
    default:
        break;
    }
    if (y0 < y1)
        vm_fb_rows_changed(vm, y0, y1 > vm->fb_h ? vm->fb_h : y1);
    pthread_mutex_unlock(&vm->fb_lock);
    v->drawn_mode = mode;
}

static bool vram_dirty(vga *v)
{
    for (uint32_t i = 0; i < v->dirty_words; i++)
        if (__atomic_load_n(&v->dirty[i], __ATOMIC_RELAXED))
            return true;
    return false;
}

/* Desenha o que estiver pendente (fim da execucao, VM pausada). Thread da VM. */
static void flush_cb(void *opaque)
{
    vga *v = opaque;
    if (v->regs_dirty || v->drawn_mode != cur_mode(v) || vram_dirty(v)) {
        bool full = v->regs_dirty;
        v->regs_dirty = false;
        v->drawn_gen = v->vram_gen;
        render(v, full);
    }
}

static void refresh_cb(void *opaque)
{
    vga *v = opaque;
    int64_t now = mvm_now(v->vm);
    bool blink_changed = false;
    if (now - v->last_blink >= 266000000LL) {
        v->last_blink = now;
        v->blink_on = !v->blink_on;
        blink_changed = !(v->gr[6] & 1) && !vbe_on(v);
    }
    uint32_t gen = v->vram_gen;
    int mode = cur_mode(v);
    bool full = v->regs_dirty || blink_changed;
    if (full || gen != v->drawn_gen || mode != v->drawn_mode || vram_dirty(v)) {
        /* modo grafico sem ninguem olhando (app em segundo plano, sem VNC): adia o desenho;
         * as paginas continuam marcadas e o modo texto sempre e desenhado (texto da tela) */
        if (mode == MODE_TEXT || mode == MODE_BLANK || vm_fb_watched(v->vm, 500)) {
            v->drawn_gen = gen;
            v->regs_dirty = false;
            render(v, full);
        }
    }
    timer_mod(v->vm, &v->refresh, now + REFRESH_NS);
}

_Atomic uint32_t *vga_vram_gen(vga *v) { return &v->vram_gen; }
uint8_t *vga_vram(vga *v) { return v->vram; }
uint64_t *vga_vram_dirty(vga *v) { return v->dirty; }
uint32_t vga_vram_size(vga *v) { return v->vram_size; }

size_t vga_text(vga *v, char *buf, size_t len)
{
    if (!v->text_mode || !len)
        return 0;
    size_t n = strlen(v->text);
    if (n >= len)
        n = len - 1;
    memcpy(buf, v->text, n);
    buf[n] = 0;
    return n;
}

void vga_reset(vga *v)
{
    v->misc = 0x67; /* cor, RAM habilitada */
    v->fcr = 0;
    memset(v->sr, 0, sizeof(v->sr));
    memset(v->gr, 0, sizeof(v->gr));
    memset(v->ar, 0, sizeof(v->ar));
    memset(v->cr, 0, sizeof(v->cr));
    v->sr[2] = 0x0f;
    v->sr[4] = 0x02;
    v->ar_idx = 0x20;
    v->ar_flip = false;
    v->pel_mask = 0xff;
    v->dac_state = 0;
    v->vbe_idx = 0;
    memset(v->vbe, 0, sizeof(v->vbe));
    v->vbe[VBE_IDX_ID] = VBE_ID;
    v->bank_offset = 0;
    v->regs_dirty = true;
    v->drawn_mode = -1;
    timer_mod(v->vm, &v->refresh, mvm_now(v->vm) + REFRESH_NS);
}

vga *vga_new(mvm_vm *vm, uint32_t vram_mb)
{
    vga *v = calloc(1, sizeof(*v));
    v->vm = vm;
    v->vram_size = vram_mb << 20;
    v->vram = calloc(1, v->vram_size);
    v->dirty_words = (v->vram_size / 4096 + 63) / 64;
    v->dirty = calloc(v->dirty_words, 8);
    v->snap = calloc(v->dirty_words, 8);
    timer_init(&v->refresh, refresh_cb, v);
    vm->fb_flush = flush_cb;
    vm->fb_flush_opaque = v;
    vga_reset(v);
    return v;
}

void vga_free(vga *v)
{
    if (!v)
        return;
    timer_del(v->vm, &v->refresh);
    if (v->vm->fb_flush_opaque == v)
        v->vm->fb_flush = NULL;
    free(v->vram);
    free(v->dirty);
    free(v->snap);
    free(v);
}
