/*
 * Controlador IDE do PIIX3 (PCI 8086:7010): dois canais legados (0x1F0/0x3F6
 * IRQ 14 e 0x170/0x376 IRQ 15), discos ATA (PIO, multi-setor, LBA28/LBA48,
 * DMA via bus master) e CD-ROM ATAPI (comandos de pacote, setores de 2048).
 */
#include "ide.h"

#include <stdlib.h>

#define ST_ERR 0x01
#define ST_DRQ 0x08
#define ST_DSC 0x10
#define ST_DF 0x20
#define ST_DRDY 0x40
#define ST_BSY 0x80
#define ERR_ABRT 0x04

#define BUF_SECTORS 128
#define BUF_SIZE (BUF_SECTORS * 512)

enum { XFER_NONE, XFER_PIO_IN, XFER_PIO_OUT, XFER_PACKET };
enum { KIND_NONE, KIND_ATA, KIND_ATAPI };
enum { OP_NONE, OP_READ, OP_WRITE, OP_ATAPI_READ, OP_ATAPI_DATA, OP_TRIM };

typedef struct ide_bus ide_bus;

typedef struct {
    ide_bus *bus;
    int unit;
    int kind;
    mvm_blk *blk;
    uint64_t sectors;       /* em unidades de 512 (ATA) ou 2048 (ATAPI) */
    uint8_t feature, nsector, sector, lcyl, hcyl, select, status, error;
    uint8_t hob_feature, hob_nsector, hob_sector, hob_lcyl, hob_hcyl;
    uint8_t mult;           /* setores por bloco em READ/WRITE MULTIPLE */
    uint8_t *buf;
    uint32_t pos, end;      /* janela de PIO atual no buffer */
    int xfer;
    uint8_t xfer_mode; /* modo definido por SET FEATURES 0x03 (0x40|n = UDMA n, 0x20|n = MWDMA n) */
    /* operacao em andamento */
    int op;
    bool dma;
    uint64_t lba;
    uint32_t remaining;     /* setores restantes */
    uint32_t block;         /* setores por bloco de DRQ */
    /* ATAPI */
    uint8_t sense_key, asc;
    uint32_t atapi_len;     /* tamanho da resposta (OP_ATAPI_DATA) */
    uint32_t chunk_left;    /* bytes restantes no trecho de DRQ atual (ATAPI PIO) */
    uint32_t byte_limit;
    bool unit_attention;    /* midia trocada: o proximo comando avisa (6/28) */
    bool media_event;       /* evento de midia para GET EVENT STATUS NOTIFICATION */
} ide_drive;

struct ide_bus {
    struct ide_ctrl *ctrl;
    int index;
    ide_drive drv[2];
    int cur;
    uint8_t devctl;
    irq_line irq;
    bool irq_pending;
    uint8_t bm_cmd, bm_status;
    uint32_t bm_prdt;
};

struct ide_ctrl {
    mvm_vm *vm;
    ide_bus bus[2];
};

static void update_irq(ide_bus *b) { irq_set(&b->irq, b->irq_pending && !(b->devctl & 2)); }

static void raise_irq(ide_bus *b)
{
    b->irq_pending = true;
    b->bm_status |= 4;
    update_irq(b);
}

static void lower_irq(ide_bus *b)
{
    b->irq_pending = false;
    update_irq(b);
}

static void set_signature(ide_drive *d)
{
    d->nsector = 1;
    d->sector = 1;
    if (d->kind == KIND_ATAPI) {
        d->lcyl = 0x14;
        d->hcyl = 0xeb;
    } else {
        d->lcyl = 0;
        d->hcyl = 0;
    }
    d->select = (uint8_t)(0xa0 | (d->unit << 4));
}

static void drive_reset(ide_drive *d)
{
    d->xfer = XFER_NONE;
    d->op = OP_NONE;
    d->dma = false;
    d->mult = 16;
    d->error = 1;
    d->status = d->kind == KIND_ATA ? (ST_DRDY | ST_DSC) : 0;
    d->sense_key = 0;
    d->asc = 0;
    set_signature(d);
}

static void put_string(uint16_t *w, int first, int words, const char *s)
{
    char tmp[64];
    memset(tmp, ' ', sizeof(tmp));
    size_t n = strlen(s);
    memcpy(tmp, s, n > (size_t)words * 2 ? (size_t)words * 2 : n);
    for (int i = 0; i < words; i++)
        w[first + i] = (uint16_t)(((uint8_t)tmp[2 * i] << 8) | (uint8_t)tmp[2 * i + 1]);
}

/* ------------------------------------------------------------- PIO */

static void pio_start(ide_drive *d, int dir, uint32_t len)
{
    d->xfer = dir;
    d->pos = 0;
    d->end = len;
    d->status = ST_DRDY | ST_DSC | ST_DRQ;
}

static void cmd_done(ide_drive *d)
{
    d->xfer = XFER_NONE;
    d->op = OP_NONE;
    d->status = ST_DRDY | ST_DSC;
    raise_irq(d->bus);
}

static void cmd_abort(ide_drive *d)
{
    d->xfer = XFER_NONE;
    d->op = OP_NONE;
    d->error = ERR_ABRT;
    d->status = ST_DRDY | ST_ERR;
    raise_irq(d->bus);
}

/* ---------------------------------------------------------- ATA disco */

static uint64_t get_lba(ide_drive *d, bool lba48)
{
    if (lba48)
        return ((uint64_t)d->hob_hcyl << 40) | ((uint64_t)d->hob_lcyl << 32) | ((uint64_t)d->hob_sector << 24) |
               ((uint64_t)d->hcyl << 16) | ((uint64_t)d->lcyl << 8) | d->sector;
    if (d->select & 0x40)
        return ((uint64_t)(d->select & 0x0f) << 24) | ((uint64_t)d->hcyl << 16) | ((uint64_t)d->lcyl << 8) | d->sector;
    /* CHS: 16 cabecas, 63 setores por trilha */
    uint32_t cyl = ((uint32_t)d->hcyl << 8) | d->lcyl, head = d->select & 0x0f;
    if (!d->sector)
        return UINT64_MAX;
    return ((uint64_t)cyl * 16 + head) * 63 + d->sector - 1u;
}

