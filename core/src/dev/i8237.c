/*
 * Controladores de DMA 8237 do PC/AT: DMA1 (canais 0-3, 8 bits, portas 0x00-0x0f)
 * e DMA2 (canais 4-7, 16 bits, portas 0xc0-0xdf), mais os registradores de
 * pagina (0x81-0x8f). Hoje so o controlador de disquete usa (canal 2).
 *
 * As transferencias sao feitas de uma vez pelo dispositivo (i8237_write_mem /
 * i8237_read_mem): o contador avanca, o bit de TC fica no status e, com
 * auto-inicializacao, os registradores voltam aos valores base.
 */
#include "devices.h"

#include <stdlib.h>

typedef struct {
    uint16_t base_addr, base_count;
    uint16_t addr, count;
    uint8_t mode;
    uint8_t page;
} dma_chan;

typedef struct {
    dma_chan ch[4];
    bool flipflop;
    uint8_t mask;    /* bit n: canal n mascarado */
    uint8_t status;  /* bits 0-3: TC; 4-7: pedidos */
    uint8_t command;
    uint8_t temp;
} dma_ctrl;

struct i8237 {
    mvm_vm *vm;
    dma_ctrl c[2];
};

/* registrador de pagina (offset a partir de 0x80) de cada canal */
static const int8_t page_chan[16] = {-1, 2, 3, 1, -1, -1, -1, 0, -1, 6, 7, 5, -1, -1, -1, 4};

static void ctrl_reset(dma_ctrl *c)
{
    c->flipflop = false;
    c->mask = 0x0f;
    c->status = 0;
    c->command = 0;
    c->temp = 0;
}

void i8237_reset(i8237 *d)
{
    for (int i = 0; i < 2; i++)
        ctrl_reset(&d->c[i]);
}

i8237 *i8237_new(mvm_vm *vm)
{
    i8237 *d = calloc(1, sizeof(*d));
    d->vm = vm;
    i8237_reset(d);
    return d;
}

static uint64_t ctrl_read(dma_ctrl *c, unsigned reg)
{
    if (reg < 8) {
        dma_chan *ch = &c->ch[reg >> 1];
        uint16_t v = (reg & 1) ? ch->count : ch->addr;
        uint8_t r = c->flipflop ? (uint8_t)(v >> 8) : (uint8_t)v;
        c->flipflop = !c->flipflop;
        return r;
    }
    switch (reg) {
    case 8: { /* status: ler limpa os bits de TC */
        uint8_t s = c->status;
        c->status &= 0xf0;
        return s;
    }
    case 13: return c->temp;
    case 15: return c->mask | 0xf0;
    default: return 0xff;
    }
}

static void ctrl_write(dma_ctrl *c, unsigned reg, uint8_t v)
{
    if (reg < 8) {
        dma_chan *ch = &c->ch[reg >> 1];
        uint16_t *base = (reg & 1) ? &ch->base_count : &ch->base_addr;
        uint16_t *cur = (reg & 1) ? &ch->count : &ch->addr;
        if (c->flipflop)
            *base = (uint16_t)((*base & 0x00ff) | (v << 8));
        else
            *base = (uint16_t)((*base & 0xff00) | v);
        *cur = *base;
        c->flipflop = !c->flipflop;
        return;
    }
    switch (reg) {
    case 8: c->command = v; break;
    case 9: /* pedido por software */
        if (v & 4) c->status |= (uint8_t)(0x10 << (v & 3));
        else c->status &= (uint8_t)~(0x10 << (v & 3));
        break;
    case 10: /* mascara de um canal */
        if (v & 4) c->mask |= (uint8_t)(1 << (v & 3));
        else c->mask &= (uint8_t)~(1 << (v & 3));
        break;
    case 11: c->ch[v & 3].mode = v; break;
    case 12: c->flipflop = false; break;
    case 13: ctrl_reset(c); break; /* master clear */
    case 14: c->mask = 0; break;
    case 15: c->mask = v & 0x0f; break;
    }
}

static uint64_t dma1_read(void *opaque, uint64_t off, unsigned size)
{
    UNUSED(size);
    return ctrl_read(&((i8237 *)opaque)->c[0], (unsigned)off & 15);
}

static void dma1_write(void *opaque, uint64_t off, uint64_t val, unsigned size)
{
    UNUSED(size);
    ctrl_write(&((i8237 *)opaque)->c[0], (unsigned)off & 15, (uint8_t)val);
}

