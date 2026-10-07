/* Barramento PCI (mecanismo de configuracao #1) e transporte virtio-pci legado. */
#include "pci.h"

#include <stdlib.h>

/* ---------------------------------------------------------------- bus */

static uint32_t cfg_rd(pci_dev *d, unsigned off, unsigned size)
{
    return (uint32_t)ld_le(d->cfg + off, size);
}

static void bar_unmap(pci_dev *d, int i)
{
    if (!d->bar_mapped[i])
        return;
    mvm_space *sp = d->bar_io[i] ? &d->bus->vm->io : &d->bus->vm->mem;
    space_remove_at(sp, d->bar_base[i]);
    d->bar_mapped[i] = false;
}

static void bar_update(pci_dev *d, int i)
{
    if (!d->bar_size[i])
        return;
    uint32_t raw = (uint32_t)ld_le(d->cfg + 0x10 + 4 * i, 4);
    uint16_t cmd = (uint16_t)ld_le(d->cfg + 4, 2);
    uint64_t base = d->bar_io[i] ? (raw & ~3u) : (raw & ~15u);
    bool enabled = d->bar_io[i] ? (cmd & 1) : (cmd & 2);
    uint64_t limit = d->bar_io[i] ? 0x10000 : 0x100000000ULL;
    bool valid = enabled && base && base + d->bar_size[i] <= limit && base != (limit - d->bar_size[i]) &&
                 (raw | (d->bar_size[i] - 1) | (d->bar_io[i] ? 3u : 15u)) != 0xffffffffu;
    if (d->bar_mapped[i] && (!valid || base != d->bar_base[i]))
        bar_unmap(d, i);
    if (valid && !d->bar_mapped[i]) {
        mvm_space *sp = d->bar_io[i] ? &d->bus->vm->io : &d->bus->vm->mem;
        if (space_find(sp, base) || space_find(sp, base + d->bar_size[i] - 1)) {
            LOGW("pci: BAR%d de %02x.%x em 0x%llx conflita com outra regiao", i, d->slot, d->fn,
                 (unsigned long long)base);
            return;
        }
        if (d->bar_host[i]) {
            mvm_region *r = space_add_ram(sp, base, d->bar_size[i], d->bar_host[i], false, "pci-bar-ram");
            if (r)
                r->dirty_gen = d->bar_gen[i];
        } else {
            space_add_io(sp, base, d->bar_size[i], d->bar_ops[i], d->bar_opaque[i], "pci-bar");
        }
        d->bar_base[i] = base;
        d->bar_mapped[i] = true;
    }
}

static void rom_update(pci_dev *d)
{
    if (!d->rom_size)
        return;
    uint32_t raw = (uint32_t)ld_le(d->cfg + 0x30, 4);
    uint16_t cmd = (uint16_t)ld_le(d->cfg + 4, 2);
    uint64_t base = raw & ~(d->rom_size - 1) & ~1u;
    bool valid = (raw & 1) && (cmd & 2) && base && base + d->rom_size <= 0x100000000ULL &&
                 base != 0x100000000ULL - d->rom_size;
    if (d->rom_mapped && (!valid || base != d->rom_base)) {
        space_remove_at(&d->bus->vm->mem, d->rom_base);
        d->rom_mapped = false;
    }
    if (valid && !d->rom_mapped) {
        mvm_space *sp = &d->bus->vm->mem;
        if (space_find(sp, base) || space_find(sp, base + d->rom_size - 1)) {
            LOGW("pci: ROM de %02x.%x em 0x%llx conflita com outra regiao", d->slot, d->fn, (unsigned long long)base);
            return;
        }
        space_add_ram(sp, base, d->rom_size, d->rom, true, "pci-rom");
        d->rom_base = base;
        d->rom_mapped = true;
    }
}

void pci_set_rom(pci_dev *d, const uint8_t *data, size_t len)
{
    uint32_t sz = 0x800;
    while (sz < len)
        sz <<= 1;
    free(d->rom);
    d->rom = calloc(1, sz);
    memcpy(d->rom, data, len);
    d->rom_size = sz;
}

static void cfg_wr(pci_dev *d, unsigned off, uint32_t val, unsigned size)
{
    for (unsigned b = 0; b < size; b++) {
        unsigned o = off + b;
        uint8_t v = (uint8_t)(val >> (8 * b));
        if (o < 4 || (o >= 0x08 && o < 0x0c) || o == 0x0e || (o >= 0x2c && o < 0x30) || o == 0x3d ||
            (o >= 0x34 && o < 0x3c))
            continue; /* somente leitura */
        if (o >= 0x10 && o < 0x28) {
            int i = (int)(o - 0x10) / 4;
            unsigned bo = (o - 0x10) % 4;
            uint32_t mask = d->bar_size[i] ? ~(d->bar_size[i] - 1) : 0;
            uint8_t m = (uint8_t)(mask >> (8 * bo));
            uint8_t ro = bo == 0 ? (d->bar_io[i] ? 0x03 : 0x0f) : 0;
            d->cfg[o] = (uint8_t)((d->cfg[o] & (~m | ro)) | (v & m & ~ro));
            continue;
        }
        if (o >= 0x30 && o < 0x34) {
            uint32_t mask = d->rom_size ? (~(d->rom_size - 1) | 1u) : 0;
            uint8_t m = (uint8_t)(mask >> (8 * (o - 0x30)));
            d->cfg[o] = (uint8_t)((d->cfg[o] & ~m) | (v & m));
            continue;
        }
        if (o == 0x06 || o == 0x07) { /* status: RW1C */
            d->cfg[o] &= (uint8_t)~v;
            continue;
        }
        d->cfg[o] = v;
    }
    if (off < 0x28 && off + size > 0x04) {
        for (int i = 0; i < 6; i++)
            bar_update(d, i);
    }
    if ((off < 0x34 && off + size > 0x30) || (off < 0x06 && off + size > 0x04))
        rom_update(d);
    if (d->cfg_write)
        d->cfg_write(d, off, val, size);
}