static void set_lba(ide_drive *d, uint64_t lba, bool lba48)
{
    if (lba48) {
        d->sector = (uint8_t)lba;
        d->lcyl = (uint8_t)(lba >> 8);
        d->hcyl = (uint8_t)(lba >> 16);
        d->hob_sector = (uint8_t)(lba >> 24);
        d->hob_lcyl = (uint8_t)(lba >> 32);
        d->hob_hcyl = (uint8_t)(lba >> 40);
    } else if (d->select & 0x40) {
        d->sector = (uint8_t)lba;
        d->lcyl = (uint8_t)(lba >> 8);
        d->hcyl = (uint8_t)(lba >> 16);
        d->select = (uint8_t)((d->select & 0xf0) | ((lba >> 24) & 0x0f));
    }
}

/* marca nas palavras 63 (MWDMA) e 88 (UDMA) o modo selecionado */
static void apply_xfer_mode(ide_drive *d, uint16_t *w)
{
    w[63] &= 0x00ff;
    w[88] &= 0x00ff;
    if ((d->xfer_mode & 0xf8) == 0x20)
        w[63] |= (uint16_t)(0x100 << (d->xfer_mode & 7));
    else if ((d->xfer_mode & 0xf8) == 0x40)
        w[88] |= (uint16_t)(0x100 << (d->xfer_mode & 7));
}

static void identify_ata(ide_drive *d)
{
    uint16_t w[256];
    memset(w, 0, sizeof(w));
    uint64_t s = d->sectors;
    uint32_t cyl = (uint32_t)(s / (16 * 63));
    if (cyl > 16383) cyl = 16383;
    if (!cyl) cyl = 1;
    uint32_t chs = cyl * 16 * 63;
    uint32_t lba28 = s > 0x0fffffff ? 0x0fffffff : (uint32_t)s;
    w[0] = 0x0040;
    w[1] = (uint16_t)cyl;
    w[3] = 16;
    w[6] = 63;
    char serial[21];
    snprintf(serial, sizeof(serial), "MVM%05d%d", 1000 + d->bus->index * 2 + d->unit, d->unit);
    put_string(w, 10, 10, serial);
    put_string(w, 23, 4, "1.0");
    put_string(w, 27, 20, "MultiVM HARDDISK");
    w[47] = 0x8000 | 16;
    w[49] = (1 << 9) | (1 << 8);
    w[50] = 0x4000;
    w[51] = 0x200;
    w[52] = 0x200;
    w[53] = 7;
    w[54] = (uint16_t)cyl;
    w[55] = 16;
    w[56] = 63;
    w[57] = (uint16_t)chs;
    w[58] = (uint16_t)(chs >> 16);
    w[59] = (uint16_t)(d->mult ? 0x100 | d->mult : 0);
    w[60] = (uint16_t)lba28;
    w[61] = (uint16_t)(lba28 >> 16);
    w[63] = 0x07;
    w[64] = 0x03;
    w[65] = w[66] = w[67] = w[68] = 120;
    w[80] = 0xf0;
    w[81] = 0x16;
    w[82] = (1 << 14) | (1 << 5);
    w[83] = (1 << 14) | (1 << 13) | (1 << 12) | (1 << 10);
    w[84] = 1 << 14;
    w[85] = (1 << 14) | (1 << 5);
    w[86] = (1 << 13) | (1 << 12) | (1 << 10);
    w[87] = 1 << 14;
    w[88] = 0x3f; /* UDMA 0-5 */
    apply_xfer_mode(d, w);
    w[93] = 1 | (1 << 14) | 0x2000;
    if (d->blk->can_discard) { /* TRIM: o Linux e o Windows 7+ o usam em discos nao rotacionais */
        w[69] = (1 << 14) | (1 << 5); /* leitura deterministica, zeros apos TRIM */
        w[105] = 8;                   /* blocos de 512 B de faixas por comando */
        w[169] = 1;                   /* DATA SET MANAGEMENT / TRIM */
        w[217] = 1;                   /* nao rotacional */
    }
    w[100] = (uint16_t)s;
    w[101] = (uint16_t)(s >> 16);
    w[102] = (uint16_t)(s >> 32);
    w[103] = (uint16_t)(s >> 48);
    memcpy(d->buf, w, 512);
}

static void identify_atapi(ide_drive *d)
{
    uint16_t w[256];
    memset(w, 0, sizeof(w));
    w[0] = 0x85c0;
    char serial[21];
    snprintf(serial, sizeof(serial), "MVMCD%03d", d->bus->index * 2 + d->unit);
    put_string(w, 10, 10, serial);
    put_string(w, 23, 4, "1.0");
    put_string(w, 27, 20, "MultiVM DVD-ROM");
    w[48] = 1;
    w[49] = (1 << 9) | (1 << 8);
    w[53] = 7;
    w[62] = 7;
    w[63] = 7;
    w[64] = 3;
    w[65] = w[66] = w[67] = w[68] = 0xb4;
    w[71] = 30;
    w[72] = 30;
    w[80] = 0x1e;
    w[82] = 1 << 14;
    w[83] = 1 << 14;
    w[84] = 1 << 14;
    w[85] = 1 << 14;
    w[86] = 0;
    w[87] = 1 << 14;
    w[88] = 0x3f;
    w[93] = 1 | (1 << 14) | 0x2000;
    apply_xfer_mode(d, w);
    memcpy(d->buf, w, 512);
}

/* carrega o proximo bloco de leitura no buffer */
static bool ata_read_block(ide_drive *d)
{
    uint32_t n = d->remaining < d->block ? d->remaining : d->block;
    if (d->lba + n > d->sectors || blk_read(d->blk, d->lba * 512, d->buf, (size_t)n * 512) < 0)
        return false;
    pio_start(d, XFER_PIO_IN, n * 512);
    return true;
}

static void ata_rw(ide_drive *d, bool write, bool lba48, bool multiple, bool dma)
{
    uint64_t lba = get_lba(d, lba48);
    uint32_t count = lba48 ? (((uint32_t)d->hob_nsector << 8) | d->nsector) : d->nsector;
    if (!count)
        count = lba48 ? 65536 : 256;
    if (lba == UINT64_MAX || lba + count > d->sectors || (write && d->blk->readonly)) {
        cmd_abort(d);
        return;
    }
    d->lba = lba;
    d->remaining = count;
    d->block = multiple ? (d->mult ? d->mult : 1) : 1;
    d->op = write ? OP_WRITE : OP_READ;
    d->dma = dma;
    if (dma) {
        d->status = ST_DRDY | ST_DSC | ST_DRQ;
        ide_bus *b = d->bus;
        if (b->bm_cmd & 1)
            ide_bmdma_run(b->ctrl, b->index);
        return;
    }
    if (write) {
        uint32_t n = d->remaining < d->block ? d->remaining : d->block;
        pio_start(d, XFER_PIO_OUT, n * 512);
        return;
    }
    if (!ata_read_block(d)) {
        cmd_abort(d);
        return;
    }
    raise_irq(d->bus);
}