/* DMA2: registradores em portas pares (0xc0, 0xc2, ...) */
static uint64_t dma2_read(void *opaque, uint64_t off, unsigned size)
{
    UNUSED(size);
    if (off & 1)
        return 0xff;
    return ctrl_read(&((i8237 *)opaque)->c[1], (unsigned)(off >> 1) & 15);
}

static void dma2_write(void *opaque, uint64_t off, uint64_t val, unsigned size)
{
    UNUSED(size);
    if (!(off & 1))
        ctrl_write(&((i8237 *)opaque)->c[1], (unsigned)(off >> 1) & 15, (uint8_t)val);
}

/* paginas: offset 0 = porta 0x80 (fica com o POST; aqui so 0x81-0x8f) */
static uint64_t page_read(void *opaque, uint64_t off, unsigned size)
{
    UNUSED(size);
    i8237 *d = opaque;
    int ch = page_chan[(off + 1) & 15];
    return ch < 0 ? 0xff : d->c[ch >> 2].ch[ch & 3].page;
}

static void page_write(void *opaque, uint64_t off, uint64_t val, unsigned size)
{
    UNUSED(size);
    i8237 *d = opaque;
    int ch = page_chan[(off + 1) & 15];
    if (ch >= 0)
        d->c[ch >> 2].ch[ch & 3].page = (uint8_t)val;
}

const mvm_io_ops i8237_dma1_ops = {dma1_read, dma1_write};
const mvm_io_ops i8237_dma2_ops = {dma2_read, dma2_write};
const mvm_io_ops i8237_page_ops = {page_read, page_write};

bool i8237_ready(i8237 *d, int ch)
{
    return !(d->c[ch >> 2].mask & (1 << (ch & 3)));
}

int i8237_mode(i8237 *d, int ch)
{
    return d->c[ch >> 2].ch[ch & 3].mode;
}

bool i8237_tc(i8237 *d, int ch)
{
    return d->c[ch >> 2].status & (1 << (ch & 3));
}

/* Transfere ate len bytes entre buf e a RAM; devolve quantos foram. */
static uint32_t transfer(i8237 *d, int ch, uint8_t *buf, uint32_t len, bool to_mem)
{
    dma_ctrl *c = &d->c[ch >> 2];
    dma_chan *k = &c->ch[ch & 3];
    bool wide = ch >= 4;
    bool down = k->mode & 0x20;
    uint32_t unit = wide ? 2 : 1;
    uint32_t done = 0;
    mvm_space *mem = &d->vm->mem;

    if (!i8237_ready(d, ch))
        return 0;
    c->status &= (uint8_t)~(1 << (ch & 3));
    while (done + unit <= len) {
        uint64_t pa = wide ? ((uint64_t)(k->page & 0xfe) << 16) | ((uint64_t)k->addr << 1)
                           : ((uint64_t)k->page << 16) | k->addr;
        /* trecho continuo ate o fim do contador ou a volta do endereco de 16 bits */
        uint32_t units = (uint32_t)k->count + 1;
        if (!down && units > 0x10000u - k->addr)
            units = 0x10000u - k->addr;
        if (down)
            units = 1;
        if (units > (len - done) / unit)
            units = (len - done) / unit;
        uint32_t bytes = units * unit;
        if (to_mem)
            space_memwrite(mem, pa, buf + done, bytes);
        else
            space_memread(mem, pa, buf + done, bytes);
        done += bytes;
        k->addr = (uint16_t)(down ? k->addr - units : k->addr + units);
        bool tc = k->count < units;
        k->count = (uint16_t)(k->count - units);
        if (tc || k->count == 0xffff) {
            c->status |= (uint8_t)(1 << (ch & 3));
            if (k->mode & 0x10) { /* auto-inicializacao */
                k->addr = k->base_addr;
                k->count = k->base_count;
            } else {
                c->mask |= (uint8_t)(1 << (ch & 3));
            }
            break;
        }
    }
    return done;
}

uint32_t i8237_write_mem(i8237 *d, int ch, const uint8_t *buf, uint32_t len)
{
    return transfer(d, ch, (uint8_t *)buf, len, true);
}

uint32_t i8237_read_mem(i8237 *d, int ch, uint8_t *buf, uint32_t len)
{
    return transfer(d, ch, buf, len, false);
}
