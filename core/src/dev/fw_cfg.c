/*
 * fw_cfg: interface de configuracao de firmware compativel com a do QEMU
 * (seletor 0x510, dados 0x511, DMA 0x514). Usada pelo SeaBIOS para obter
 * tamanho de RAM, mapa e820, ordem de boot, ROMs e tabelas.
 */
#include "devices.h"

#include <stdlib.h>

#define FW_CFG_SIGNATURE 0x00
#define FW_CFG_ID 0x01
#define FW_CFG_FILE_DIR 0x19
#define FW_CFG_FILE_FIRST 0x20
#define FW_CFG_MAX_ITEMS 0x40

#define DMA_CTL_ERROR 0x01
#define DMA_CTL_READ 0x02
#define DMA_CTL_SKIP 0x04
#define DMA_CTL_SELECT 0x08
#define DMA_CTL_WRITE 0x10

typedef struct {
    uint16_t key;
    uint8_t *data;
    uint32_t len;
    char name[56]; /* vazio para chaves legadas */
} fw_item;

struct fw_cfg {
    mvm_vm *vm;
    fw_item items[FW_CFG_MAX_ITEMS];
    int nitems;
    uint16_t next_file_key;
    int cur;          /* item selecionado (-1 = nenhum) */
    uint32_t offset;
    uint64_t dma_addr;
};

static fw_item *find_key(fw_cfg *f, uint16_t key)
{
    for (int i = 0; i < f->nitems; i++)
        if (f->items[i].key == key)
            return &f->items[i];
    return NULL;
}

static fw_item *add_item(fw_cfg *f, uint16_t key, const void *data, uint32_t len)
{
    fw_item *it = find_key(f, key);
    if (!it) {
        if (f->nitems >= FW_CFG_MAX_ITEMS) {
            LOGW("fw_cfg: itens demais");
            return NULL;
        }
        it = &f->items[f->nitems++];
        memset(it, 0, sizeof(*it));
        it->key = key;
    }
    free(it->data);
    it->data = malloc(len ? len : 1);
    if (len)
        memcpy(it->data, data, len);
    it->len = len;
    return it;
}

static void rebuild_dir(fw_cfg *f)
{
    int n = 0;
    for (int i = 0; i < f->nitems; i++)
        if (f->items[i].name[0])
            n++;
    uint32_t len = 4 + 64 * (uint32_t)n;
    uint8_t *d = calloc(1, len);
    uint32_t be = bswap32((uint32_t)n);
    memcpy(d, &be, 4);
    int k = 0;
    for (int i = 0; i < f->nitems; i++) {
        fw_item *it = &f->items[i];
        if (!it->name[0])
            continue;
        uint8_t *e = d + 4 + 64 * k++;
        uint32_t sz = bswap32(it->len);
        uint16_t sel = (uint16_t)((it->key >> 8) | (it->key << 8));
        memcpy(e, &sz, 4);
        memcpy(e + 4, &sel, 2);
        memcpy(e + 8, it->name, 56);
    }
    add_item(f, FW_CFG_FILE_DIR, d, len);
    free(d);
}

void fw_cfg_add_bytes(fw_cfg *f, uint16_t key, const void *data, uint32_t len) { add_item(f, key, data, len); }

void fw_cfg_add_u32(fw_cfg *f, uint16_t key, uint32_t v) { add_item(f, key, &v, 4); }

void fw_cfg_add_u64(fw_cfg *f, uint16_t key, uint64_t v) { add_item(f, key, &v, 8); }

void fw_cfg_add_file(fw_cfg *f, const char *name, const void *data, uint32_t len)
{
    /* substitui arquivo existente com o mesmo nome */
    for (int i = 0; i < f->nitems; i++) {
        if (f->items[i].name[0] && !strncmp(f->items[i].name, name, 55)) {
            add_item(f, f->items[i].key, data, len);
            return;
        }
    }
    fw_item *it = add_item(f, f->next_file_key++, data, len);
    if (!it)
        return;
    snprintf(it->name, sizeof(it->name), "%s", name);
    rebuild_dir(f);
}