/* bloco de PIO concluido */
static void pio_block_done(ide_drive *d)
{
    uint32_t n = d->end / 512;
    if (d->op == OP_READ) {
        d->lba += n;
        d->remaining -= n;
        if (!d->remaining) {
            d->xfer = XFER_NONE;
            d->op = OP_NONE;
            d->status = ST_DRDY | ST_DSC;
            return;
        }
        if (!ata_read_block(d)) {
            cmd_abort(d);
            return;
        }
        raise_irq(d->bus);
    } else if (d->op == OP_WRITE) {
        if (blk_write(d->blk, d->lba * 512, d->buf, (size_t)n * 512) < 0) {
            cmd_abort(d);
            return;
        }
        d->lba += n;
        d->remaining -= n;
        if (!d->remaining) {
            cmd_done(d);
            return;
        }
        uint32_t m = d->remaining < d->block ? d->remaining : d->block;
        pio_start(d, XFER_PIO_OUT, m * 512);
        raise_irq(d->bus);
    }
}

/* ---------------------------------------------------------------- ATAPI */

#define SENSE_NOT_READY 2
#define SENSE_ILLEGAL 5
#define SENSE_UNIT_ATTN 6

static void atapi_status_done(ide_drive *d)
{
    d->xfer = XFER_NONE;
    d->op = OP_NONE;
    d->status = ST_DRDY | ST_DSC;
    d->nsector = 3; /* I/O | C/D: conclusao */
    raise_irq(d->bus);
}

static void atapi_error(ide_drive *d, uint8_t key, uint8_t asc)
{
    d->sense_key = key;
    d->asc = asc;
    d->xfer = XFER_NONE;
    d->op = OP_NONE;
    d->error = (uint8_t)(key << 4);
    d->status = ST_DRDY | ST_ERR;
    d->nsector = 3;
    raise_irq(d->bus);
}

/* entrega o proximo trecho de PIO (ou conclui). Dados validos em buf[pos, end). */
static void atapi_pio_next(ide_drive *d)
{
    if (d->pos >= d->end) {
        if (d->op != OP_ATAPI_READ || !d->remaining) {
            atapi_status_done(d);
            return;
        }
        uint32_t n = d->remaining < BUF_SIZE / 2048 ? d->remaining : BUF_SIZE / 2048;
        if (blk_read(d->blk, d->lba * 2048, d->buf, (size_t)n * 2048) < 0) {
            atapi_error(d, 3, 0x11); /* erro de leitura */
            return;
        }
        d->lba += n;
        d->remaining -= n;
        d->pos = 0;
        d->end = n * 2048;
    }
    uint32_t avail = d->end - d->pos;
    uint32_t lim = d->byte_limit & ~1u;
    if (!lim || lim > 0xfffe) lim = 0xfffe;
    uint32_t chunk = avail < lim ? avail : lim;
    d->chunk_left = chunk;
    d->lcyl = (uint8_t)chunk;
    d->hcyl = (uint8_t)(chunk >> 8);
    d->nsector = 2; /* I/O: dados para o host */
    d->xfer = XFER_PIO_IN;
    d->status = ST_DRDY | ST_DSC | ST_DRQ;
    raise_irq(d->bus);
}

static void atapi_reply(ide_drive *d, const void *data, uint32_t len, uint32_t alloc)
{
    if (len > alloc) len = alloc;
    memcpy(d->buf, data, len);
    if (!len) {
        atapi_status_done(d);
        return;
    }
    d->op = OP_ATAPI_DATA;
    d->pos = 0;
    d->end = len;
    d->atapi_len = len;
    if (d->dma) {
        d->status = ST_DRDY | ST_DSC | ST_DRQ;
        ide_bus *b = d->bus;
        if (b->bm_cmd & 1)
            ide_bmdma_run(b->ctrl, b->index);
        return;
    }
    atapi_pio_next(d);
}

static inline uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static inline void wbe32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static inline void wbe16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static void lba_to_msf(uint8_t *p, uint32_t lba)
{
    lba += 150;
    p[0] = 0;
    p[1] = (uint8_t)(lba / (60 * 75));
    p[2] = (uint8_t)((lba / 75) % 60);
    p[3] = (uint8_t)(lba % 75);
}

static void atapi_read(ide_drive *d, uint32_t lba, uint32_t count)
{
    if (!d->blk) {
        atapi_error(d, SENSE_NOT_READY, 0x3a);
        return;
    }
    if ((uint64_t)lba + count > d->sectors) {
        atapi_error(d, SENSE_ILLEGAL, 0x21);
        return;
    }
    if (!count) {
        atapi_status_done(d);
        return;
    }
    d->op = OP_ATAPI_READ;
    d->lba = lba;
    d->remaining = count;
    d->pos = d->end = 0;
    if (d->dma) {
        d->status = ST_DRDY | ST_DSC | ST_DRQ;
        ide_bus *b = d->bus;
        if (b->bm_cmd & 1)
            ide_bmdma_run(b->ctrl, b->index);
        return;
    }
    atapi_pio_next(d);
}