static pci_dev *find(pci_bus *b, uint32_t addr)
{
    unsigned bus = (addr >> 16) & 0xff, dev = (addr >> 11) & 31, fn = (addr >> 8) & 7;
    if (bus != 0)
        return NULL;
    for (int i = 0; i < b->ndev; i++)
        if ((unsigned)b->dev[i]->slot == dev && (unsigned)b->dev[i]->fn == fn)
            return b->dev[i];
    return NULL;
}

static uint64_t pci_io_read(void *opaque, uint64_t off, unsigned size)
{
    pci_bus *b = opaque;
    if (off < 4) {
        if (off == 0 && size == 4)
            return b->addr;
        return 0xffffffffu;
    }
    if (!(b->addr & 0x80000000u))
        return size == 4 ? 0xffffffffu : (1u << (8 * size)) - 1;
    pci_dev *d = find(b, b->addr);
    unsigned reg = (b->addr & 0xfc) + (unsigned)(off - 4);
    if (!d || reg + size > 256)
        return size == 4 ? 0xffffffffu : (1u << (8 * size)) - 1;
    return cfg_rd(d, reg, size);
}

static void pci_io_write(void *opaque, uint64_t off, uint64_t v, unsigned size)
{
    pci_bus *b = opaque;
    if (off < 4) {
        if (off == 0 && size == 4)
            b->addr = (uint32_t)v;
        return;
    }
    if (!(b->addr & 0x80000000u))
        return;
    pci_dev *d = find(b, b->addr);
    unsigned reg = (b->addr & 0xfc) + (unsigned)(off - 4);
    if (!d || reg + size > 256)
        return;
    cfg_wr(d, reg, (uint32_t)v, size);
}

const mvm_io_ops pci_host_ops = {pci_io_read, pci_io_write};

pci_bus *pci_bus_new(mvm_vm *vm)
{
    pci_bus *b = calloc(1, sizeof(*b));
    b->vm = vm;
    return b;
}

void pci_bus_free(pci_bus *b)
{
    if (!b)
        return;
    for (int i = 0; i < b->ndev; i++) {
        free(b->dev[i]->rom);
        free(b->dev[i]);
    }
    free(b);
}

pci_dev *pci_add(pci_bus *b, int slot, uint16_t vendor, uint16_t device, uint32_t class_rev)
{
    return pci_add_fn(b, slot, 0, vendor, device, class_rev);
}

pci_dev *pci_add_fn(pci_bus *b, int slot, int fn, uint16_t vendor, uint16_t device, uint32_t class_rev)
{
    if (b->ndev >= PCI_MAX_DEV)
        return NULL;
    pci_dev *d = calloc(1, sizeof(*d));
    d->bus = b;
    d->slot = slot;
    d->fn = fn;
    st_le(d->cfg + 0, vendor, 2);
    st_le(d->cfg + 2, device, 2);
    st_le(d->cfg + 8, class_rev, 4);
    b->dev[b->ndev++] = d;
    return d;
}

void pci_set_bar(pci_dev *d, int i, uint32_t size, bool io, const mvm_io_ops *ops, void *opaque, uint32_t addr)
{
    d->bar_size[i] = size;
    d->bar_io[i] = io;
    d->bar_ops[i] = ops;
    d->bar_opaque[i] = opaque;
    st_le(d->cfg + 0x10 + 4 * i, (addr & ~(size - 1)) | (io ? 1 : 0), 4);
}

void pci_set_ram_bar(pci_dev *d, int i, uint32_t size, uint8_t *host, _Atomic uint32_t *gen, bool prefetch, uint32_t addr)
{
    d->bar_size[i] = size;
    d->bar_io[i] = false;
    d->bar_host[i] = host;
    d->bar_gen[i] = gen;
    st_le(d->cfg + 0x10 + 4 * i, (addr & ~(size - 1)) | (prefetch ? 8 : 0), 4);
}