fw_cfg *fw_cfg_new(mvm_vm *vm)
{
    fw_cfg *f = calloc(1, sizeof(*f));
    f->vm = vm;
    f->cur = -1;
    f->next_file_key = FW_CFG_FILE_FIRST;
    add_item(f, FW_CFG_SIGNATURE, "QEMU", 4);
    fw_cfg_add_u32(f, FW_CFG_ID, 3); /* interface tradicional + DMA */
    rebuild_dir(f);
    return f;
}

void fw_cfg_free(fw_cfg *f)
{
    if (!f)
        return;
    for (int i = 0; i < f->nitems; i++)
        free(f->items[i].data);
    free(f);
}

static void select_key(fw_cfg *f, uint16_t key)
{
    f->offset = 0;
    f->cur = -1;
    for (int i = 0; i < f->nitems; i++)
        if (f->items[i].key == key) {
            f->cur = i;
            break;
        }
}

static uint8_t read_byte(fw_cfg *f)
{
    if (f->cur < 0)
        return 0;
    fw_item *it = &f->items[f->cur];
    return f->offset < it->len ? it->data[f->offset++] : 0;
}

static void do_dma(fw_cfg *f)
{
    mvm_space *mem = &f->vm->mem;
    uint64_t a = f->dma_addr;
    uint8_t hdr[16];
    space_memread(mem, a, hdr, 16);
    uint32_t ctl = bswap32((uint32_t)ld_le(hdr, 4));
    uint32_t len = bswap32((uint32_t)ld_le(hdr + 4, 4));
    uint64_t addr = bswap64(ld_le(hdr + 8, 8));
    if (ctl & DMA_CTL_SELECT)
        select_key(f, (uint16_t)(ctl >> 16));
    uint32_t status = 0;
    if (ctl & DMA_CTL_WRITE) {
        status = DMA_CTL_ERROR; /* escrita nao suportada */
    } else if (ctl & (DMA_CTL_READ | DMA_CTL_SKIP)) {
        fw_item *it = f->cur >= 0 ? &f->items[f->cur] : NULL;
        uint32_t avail = it && f->offset < it->len ? it->len - f->offset : 0;
        uint32_t n = len < avail ? len : avail;
        if (ctl & DMA_CTL_READ) {
            if (n)
                space_memwrite(mem, addr, it->data + f->offset, n);
            /* o restante e preenchido com zeros */
            static const uint8_t zero[256];
            for (uint32_t z = n; z < len;) {
                uint32_t c = len - z > sizeof(zero) ? (uint32_t)sizeof(zero) : len - z;
                space_memwrite(mem, addr + z, zero, c);
                z += c;
            }
        }
        f->offset += n;
    }
    uint32_t out = bswap32(status);
    space_memwrite(mem, a, &out, 4);
}

static uint64_t fwcfg_read(void *opaque, uint64_t off, unsigned size)
{
    fw_cfg *f = opaque;
    if (off == 1) {
        uint64_t v = 0;
        for (unsigned i = 0; i < size; i++)
            v |= (uint64_t)read_byte(f) << (8 * i);
        return v;
    }
    if (off >= 4 && off < 12) { /* registrador de DMA: le a assinatura "QEMU CFG" */
        static const char sig[8] = {'Q', 'E', 'M', 'U', ' ', 'C', 'F', 'G'};
        uint64_t v = 0;
        for (unsigned i = 0; i < size && off - 4 + i < 8; i++)
            v |= (uint64_t)(uint8_t)sig[off - 4 + i] << (8 * i);
        return v;
    }
    return 0;
}

static void fwcfg_write(void *opaque, uint64_t off, uint64_t v, unsigned size)
{
    fw_cfg *f = opaque;
    if (off == 0 && size >= 2) {
        select_key(f, (uint16_t)v);
    } else if (off == 4 && size == 4) {
        f->dma_addr = (f->dma_addr & 0xffffffffULL) | ((uint64_t)bswap32((uint32_t)v) << 32);
    } else if (off == 8 && size == 4) {
        f->dma_addr = (f->dma_addr & ~0xffffffffULL) | bswap32((uint32_t)v);
        do_dma(f);
        f->dma_addr = 0;
    } else if (off == 4 && size == 8) {
        f->dma_addr = bswap64(v);
        do_dma(f);
        f->dma_addr = 0;
    }
}

const mvm_io_ops fw_cfg_ops = {fwcfg_read, fwcfg_write};