static void atapi_command(ide_drive *d)
{
    uint8_t cdb[12];
    memcpy(cdb, d->buf, 12);
    uint8_t r[128];
    memset(r, 0, sizeof(r));
    bool media = d->blk != NULL;
    uint32_t last = d->sectors ? (uint32_t)d->sectors - 1 : 0;

    /* midia trocada: o primeiro comando (fora os de estado) recebe UNIT ATTENTION */
    if (d->unit_attention && cdb[0] != 0x03 && cdb[0] != 0x12 && cdb[0] != 0x4a) {
        d->unit_attention = false;
        atapi_error(d, 6, 0x28);
        return;
    }

    switch (cdb[0]) {
    case 0x00: /* TEST UNIT READY */
        if (!media) atapi_error(d, SENSE_NOT_READY, 0x3a);
        else atapi_status_done(d);
        return;
    case 0x03: /* REQUEST SENSE */
        r[0] = 0x70;
        r[2] = d->sense_key;
        r[7] = 10;
        r[12] = d->asc;
        d->sense_key = 0;
        d->asc = 0;
        atapi_reply(d, r, 18, cdb[4]);
        return;
    case 0x12: /* INQUIRY */
        r[0] = 0x05;
        r[1] = 0x80;
        r[2] = 0x00;
        r[3] = 0x21;
        r[4] = 31;
        memcpy(r + 8, "MULTIVM ", 8);
        memcpy(r + 16, "DVD-ROM         ", 16);
        memcpy(r + 32, "1.0 ", 4);
        atapi_reply(d, r, 36, cdb[4]);
        return;
    case 0x1a: case 0x5a: { /* MODE SENSE (6/10) */
        unsigned page = cdb[2] & 0x3f;
        bool ten = cdb[0] == 0x5a;
        uint32_t alloc = ten ? ((uint32_t)cdb[7] << 8 | cdb[8]) : cdb[4];
        unsigned hl = ten ? 8 : 4, n = hl;
        if (page == 0x01 || page == 0x3f) {
            r[n] = 0x01; r[n + 1] = 0x06; r[n + 3] = 5;
            n += 8;
        }
        if (page == 0x2a || page == 0x3f) {
            uint8_t *p = r + n;
            p[0] = 0x2a; p[1] = 0x12;
            p[2] = 0x3b; p[3] = 0x00; p[4] = 0x71; p[5] = 0x60; p[6] = 0x29; p[7] = 0x00;
            wbe16(p + 8, 5540); wbe16(p + 10, 2); wbe16(p + 12, 512); wbe16(p + 14, 5540);
            n += 20;
        }
        if (n == hl) {
            atapi_error(d, SENSE_ILLEGAL, 0x24);
            return;
        }
        if (ten) {
            wbe16(r, n - 2);
            r[2] = 0x70;
        } else {
            r[0] = (uint8_t)(n - 1);
            r[1] = 0x70;
        }
        atapi_reply(d, r, n, alloc);
        return;
    }
    case 0x1b: case 0x1e: case 0x2b: case 0xbb: case 0x35: /* START STOP, PREVENT, SEEK, SPEED, SYNC */
        atapi_status_done(d);
        return;
    case 0x25: /* READ CAPACITY */
        if (!media) { atapi_error(d, SENSE_NOT_READY, 0x3a); return; }
        wbe32(r, last);
        wbe32(r + 4, 2048);
        atapi_reply(d, r, 8, 8);
        return;
    case 0x28: /* READ(10) */
        atapi_read(d, be32(cdb + 2), ((uint32_t)cdb[7] << 8) | cdb[8]);
        return;
    case 0xa8: /* READ(12) */
        atapi_read(d, be32(cdb + 2), be32(cdb + 6));
        return;
    case 0xbe: /* READ CD: so dados de usuario */
        if ((cdb[9] & 0xf8) != 0x10) {
            atapi_error(d, SENSE_ILLEGAL, 0x24);
            return;
        }
        atapi_read(d, be32(cdb + 2), ((uint32_t)cdb[6] << 16) | ((uint32_t)cdb[7] << 8) | cdb[8]);
        return;
    case 0x43: { /* READ TOC */
        if (!media) { atapi_error(d, SENSE_NOT_READY, 0x3a); return; }
        bool msf = cdb[1] & 2;
        unsigned fmt = cdb[2] & 0xf;
        if (!fmt) fmt = cdb[9] >> 6;
        uint32_t alloc = ((uint32_t)cdb[7] << 8) | cdb[8];
        unsigned n;
        if (fmt == 0) {
            r[2] = 1; r[3] = 1;
            n = 4;
            if (cdb[6] <= 1) {
                r[n + 1] = 0x14; r[n + 2] = 1;
                if (msf) lba_to_msf(r + n + 4, 0); else wbe32(r + n + 4, 0);
                n += 8;
            }
            r[n + 1] = 0x16; r[n + 2] = 0xaa;
            if (msf) lba_to_msf(r + n + 4, last + 1); else wbe32(r + n + 4, last + 1);
            n += 8;
        } else if (fmt == 1) {
            r[2] = 1; r[3] = 1;
            r[5] = 0x14; r[6] = 1;
            n = 12;
        } else if (fmt == 2) {
            r[2] = 1; r[3] = 1;
            n = 4;
            static const uint8_t pts[3] = {0xa0, 0xa1, 0xa2};
            for (int i = 0; i < 3; i++) {
                uint8_t *p = r + n;
                p[0] = 1; p[1] = 0x14; p[3] = pts[i];
                if (i < 2) p[8] = 1;
                else lba_to_msf(p + 7, last + 1), p[7] = p[8], p[8] = p[9], p[9] = p[10], p[10] = 0;
                n += 11;
            }
            uint8_t *p = r + n;
            p[0] = 1; p[1] = 0x14; p[3] = 1; p[8] = 0; p[9] = 2; p[10] = 0;
            n += 11;
        } else {
            atapi_error(d, SENSE_ILLEGAL, 0x24);
            return;
        }
        wbe16(r, n - 2);
        atapi_reply(d, r, n, alloc);
        return;
    }
    case 0x42: /* READ SUB-CHANNEL */
        r[1] = 0x15;
        atapi_reply(d, r, 4, ((uint32_t)cdb[7] << 8) | cdb[8]);
        return;
    case 0x46: { /* GET CONFIGURATION */
        uint32_t alloc = ((uint32_t)cdb[7] << 8) | cdb[8];
        uint16_t prof = !media ? 0 : (d->sectors > 1433600 ? 0x10 : 0x08);
        wbe16(r + 6, prof);
        /* feature 0: lista de perfis */
        uint8_t *f = r + 8;
        f[1] = 0; f[2] = 0x03; f[3] = 8;
        wbe16(f + 4, 0x10); f[6] = prof == 0x10;
        wbe16(f + 8, 0x08); f[10] = prof == 0x08;
        wbe32(r, 8 + 12 - 4);
        atapi_reply(d, r, 20, alloc);
        return;
    }
    case 0x4a: { /* GET EVENT STATUS NOTIFICATION */
        if (!(cdb[1] & 1)) { atapi_error(d, SENSE_ILLEGAL, 0x24); return; }
        uint32_t alloc = ((uint32_t)cdb[7] << 8) | cdb[8];
        wbe16(r, 6);
        r[2] = 4;
        r[3] = 0x10;
        r[4] = 0;
        if (d->media_event) { /* 2 = midia nova, 3 = midia removida */
            r[4] = media ? 2 : 3;
            d->media_event = false;
        }
        r[5] = media ? 2 : 0;
        atapi_reply(d, r, 8, alloc);
        return;
    }
    case 0x51: { /* READ DISC INFORMATION */
        if (!media) { atapi_error(d, SENSE_NOT_READY, 0x3a); return; }
        uint32_t alloc = ((uint32_t)cdb[7] << 8) | cdb[8];
        wbe16(r, 32);
        r[2] = 0x0e; /* disco completo, ultima sessao completa */
        r[3] = 1; r[4] = 1; r[5] = 1; r[6] = 1;
        atapi_reply(d, r, 34, alloc);
        return;
    }
    case 0x52: { /* READ TRACK INFORMATION: uma unica trilha de dados */
        if (!media) { atapi_error(d, SENSE_NOT_READY, 0x3a); return; }
        uint32_t alloc = ((uint32_t)cdb[7] << 8) | cdb[8];
        uint32_t addr = ((uint32_t)cdb[2] << 24) | ((uint32_t)cdb[3] << 16) | ((uint32_t)cdb[4] << 8) | cdb[5];
        uint8_t type = cdb[1] & 3;
        if ((type == 1 && addr > 1) || (type == 2 && addr > 1) || (type == 0 && addr >= d->sectors)) {
            atapi_error(d, SENSE_ILLEGAL, 0x21);
            return;
        }
        wbe16(r, 34);
        r[2] = 1; r[3] = 1;   /* trilha 1, sessao 1 */
        r[5] = 0x04;          /* trilha de dados */
        r[6] = 0x01;          /* modo de dados 1 */
        wbe32(r + 8, 0);      /* inicio */
        wbe32(r + 24, (uint32_t)d->sectors); /* tamanho */
        atapi_reply(d, r, 36, alloc);
        return;
    }
    case 0xbd: /* MECHANISM STATUS */
        wbe16(r + 6, 8);
        atapi_reply(d, r, 8, ((uint32_t)cdb[8] << 8) | cdb[9]);
        return;
    default:
        LOGD("atapi: comando 0x%02x nao suportado", cdb[0]);
        atapi_error(d, SENSE_ILLEGAL, 0x20);
        return;
    }
}