void pci_reset_dev(pci_dev *d)
{
    for (int i = 0; i < 6; i++)
        bar_unmap(d, i);
    if (d->rom_mapped) {
        space_remove_at(&d->bus->vm->mem, d->rom_base);
        d->rom_mapped = false;
    }
    /* todo o espaco de configuracao volta ao estado inicial (ex.: PAM do i440FX,
     * que o SeaBIOS consulta para saber se precisa recopiar a si mesmo) */
    memcpy(d->cfg, d->cfg_init, sizeof(d->cfg));
    st_le(d->cfg + 0x30, 0, 4);
    st_le(d->cfg + 4, d->default_cmd, 2);
    for (int i = 0; i < 6; i++)
        if (d->bar_size[i])
            st_le(d->cfg + 0x10 + 4 * i, d->default_bar[i], 4);
    for (int i = 0; i < 6; i++)
        bar_update(d, i);
}

void pci_finalize(pci_dev *d)
{
    memcpy(d->cfg_init, d->cfg, sizeof(d->cfg));
    d->default_cmd = (uint16_t)ld_le(d->cfg + 4, 2);
    for (int i = 0; i < 6; i++) {
        d->default_bar[i] = (uint32_t)ld_le(d->cfg + 0x10 + 4 * i, 4);
        bar_update(d, i);
    }
}

/* ------------------------------------------------- virtio-pci legado */

typedef struct {
    virtio_dev *v;
    pci_dev *pci;
    uint16_t qsel;
} vpci;

static uint64_t vpci_read(void *opaque, uint64_t off, unsigned size)
{
    vpci *p = opaque;
    virtio_dev *d = p->v;
    if (off >= 20)
        return d->cfg_read ? d->cfg_read(d, (uint32_t)(off - 20), size) : 0;
    virtq *q = p->qsel < VIRTIO_MAX_QUEUES ? &d->vq[p->qsel] : NULL;
    switch (off) {
    case 0: return (uint32_t)d->host_features;
    case 4: return (uint32_t)d->guest_features;
    case 8: return q ? (uint32_t)(q->desc >> 12) : 0;
    case 12: return (q && p->qsel < d->nvq) ? q->num_max : 0;
    case 14: return p->qsel;
    case 16: return 0;
    case 18: return d->status;
    case 19: {
        uint32_t v = d->isr;
        d->isr = 0;
        virtio_update_irq(d);
        return v;
    }
    default: return 0;
    }
}

static void vpci_write(void *opaque, uint64_t off, uint64_t val, unsigned size)
{
    vpci *p = opaque;
    virtio_dev *d = p->v;
    if (off >= 20) {
        if (d->cfg_write)
            d->cfg_write(d, (uint32_t)(off - 20), (uint32_t)val, size);
        return;
    }
    virtq *q = p->qsel < VIRTIO_MAX_QUEUES ? &d->vq[p->qsel] : NULL;
    switch (off) {
    case 4: d->guest_features = (uint32_t)val; break;
    case 8:
        if (!q) break;
        if (!val) {
            q->ready = false;
            q->desc = q->avail = q->used = 0;
            q->last_avail = 0;
        } else {
            q->num = q->num_max;
            q->desc = (uint64_t)(uint32_t)val << 12;
            q->avail = q->desc + 16ULL * q->num;
            q->used = (q->avail + 6 + 2ULL * q->num + 4095) & ~4095ULL;
            q->last_avail = 0;
            q->ready = true;
        }
        break;
    case 14: p->qsel = (uint16_t)val; break;
    case 16:
        if (val < (uint64_t)d->nvq && d->notify)
            d->notify(d, (int)val);
        break;
    case 18:
        if (!(val & 0xff))
            virtio_reset(d);
        else
            d->status = (uint32_t)val & 0xff;
        break;
    default: break;
    }
}

static const mvm_io_ops vpci_ops = {vpci_read, vpci_write};

pci_dev *virtio_pci_add(pci_bus *b, int slot, virtio_dev *v, int irq, uint16_t io_base)
{
    static const uint16_t legacy_id[] = {0, 0x1000, 0x1001, 0x1003, 0x1004, 0x1005, 0, 0, 0x1009};
    uint16_t devid = v->device_id < ARRAY_SIZE(legacy_id) && legacy_id[v->device_id] ? legacy_id[v->device_id]
                                                                                      : (uint16_t)(0x1040 + v->device_id);
    uint32_t cls = v->device_id == 2 ? 0x01000000 : v->device_id == 1 ? 0x02000000 : 0xff000000;
    pci_dev *d = pci_add(b, slot, 0x1af4, devid, cls);
    vpci *p = calloc(1, sizeof(*p));
    p->v = v;
    p->pci = d;
    d->priv = p;
    st_le(d->cfg + 0x2c, 0x1af4, 2);
    st_le(d->cfg + 0x2e, v->device_id, 2);
    d->cfg[0x3c] = (uint8_t)irq;
    d->cfg[0x3d] = 1;
    st_le(d->cfg + 4, 1, 2); /* IO habilitado */
    pci_set_bar(d, 0, 0x40, true, &vpci_ops, p, io_base);
    pci_finalize(d);
    return d;
}

void virtio_pci_free(pci_dev *d)
{
    if (d)
        free(d->priv);
}