/* ------------------------------------------------------------ comandos */

static void exec_command(ide_drive *d, uint8_t cmd)
{
    ide_bus *b = d->bus;
    lower_irq(b);
    d->error = 0;
    bool atapi = d->kind == KIND_ATAPI;
    switch (cmd) {
    case 0xec: /* IDENTIFY DEVICE */
        if (atapi) {
            set_signature(d);
            cmd_abort(d);
            return;
        }
        identify_ata(d);
        pio_start(d, XFER_PIO_IN, 512);
        d->op = OP_NONE;
        raise_irq(b);
        return;
    case 0xa1: /* IDENTIFY PACKET DEVICE */
        if (!atapi) { cmd_abort(d); return; }
        identify_atapi(d);
        pio_start(d, XFER_PIO_IN, 512);
        d->op = OP_NONE;
        raise_irq(b);
        return;
    case 0xef: /* SET FEATURES */
        if (d->feature == 0x03) {
            uint8_t m = d->nsector;
            bool ok = m <= 0x01 || (m >= 0x08 && m <= 0x0c) || (m >= 0x20 && m <= 0x22) || (m >= 0x40 && m <= 0x45);
            if (!ok) { cmd_abort(d); return; }
            d->xfer_mode = m;
        }
        d->status = ST_DRDY | ST_DSC;
        d->error = 0;
        if (atapi) d->nsector = 0x03; /* I/O + C/D: fim de comando */
        cmd_done(d);
        return;
    case 0xe5: /* CHECK POWER MODE */
        d->nsector = 0xff;
        cmd_done(d);
        return;
    case 0xe0: case 0xe1: case 0xe2: case 0xe3: case 0xe6: /* standby/idle/sleep */
        cmd_done(d);
        return;
    case 0x90: /* EXECUTE DEVICE DIAGNOSTIC */
        set_signature(d);
        d->error = 1;
        d->status = atapi ? 0 : (ST_DRDY | ST_DSC);
        raise_irq(b);
        return;
    case 0x08: /* DEVICE RESET */
        if (!atapi) { cmd_abort(d); return; }
        drive_reset(d);
        return;
    case 0xa0: /* PACKET */
        if (!atapi) { cmd_abort(d); return; }
        d->dma = d->feature & 1;
        d->byte_limit = ((uint32_t)d->hcyl << 8) | d->lcyl;
        d->xfer = XFER_PACKET;
        d->pos = 0;
        d->end = 12;
        d->nsector = 1; /* C/D: esperando pacote */
        d->status = ST_DRDY | ST_DSC | ST_DRQ;
        return;
    default:
        break;
    }
    if (atapi) { /* demais comandos ATA sao abortados em dispositivos ATAPI */
        cmd_abort(d);
        return;
    }
    switch (cmd) {
    case 0x20: case 0x21: ata_rw(d, false, false, false, false); return;
    case 0x24: ata_rw(d, false, true, false, false); return;
    case 0x30: case 0x31: ata_rw(d, true, false, false, false); return;
    case 0x34: ata_rw(d, true, true, false, false); return;
    case 0xc4: ata_rw(d, false, false, true, false); return;
    case 0x29: ata_rw(d, false, true, true, false); return;
    case 0xc5: ata_rw(d, true, false, true, false); return;
    case 0x39: ata_rw(d, true, true, true, false); return;
    case 0xc8: case 0xc9: ata_rw(d, false, false, false, true); return;
    case 0x25: ata_rw(d, false, true, false, true); return;
    case 0xca: case 0xcb: ata_rw(d, true, false, false, true); return;
    case 0x35: ata_rw(d, true, true, false, true); return;
    case 0xc6: /* SET MULTIPLE MODE */
        if (d->nsector > 16 || (d->nsector & (d->nsector - 1))) { cmd_abort(d); return; }
        d->mult = d->nsector;
        cmd_done(d);
        return;
    case 0x40: case 0x41: case 0x42: /* READ VERIFY */
    case 0x10: case 0x70: case 0x91:
    case 0x00:
        cmd_done(d);
        return;
    case 0x06: { /* DATA SET MANAGEMENT (TRIM): lista de faixas por DMA */
        uint32_t count = ((uint32_t)d->hob_nsector << 8) | d->nsector;
        if (!(d->feature & 1) || !d->blk->can_discard || !count || count > 8) {
            cmd_abort(d);
            return;
        }
        d->remaining = count;
        d->op = OP_TRIM;
        d->dma = true;
        d->status = ST_DRDY | ST_DSC | ST_DRQ;
        ide_bus *bb = d->bus;
        if (bb->bm_cmd & 1)
            ide_bmdma_run(bb->ctrl, bb->index);
        return;
    }
    case 0xe7: case 0xea: /* FLUSH CACHE */
        blk_flush(d->blk);
        cmd_done(d);
        return;
    case 0xf8: case 0x27: { /* READ NATIVE MAX ADDRESS */
        set_lba(d, d->sectors - 1, cmd == 0x27);
        cmd_done(d);
        return;
    }
    default:
        LOGD("ide: comando ATA 0x%02x nao suportado", cmd);
        cmd_abort(d);
        return;
    }
}

void ide_debug_dump(ide_ctrl *c)
{
    for (int i = 0; i < 2; i++) {
        ide_bus *b = &c->bus[i];
        ide_drive *d = &b->drv[b->cur];
        LOGI("ide%d: cur=%d kind=%d st=%02x err=%02x op=%d dma=%d xfer=%d pos=%u end=%u rem=%u irqp=%d devctl=%02x bm=%02x/%02x",
             i, b->cur, d->kind, d->status, d->error, d->op, d->dma, d->xfer, d->pos, d->end, d->remaining,
             b->irq_pending, b->devctl, b->bm_cmd, b->bm_status);
    }
}

/* ------------------------------------------------------------ bus master */

/*
 * Movimenta os dados de um comando DMA (disco ou ATAPI) pelo callback de
 * scatter-gather 'fn'. Retorna -1 em erro de E/S. Se a lista acabar antes dos
 * dados, so os setores completos contam (transferencia curta).
 */
static int dma_move(ide_drive *d, ide_xfer_fn fn, void *opaque, uint32_t *moved)
{
    uint32_t unit = (d->kind == KIND_ATAPI && d->op == OP_ATAPI_READ) ? 2048 : 512;
    if (d->op == OP_ATAPI_DATA) {
        *moved += fn(opaque, d->buf, d->atapi_len, true);
        return 0;
    }
    bool write = d->op == OP_WRITE || d->op == OP_TRIM;
    while (d->remaining) {
        uint32_t n = d->remaining * unit > BUF_SIZE ? BUF_SIZE / unit : d->remaining;
        uint32_t len = n * unit;
        if (!write && blk_read(d->blk, d->lba * unit, d->buf, len) < 0)
            return -1;
        uint32_t got = fn(opaque, d->buf, len, !write);
        *moved += got;
        uint32_t full = got / unit;
        if (d->op == OP_TRIM) { /* entradas de 8 bytes: LBA (48 bits) + setores (16 bits) */
            for (uint32_t k = 0; k < full * unit; k += 8) {
                uint64_t e = ld_le(d->buf + k, 8), lba = e & 0xffffffffffffULL, cnt = e >> 48;
                if (cnt && (lba + cnt > d->sectors || blk_discard(d->blk, lba * 512, cnt * 512) < 0))
                    return -1;
            }
            d->remaining -= full;
            if (got < len)
                break;
            continue;
        }
        if (write && full && blk_write(d->blk, d->lba * unit, d->buf, (size_t)full * unit) < 0)
            return -1;
        d->lba += full;
        d->remaining -= full;
        if (got < len)
            break;
    }
    return 0;
}

/* lista PRD do bus master do PIIX (entradas de 8 bytes) */
typedef struct {
    mvm_space *mem;
    uint32_t prd, addr, left;
    bool eot;
    int guard;
} bm_sg;

static uint32_t bm_xfer(void *opaque, uint8_t *data, uint32_t len, bool to_host)
{
    bm_sg *s = opaque;
    uint32_t done = 0;
    while (done < len) {
        if (!s->left) {
            if (s->eot || s->guard++ > 8192)
                break;
            s->addr = (uint32_t)space_read(s->mem, s->prd, 4);
            uint32_t cnt = (uint32_t)space_read(s->mem, s->prd + 4, 4);
            s->eot = cnt & 0x80000000u;
            s->left = cnt & 0xfffe;
            if (!s->left)
                s->left = 0x10000;
            s->prd += 8;
        }
        uint32_t step = len - done < s->left ? len - done : s->left;
        if (to_host)
            space_memwrite(s->mem, s->addr, data + done, step);
        else
            space_memread(s->mem, s->addr, data + done, step);
        s->addr += step;
        s->left -= step;
        done += step;
    }
    return done;
}

void ide_bmdma_run(ide_ctrl *c, int bi)
{
    ide_bus *b = &c->bus[bi];
    ide_drive *d = &b->drv[b->cur];
    if (!(d->status & ST_DRQ) || !d->dma || d->op == OP_NONE)
        return;
    bm_sg sg = {&c->vm->mem, b->bm_prdt, 0, 0, false, 0};
    uint32_t moved = 0;
    int r = dma_move(d, bm_xfer, &sg, &moved);
    b->bm_status &= ~1u;
    if (r < 0) {
        b->bm_status |= 2;
        if (d->kind == KIND_ATAPI) atapi_error(d, 3, 0x11);
        else cmd_abort(d);
        return;
    }
    if (d->kind == KIND_ATAPI) {
        atapi_status_done(d);
    } else {
        d->dma = false;
        cmd_done(d);
    }
}

static uint64_t bm_read(void *opaque, uint64_t off, unsigned size)
{
    ide_ctrl *c = opaque;
    ide_bus *b = &c->bus[(off >> 3) & 1];
    unsigned r = off & 7;
    uint64_t v = 0;
    for (unsigned i = 0; i < size; i++, r++) {
        uint8_t byte;
        switch (r) {
        case 0: byte = b->bm_cmd; break;
        case 2: byte = b->bm_status; break;
        case 4: case 5: case 6: case 7: byte = (uint8_t)(b->bm_prdt >> (8 * (r - 4))); break;
        default: byte = 0; break;
        }
        v |= (uint64_t)byte << (8 * i);
    }
    return v;
}

static void bm_write(void *opaque, uint64_t off, uint64_t val, unsigned size)
{
    ide_ctrl *c = opaque;
    int bi = (int)((off >> 3) & 1);
    ide_bus *b = &c->bus[bi];
    unsigned r = off & 7;
    for (unsigned i = 0; i < size; i++, r++) {
        uint8_t v = (uint8_t)(val >> (8 * i));
        switch (r) {
        case 0: {
            bool start = (v & 1) && !(b->bm_cmd & 1);
            b->bm_cmd = v & 0x09;
            if (start) {
                b->bm_status |= 1;
                ide_bmdma_run(c, bi);
            } else if (!(v & 1)) {
                b->bm_status &= ~1u;
            }
            break;
        }
        case 2:
            b->bm_status = (uint8_t)((b->bm_status & ~0x06u & ~0x60u) | (v & 0x60) | (b->bm_status & 0x06 & ~v));
            break;
        case 4: case 5: case 6: case 7: {
            unsigned sh = 8 * (r - 4);
            b->bm_prdt = (b->bm_prdt & ~(0xffu << sh)) | ((uint32_t)v << sh);
            b->bm_prdt &= ~3u;
            break;
        }
        default: break;
        }
    }
}

const mvm_io_ops ide_bmdma_ops = {bm_read, bm_write};

/* ------------------------------------------------------ portas do canal */

static bool bus_empty(ide_bus *b) { return b->drv[0].kind == KIND_NONE && b->drv[1].kind == KIND_NONE; }

static uint32_t data_read(ide_bus *b, unsigned size)
{
    ide_drive *d = &b->drv[b->cur];
    if (d->xfer != XFER_PIO_IN)
        return 0xffffffffu >> (32 - 8 * size);
    uint32_t v = 0;
    bool atapi_data = d->kind == KIND_ATAPI && (d->op == OP_ATAPI_READ || d->op == OP_ATAPI_DATA);
    for (unsigned i = 0; i < size && d->pos < d->end; i++) {
        v |= (uint32_t)d->buf[d->pos++] << (8 * i);
        if (atapi_data && d->chunk_left)
            d->chunk_left--;
    }
    if (atapi_data) {
        if (!d->chunk_left)
            atapi_pio_next(d);
    } else if (d->pos >= d->end) {
        if (d->op == OP_READ) {
            pio_block_done(d);
        } else {
            d->xfer = XFER_NONE;
            d->status = ST_DRDY | ST_DSC;
        }
    }
    return v;
}

static void data_write(ide_bus *b, uint32_t v, unsigned size)
{
    ide_drive *d = &b->drv[b->cur];
    if (d->xfer != XFER_PIO_OUT && d->xfer != XFER_PACKET)
        return;
    for (unsigned i = 0; i < size && d->pos < d->end; i++)
        d->buf[d->pos++] = (uint8_t)(v >> (8 * i));
    if (d->pos >= d->end) {
        if (d->xfer == XFER_PACKET) {
            d->xfer = XFER_NONE;
            d->status = ST_DRDY | ST_DSC | ST_BSY;
            atapi_command(d);
        } else {
            pio_block_done(d);
        }
    }
}

static uint64_t ide_read(void *opaque, uint64_t off, unsigned size)
{
    ide_bus *b = opaque;
    ide_drive *d = &b->drv[b->cur];
    bool hob = b->devctl & 0x80;
    if (off == 0)
        return data_read(b, size);
    if (bus_empty(b))
        return 0xff;
    bool absent = d->kind == KIND_NONE;
    switch (off) {
    case 1: return absent ? 0 : hob ? d->hob_feature : d->error;
    case 2: return absent ? 0 : hob ? d->hob_nsector : d->nsector;
    case 3: return absent ? 0 : hob ? d->hob_sector : d->sector;
    case 4: return absent ? 0 : hob ? d->hob_lcyl : d->lcyl;
    case 5: return absent ? 0 : hob ? d->hob_hcyl : d->hcyl;
    case 6: return absent ? (0xa0 | (b->cur << 4)) : d->select;
    default:
        if (absent)
            return 0;
        lower_irq(b);
        return d->status;
    }
}

static void ide_write(void *opaque, uint64_t off, uint64_t val, unsigned size)
{
    ide_bus *b = opaque;
    uint8_t v = (uint8_t)val;
    if (off == 0) {
        data_write(b, (uint32_t)val, size);
        return;
    }
    if (off == 7) {
        ide_drive *d = &b->drv[b->cur];
        if (d->kind == KIND_NONE)
            return;
        exec_command(d, v);
        return;
    }
    b->devctl &= ~0x80u; /* escrita no taskfile limpa HOB */
    for (int i = 0; i < 2; i++) {
        ide_drive *d = &b->drv[i];
        switch (off) {
        case 1: d->hob_feature = d->feature; d->feature = v; break;
        case 2: d->hob_nsector = d->nsector; d->nsector = v; break;
        case 3: d->hob_sector = d->sector; d->sector = v; break;
        case 4: d->hob_lcyl = d->lcyl; d->lcyl = v; break;
        case 5: d->hob_hcyl = d->hcyl; d->hcyl = v; break;
        case 6: d->select = (uint8_t)((v & ~0x10u) | (i << 4) | 0xa0); break;
        default: break;
        }
    }
    if (off == 6)
        b->cur = (v >> 4) & 1;
}

static uint64_t ctl_read(void *opaque, uint64_t off, unsigned size)
{
    ide_bus *b = opaque;
    (void)off; (void)size;
    if (bus_empty(b))
        return 0xff;
    ide_drive *d = &b->drv[b->cur];
    return d->kind == KIND_NONE ? 0 : d->status;
}

static void ctl_write(void *opaque, uint64_t off, uint64_t val, unsigned size)
{
    ide_bus *b = opaque;
    (void)off; (void)size;
    uint8_t v = (uint8_t)val;
    if (!(b->devctl & 4) && (v & 4)) { /* SRST */
        for (int i = 0; i < 2; i++) {
            b->drv[i].status = ST_BSY | ST_DSC;
            b->drv[i].error = 1;
        }
    } else if ((b->devctl & 4) && !(v & 4)) {
        for (int i = 0; i < 2; i++)
            if (b->drv[i].kind != KIND_NONE)
                drive_reset(&b->drv[i]);
        b->cur = 0;
    }
    b->devctl = v;
    update_irq(b);
}

const mvm_io_ops ide_cmd_ops = {ide_read, ide_write};
const mvm_io_ops ide_ctl_ops = {ctl_read, ctl_write};

/* --------------------------------------------------------------- setup */

ide_ctrl *ide_new(mvm_vm *vm, irq_line irq14, irq_line irq15)
{
    ide_ctrl *c = calloc(1, sizeof(*c));
    c->vm = vm;
    for (int i = 0; i < 2; i++) {
        ide_bus *b = &c->bus[i];
        b->ctrl = c;
        b->index = i;
        b->irq = i ? irq15 : irq14;
        for (int u = 0; u < 2; u++) {
            b->drv[u].bus = b;
            b->drv[u].unit = u;
            b->drv[u].buf = calloc(1, BUF_SIZE);
        }
    }
    return c;
}

void *ide_bus_opaque(ide_ctrl *c, int bus) { return &c->bus[bus & 1]; }

int ide_drive_kind(ide_ctrl *c, int bus, int unit) { return c->bus[bus & 1].drv[unit & 1].kind; }

bool ide_attach(ide_ctrl *c, int bus, int unit, mvm_blk *blk, bool cdrom)
{
    ide_drive *d = &c->bus[bus & 1].drv[unit & 1];
    if (d->kind != KIND_NONE)
        return false;
    d->kind = cdrom ? KIND_ATAPI : KIND_ATA;
    d->xfer_mode = 0x45; /* UDMA5 ate o SO escolher */
    d->blk = blk;
    d->sectors = blk ? blk->size / (cdrom ? 2048 : 512) : 0;
    c->bus[bus & 1].bm_status |= (uint8_t)(0x20 << (unit & 1));
    drive_reset(d);
    return true;
}

static void change_media(ide_drive *d, mvm_blk *blk)
{
    d->blk = blk;
    d->sectors = blk ? blk->size / 2048 : 0;
    d->unit_attention = true;
    d->media_event = true;
}

bool ide_change_media(ide_ctrl *c, int bus, int unit, mvm_blk *blk)
{
    ide_drive *d = &c->bus[bus & 1].drv[unit & 1];
    if (d->kind != KIND_ATAPI)
        return false;
    change_media(d, blk);
    return true;
}

void ide_reset(ide_ctrl *c)
{
    for (int i = 0; i < 2; i++) {
        ide_bus *b = &c->bus[i];
        b->devctl = 0;
        b->cur = 0;
        b->bm_cmd = 0;
        b->bm_status &= 0x60;
        b->bm_prdt = 0;
        lower_irq(b);
        for (int u = 0; u < 2; u++)
            if (b->drv[u].kind != KIND_NONE)
                drive_reset(&b->drv[u]);
    }
}

void ide_free(ide_ctrl *c)
{
    if (!c)
        return;
    for (int i = 0; i < 2; i++)
        for (int u = 0; u < 2; u++)
            free(c->bus[i].drv[u].buf);
    free(c);
}

/* ------------------------------------------------ porta de comandos (AHCI) */

struct ide_port {
    ide_ctrl *c;
};

ide_port *ide_port_new(mvm_vm *vm, mvm_blk *blk, bool cdrom)
{
    ide_port *p = calloc(1, sizeof(*p));
    p->c = ide_new(vm, (irq_line){0}, (irq_line){0});
    ide_attach(p->c, 0, 0, blk, cdrom);
    return p;
}

void ide_port_free(ide_port *p)
{
    if (!p)
        return;
    ide_free(p->c);
    free(p);
}

static ide_drive *port_drive(ide_port *p) { return &p->c->bus[0].drv[0]; }

void ide_port_reset(ide_port *p) { drive_reset(port_drive(p)); }

int ide_port_kind(ide_port *p) { return port_drive(p)->kind; }

bool ide_port_change_media(ide_port *p, mvm_blk *blk)
{
    ide_drive *d = port_drive(p);
    if (d->kind != KIND_ATAPI)
        return false;
    change_media(d, blk);
    return true;
}

uint32_t ide_port_signature(ide_port *p)
{
    ide_drive *d = port_drive(p);
    return ((uint32_t)d->hcyl << 24) | ((uint32_t)d->lcyl << 16) | ((uint32_t)d->sector << 8) | d->nsector;
}

void ide_port_exec(ide_port *p, const uint8_t *fis, const uint8_t *cdb, ide_xfer_fn fn, void *opaque, ide_port_result *r)
{
    ide_bus *b = &p->c->bus[0];
    ide_drive *d = port_drive(p);
    b->cur = 0;
    b->devctl = 0;
    d->feature = fis[3];
    d->sector = fis[4];
    d->lcyl = fis[5];
    d->hcyl = fis[6];
    d->select = (uint8_t)(fis[7] | 0xa0);
    d->hob_sector = fis[8];
    d->hob_lcyl = fis[9];
    d->hob_hcyl = fis[10];
    d->hob_feature = fis[11];
    d->nsector = fis[12];
    d->hob_nsector = fis[13];
    exec_command(d, fis[2]);
    uint32_t moved = 0;
    bool pio = false;
    for (int guard = 0; guard < (1 << 22); guard++) {
        if (d->dma && (d->status & ST_DRQ) && d->op != OP_NONE) {
            int e = dma_move(d, fn, opaque, &moved);
            if (e < 0) {
                if (d->kind == KIND_ATAPI) atapi_error(d, 3, 0x11);
                else cmd_abort(d);
            } else if (d->kind == KIND_ATAPI) {
                atapi_status_done(d);
            } else {
                d->dma = false;
                cmd_done(d);
            }
            break;
        }
        if (d->xfer == XFER_PACKET) {
            memset(d->buf, 0, 16);
            memcpy(d->buf, cdb, 12);
            d->pos = 12;
            d->xfer = XFER_NONE;
            d->status = ST_DRDY | ST_DSC | ST_BSY;
            atapi_command(d);
            continue;
        }
        if (d->xfer == XFER_PIO_IN) {
            pio = true;
            bool atapi_data = d->kind == KIND_ATAPI && (d->op == OP_ATAPI_READ || d->op == OP_ATAPI_DATA);
            uint32_t n = d->end - d->pos;
            if (atapi_data && d->chunk_left && d->chunk_left < n)
                n = d->chunk_left;
            moved += fn(opaque, d->buf + d->pos, n, true);
            d->pos += n;
            if (atapi_data) {
                d->chunk_left = 0;
                atapi_pio_next(d);
            } else if (d->pos >= d->end) {
                if (d->op == OP_READ) {
                    pio_block_done(d);
                } else {
                    d->xfer = XFER_NONE;
                    d->status = ST_DRDY | ST_DSC;
                }
            }
            continue;
        }
        if (d->xfer == XFER_PIO_OUT) {
            pio = true;
            moved += fn(opaque, d->buf + d->pos, d->end - d->pos, false);
            d->pos = d->end;
            pio_block_done(d);
            continue;
        }
        break;
    }
    r->status = d->status & (uint8_t)~ST_BSY;
    r->error = d->error;
    r->device = d->select;
    r->nsector = d->nsector;
    r->sector = d->sector;
    r->lcyl = d->lcyl;
    r->hcyl = d->hcyl;
    r->hob_nsector = d->hob_nsector;
    r->hob_sector = d->hob_sector;
    r->hob_lcyl = d->hob_lcyl;
    r->hob_hcyl = d->hob_hcyl;
    r->bytes = moved;
    r->pio = pio;
}
