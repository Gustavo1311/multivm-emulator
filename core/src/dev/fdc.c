/*
 * Controlador de disquete compativel com o Intel 82078 (modo PC/AT), dois drives.
 * Portas 0x3f0-0x3f5 e 0x3f7 (a 0x3f6 e do IDE), IRQ 6, DMA canal 2.
 *
 * A geometria da midia sai do tamanho da imagem (160 KiB a 2,88 MiB). Os
 * comandos sao executados de uma vez; a interrupcao de conclusao vem um pouco
 * depois por um temporizador, como num controlador real (so que bem mais
 * rapido). Com DMA desligado (SPECIFY ND) os dados passam pelo FIFO (PIO).
 */
#include "devices.h"

#include <stdlib.h>

#define FDC_DRIVES 2
#define SECTOR 512
#define MAX_SPT 36
#define XFER_MAX (2 * MAX_SPT * SECTOR) /* uma trilha nas duas faces (MT) */

/* registradores */
#define MSR_RQM 0x80
#define MSR_DIO 0x40
#define MSR_NDMA 0x20
#define MSR_CB 0x10

#define DOR_SEL 0x03
#define DOR_NRESET 0x04
#define DOR_DMAEN 0x08

#define ST0_ABNORMAL 0x40
#define ST0_INVALID 0x80
#define ST0_SEEK 0x20
#define ST0_RDYCHG 0xc0

#define ST1_MA 0x01
#define ST1_NW 0x02
#define ST1_ND 0x04
#define ST1_EC 0x80

#define ST3_TS 0x08
#define ST3_T0 0x10
#define ST3_RY 0x20
#define ST3_WP 0x40

/* comandos (byte inteiro ou 5 bits baixos) */
#define CMD_READ_TRACK 0x02
#define CMD_SPECIFY 0x03
#define CMD_SENSE_DRIVE 0x04
#define CMD_WRITE 0x05
#define CMD_READ 0x06
#define CMD_RECAL 0x07
#define CMD_SENSE_INT 0x08
#define CMD_READ_ID 0x0a
#define CMD_FORMAT 0x0d
#define CMD_DUMPREG 0x0e
#define CMD_SEEK 0x0f
#define CMD_VERSION 0x10
#define CMD_PERPENDICULAR 0x12
#define CMD_CONFIGURE 0x13
#define CMD_LOCK 0x14
#define CMD_PART_ID 0x18

#define FDC_DMA_CH 2
#define FDC_DELAY_NS 50000LL     /* conclusao de leitura/escrita */
#define FDC_SEEK_NS 200000LL     /* conclusao de seek/recalibrate */
#define FDC_DMA_RETRY_NS 100000LL

typedef struct {
    mvm_blk *blk;    /* NULL = drive sem disquete */
    bool exists;     /* o drive esta instalado (mesmo vazio) */
    bool readonly;
    uint8_t tracks, heads, spt;
    uint8_t rate;      /* taxa de dados exigida (codigo do CCR) */
    uint8_t cyl;       /* posicao da cabeca */
    uint8_t head, sect;
    bool changed;      /* linha de troca de midia (DIR bit 7) */
} fd_drive;

typedef enum { PH_CMD, PH_EXEC, PH_RESULT } fd_phase;

struct fdc {
    mvm_vm *vm;
    irq_line irq;
    i8237 *dma;
    fd_drive drv[FDC_DRIVES];

    uint8_t dor, tdr, dsr, rate;
    uint8_t msr;
    fd_phase phase;

    uint8_t cmd[16];
    int cmd_len, cmd_pos;
    uint8_t res[16];
    int res_len, res_pos;

    /* transferencia em andamento */
    uint8_t *buf;
    int xfer_len, xfer_pos;
    bool xfer_write, xfer_pio;
    int dma_waits;

    /* estado do controlador */
    uint8_t specify[2];
    bool nodma;
    uint8_t config, pretrk;
    bool lock;
    uint8_t perp;
    int reset_sense;      /* SENSE INTERRUPT pendentes apos reset */
    bool irq_pending;
    uint8_t int_st0;      /* ST0 de um seek/recalibrate concluido */
    int int_drive;
    bool int_valid;

    mvm_timer timer;
    void (*timer_fn)(fdc *f);
    mvm_timer seek_timer;  /* seek em andamento (o controlador fica livre) */
};

typedef struct {
    uint32_t size;
    uint8_t tracks, heads, spt, rate, cmos;
} fd_format;

/* CCR: 0 = 500 kbps, 1 = 300, 2 = 250, 3 = 1 Mbps. cmos: tipo do drive */
static const fd_format formats[] = {
    {163840, 40, 1, 8, 2, 1},   /* 160 KiB  5,25" */
    {184320, 40, 1, 9, 2, 1},   /* 180 KiB */
    {327680, 40, 2, 8, 2, 1},   /* 320 KiB */
    {368640, 40, 2, 9, 2, 1},   /* 360 KiB */
    {737280, 80, 2, 9, 2, 3},   /* 720 KiB  3,5" */
    {1228800, 80, 2, 15, 0, 2}, /* 1,2 MiB  5,25" */
    {1474560, 80, 2, 18, 0, 4}, /* 1,44 MiB 3,5" */
    {1720320, 80, 2, 21, 0, 4}, /* 1,68 MiB (DMF) */
    {1763328, 82, 2, 21, 0, 4}, /* 1,72 MiB */
    {2949120, 80, 2, 36, 3, 5}, /* 2,88 MiB */
};

static const fd_format *format_for(uint64_t size)
{
    for (size_t i = 0; i < ARRAY_SIZE(formats); i++)
        if (formats[i].size == size)
            return &formats[i];
    /* tamanho desconhecido: menor formato que caiba (o resto le como zeros) */
    for (size_t i = 0; i < ARRAY_SIZE(formats); i++)
        if (size < formats[i].size && formats[i].heads == 2 && formats[i].tracks == 80)
            return &formats[i];
    return &formats[ARRAY_SIZE(formats) - 1];
}

int fdc_cmos_type(uint64_t size) { return format_for(size)->cmos; }

/* ---------------------------------------------------------------- IRQ */

static void update_irq(fdc *f)
{
    /* no modo AT a saida de IRQ passa pela porta DMAEN do DOR */
    irq_set(&f->irq, f->irq_pending && (f->dor & DOR_DMAEN) ? 1 : 0);
}

static void raise_irq(fdc *f)
{
    f->irq_pending = true;
    update_irq(f);
}

static void lower_irq(fdc *f)
{
    f->irq_pending = false;
    update_irq(f);
}

/* ---------------------------------------------------------------- fases */

static void to_command(fdc *f)
{
    f->phase = PH_CMD;
    f->cmd_pos = f->cmd_len = 0;
    f->msr = MSR_RQM;
}

static void to_result(fdc *f, int n)
{
    f->phase = PH_RESULT;
    f->res_len = n;
    f->res_pos = 0;
    f->msr = MSR_RQM | MSR_DIO | MSR_CB;
}

static void schedule(fdc *f, int64_t ns, void (*fn)(fdc *))
{
    f->timer_fn = fn;
    timer_mod(f->vm, &f->timer, mvm_now(f->vm) + ns);
}

static void timer_cb(void *opaque)
{
    fdc *f = opaque;
    void (*fn)(fdc *) = f->timer_fn;
    f->timer_fn = NULL;
    if (fn)
        fn(f);
}

static void soft_reset(fdc *f)
{
    timer_del(f->vm, &f->timer);
    timer_del(f->vm, &f->seek_timer);
    f->timer_fn = NULL;
    lower_irq(f);
    to_command(f);
    f->xfer_len = f->xfer_pos = 0;
    f->int_valid = false;
    f->nodma = false;
    if (!f->lock) {
        f->config = 0x20; /* FIFO desligado, polling ligado */
        f->pretrk = 0;
    }
    f->perp = 0;
}

/* ---------------------------------------------------------------- midia */

static fd_drive *cur_drive(fdc *f, uint8_t hds) { return &f->drv[hds & 1]; }

static bool drive_present(fd_drive *d) { return d->blk != NULL; }

/* posicao (bytes) do setor (c, h, r); -1 se fora da midia */
static int64_t chs_offset(fd_drive *d, int c, int h, int r)
{
    if (c >= d->tracks || h >= d->heads || r < 1 || r > d->spt)
        return -1;
    return ((int64_t)(c * d->heads + h) * d->spt + (r - 1)) * SECTOR;
}

static int read_sector(fd_drive *d, int64_t off, uint8_t *buf)
{
    if (off + SECTOR > (int64_t)d->blk->size) { /* imagem menor que o formato */
        memset(buf, 0, SECTOR);
        if (off < (int64_t)d->blk->size)
            return blk_read(d->blk, (uint64_t)off, buf, (size_t)(d->blk->size - (uint64_t)off));
        return 0;
    }
    return blk_read(d->blk, (uint64_t)off, buf, SECTOR);
}

static int write_sector(fd_drive *d, int64_t off, const uint8_t *buf)
{
    if (off + SECTOR > (int64_t)d->blk->size)
        return -1;
    return blk_write(d->blk, (uint64_t)off, buf, SECTOR);
}

/* ---------------------------------------------------------------- leitura/escrita */

/* resultado padrao de 7 bytes: ST0 ST1 ST2 C H R N */
static void rw_result(fdc *f, uint8_t st0, uint8_t st1, uint8_t st2, uint8_t c, uint8_t h, uint8_t r, uint8_t n)
{
    f->res[0] = st0;
    f->res[1] = st1;
    f->res[2] = st2;
    f->res[3] = c;
    f->res[4] = h;
    f->res[5] = r;
    f->res[6] = n;
    to_result(f, 7);
    raise_irq(f);
}

static void rw_error(fdc *f, uint8_t st1, uint8_t st2)
{
    uint8_t hds = f->cmd[1];
    rw_result(f, ST0_ABNORMAL | (hds & 7), st1, st2, f->cmd[2], f->cmd[3], f->cmd[4], f->cmd[5]);
}

/* Quantos setores a operacao cobre: de (C, H, R) ate EOT; com MT continua na face 1. */
static int sector_count(fdc *f, fd_drive *d)
{
    int h = f->cmd[3] & 1, r = f->cmd[4];
    int eot = f->cmd[6] > d->spt ? d->spt : f->cmd[6];
    if (r > eot)
        return 0;
    int n = eot - r + 1;
    if ((f->cmd[0] & 0x80) && h == 0 && d->heads > 1)
        n += eot;
    return n;
}

/* posicao inicial da transferencia: C/H/R do comando */
static int64_t xfer_sector_offset(fdc *f, fd_drive *d, int idx)
{
    int c = f->cmd[2], h = f->cmd[3] & 1, r = f->cmd[4] + idx;
    int eot = f->cmd[6] > d->spt ? d->spt : f->cmd[6];
    if (r > eot) { /* MT: segunda face */
        r -= eot;
        h = 1;
    }
    return chs_offset(d, c, h, r);
}

static void finish_rw(fdc *f)
{
    fd_drive *d = cur_drive(f, f->cmd[1]);
    int sectors = f->xfer_pos / SECTOR;
    uint8_t hds = f->cmd[1];
    uint8_t st0 = (uint8_t)(hds & 7);

    if (f->xfer_write) {
        for (int i = 0; i < sectors; i++) {
            int64_t off = xfer_sector_offset(f, d, i);
            if (off < 0 || write_sector(d, off, f->buf + i * SECTOR) < 0) {
                rw_error(f, ST1_ND, 0);
                return;
            }
        }
        if (sectors)
            blk_flush(d->blk);
    }

    /* proximo setor depois do ultimo transferido (tabela do 82077): no fim da
     * trilha R volta a 1; com MT a face troca e o cilindro so avanca depois da face 1 */
    int c = f->cmd[2], h = f->cmd[3] & 1;
    int eot = f->cmd[6] > d->spt ? d->spt : f->cmd[6];
    bool mt = f->cmd[0] & 0x80;
    int pos = f->cmd[4] - 1 + sectors;
    if (mt && h == 0 && d->heads > 1 && pos >= eot) {
        pos -= eot;
        h = 1;
    }
    if (pos >= eot) {
        pos = 0;
        c++;
        if (mt)
            h ^= 1;
    }
    int r = pos + 1;
    d->cyl = f->cmd[2];
    d->head = (uint8_t)h;
    d->sect = (uint8_t)r;
    f->xfer_len = f->xfer_pos = 0;
    rw_result(f, st0, 0, 0, (uint8_t)c, (uint8_t)h, (uint8_t)r, f->cmd[5]);
}

static void dma_transfer(fdc *f)
{
    if (!i8237_ready(f->dma, FDC_DMA_CH)) {
        /* o driver ainda nao liberou o canal: tenta de novo (ate ~0,5 s) */
        if (++f->dma_waits < 5000) {
            schedule(f, FDC_DMA_RETRY_NS, dma_transfer);
            return;
        }
        rw_error(f, ST1_EC, 0);
        return;
    }
    uint32_t n;
    if (f->xfer_write)
        n = i8237_read_mem(f->dma, FDC_DMA_CH, f->buf, (uint32_t)f->xfer_len);
    else
        n = i8237_write_mem(f->dma, FDC_DMA_CH, f->buf, (uint32_t)f->xfer_len);
    /* setores completos; o TC do DMA encerra antes do EOT */
    f->xfer_pos = (int)(n / SECTOR) * SECTOR;
    if (!f->xfer_pos && n)
        f->xfer_pos = SECTOR;
    finish_rw(f);
}

static void start_rw(fdc *f, bool write)
{
    fd_drive *d = cur_drive(f, f->cmd[1]);
    uint8_t n = f->cmd[5];

    if (!drive_present(d)) {
        rw_error(f, ST1_MA, 0);
        return;
    }
    if (f->rate != d->rate || n != 2) { /* taxa errada: nao acha as marcas */
        rw_error(f, ST1_MA, 0);
        return;
    }
    if (write && d->readonly) {
        rw_error(f, ST1_NW, 0);
        return;
    }
    int count = sector_count(f, d);
    if (count <= 0 || chs_offset(d, f->cmd[2], f->cmd[3] & 1, f->cmd[4]) < 0) {
        rw_error(f, ST1_ND, 0);
        return;
    }
    f->xfer_len = count * SECTOR;
    f->xfer_pos = 0;
    f->xfer_write = write;
    f->xfer_pio = f->nodma;
    f->dma_waits = 0;
    if (!write) {
        for (int i = 0; i < count; i++) {
            int64_t off = xfer_sector_offset(f, d, i);
            if (off < 0 || read_sector(d, off, f->buf + i * SECTOR) < 0) {
                rw_error(f, ST1_ND, 0);
                return;
            }
        }
    }
    f->phase = PH_EXEC;
    if (f->xfer_pio) {
        f->msr = MSR_RQM | MSR_NDMA | MSR_CB | (write ? 0 : MSR_DIO);
        raise_irq(f); /* modo sem DMA: a IRQ pede os dados */
    } else {
        f->msr = MSR_CB;
        schedule(f, FDC_DELAY_NS, dma_transfer);
    }
}

/* ---------------------------------------------------------------- formatacao */

static void format_done(fdc *f)
{
    fd_drive *d = cur_drive(f, f->cmd[1]);
    int h = (f->cmd[1] >> 2) & 1;
    int sc = f->cmd[3];
    uint8_t fill = f->cmd[5];
    uint8_t ids[4 * MAX_SPT];
    uint32_t got = 0;
    if (f->xfer_pio) {
        memcpy(ids, f->buf, (size_t)f->xfer_pos);
        got = (uint32_t)f->xfer_pos;
    } else {
        if (!i8237_ready(f->dma, FDC_DMA_CH)) {
            if (++f->dma_waits < 5000) {
                schedule(f, FDC_DMA_RETRY_NS, format_done);
                return;
            }
            rw_error(f, ST1_EC, 0);
            return;
        }
        got = i8237_read_mem(f->dma, FDC_DMA_CH, ids, (uint32_t)(4 * sc));
    }
    uint8_t sec[SECTOR];
    memset(sec, fill, sizeof(sec));
    int c = d->cyl;
    for (int i = 0; i < sc && (uint32_t)(i * 4 + 4) <= got; i++) {
        c = ids[i * 4];
        int64_t off = chs_offset(d, ids[i * 4], ids[i * 4 + 1] & 1, ids[i * 4 + 2]);
        if (off < 0 || write_sector(d, off, sec) < 0) {
            rw_result(f, ST0_ABNORMAL | (f->cmd[1] & 7), ST1_ND, 0, (uint8_t)c, (uint8_t)h, 1, f->cmd[2]);
            return;
        }
    }
    blk_flush(d->blk);
    f->xfer_len = f->xfer_pos = 0;
    rw_result(f, (uint8_t)(f->cmd[1] & 7), 0, 0, (uint8_t)c, (uint8_t)h, 1, f->cmd[2]);
}

static void start_format(fdc *f)
{
    fd_drive *d = cur_drive(f, f->cmd[1]);
    int sc = f->cmd[3];
    if (!drive_present(d) || f->rate != d->rate || f->cmd[2] != 2) {
        rw_result(f, ST0_ABNORMAL | (f->cmd[1] & 7), ST1_MA, 0, d->cyl, 0, 1, f->cmd[2]);
        return;
    }
    if (d->readonly) {
        rw_result(f, ST0_ABNORMAL | (f->cmd[1] & 7), ST1_NW, 0, d->cyl, 0, 1, f->cmd[2]);
        return;
    }
    if (sc < 1 || sc > MAX_SPT) {
        rw_result(f, ST0_ABNORMAL | (f->cmd[1] & 7), ST1_ND, 0, d->cyl, 0, 1, f->cmd[2]);
        return;
    }
    f->xfer_len = 4 * sc;
    f->xfer_pos = 0;
    f->xfer_write = true;
    f->xfer_pio = f->nodma;
    f->dma_waits = 0;
    f->phase = PH_EXEC;
    if (f->xfer_pio) {
        f->msr = MSR_RQM | MSR_NDMA | MSR_CB;
        raise_irq(f);
    } else {
        f->msr = MSR_CB;
        schedule(f, FDC_DELAY_NS, format_done);
    }
}

/* ---------------------------------------------------------------- seek */

static void seek_done(void *opaque)
{
    fdc *f = opaque;
    f->int_valid = true;
    raise_irq(f);
}

static void do_seek(fdc *f, int drive, int cyl)
{
    fd_drive *d = &f->drv[drive];
    if (cyl < 0)
        cyl = 0;
    if (cyl > 255)
        cyl = 255;
    d->cyl = (uint8_t)cyl;
    /* com midia, um seek desliga a linha de troca de midia */
    if (drive_present(d))
        d->changed = false;
    f->int_drive = drive;
    f->int_st0 = (uint8_t)(ST0_SEEK | drive | (f->cmd[1] & 4));
    f->int_valid = false;
    to_command(f);
    timer_mod(f->vm, &f->seek_timer, mvm_now(f->vm) + FDC_SEEK_NS);
}

/* ---------------------------------------------------------------- comandos */

static int cmd_length(uint8_t c)
{
    switch (c) {
    case 0x8f: case 0xcf: return 3; /* seek relativo */
    case CMD_LOCK: case 0x94: return 1;
    }
    switch (c & 0x1f) {
    case CMD_READ_TRACK: case CMD_WRITE: case CMD_READ:
    case 0x09: case 0x0c: /* com marca de apagado */
        return 9;
    case CMD_SPECIFY: return 3;
    case CMD_SENSE_DRIVE: return 2;
    case CMD_RECAL: return 2;
    case CMD_SENSE_INT: return 1;
    case CMD_READ_ID: return 2;
    case CMD_FORMAT: return 6;
    case CMD_DUMPREG: return 1;
    case CMD_SEEK: return 3;
    case CMD_VERSION: return 1;
    case CMD_PERPENDICULAR: return 2;
    case CMD_CONFIGURE: return 4;
    case CMD_PART_ID: return 1;
    default: return 1;
    }
}

static void exec_command(fdc *f)
{
    uint8_t c = f->cmd[0];
    fd_drive *d = cur_drive(f, f->cmd[1]);

    if (c == 0x8f || c == 0xcf) { /* seek relativo */
        int drive = f->cmd[1] & 1;
        int delta = f->cmd[2];
        do_seek(f, drive, f->drv[drive].cyl + (c & 0x40 ? -delta : delta));
        return;
    }
    if (c == CMD_LOCK || c == 0x94) {
        f->lock = c & 0x80;
        f->res[0] = (uint8_t)(f->lock ? 0x10 : 0);
        to_result(f, 1);
        return;
    }
    switch (c & 0x1f) {
    case CMD_READ:
    case CMD_READ_TRACK:
    case 0x0c:
        start_rw(f, false);
        return;
    case CMD_WRITE:
    case 0x09:
        start_rw(f, true);
        return;
    case CMD_FORMAT:
        start_format(f);
        return;
    case CMD_SPECIFY:
        f->specify[0] = f->cmd[1];
        f->specify[1] = f->cmd[2];
        f->nodma = f->cmd[2] & 1;
        to_command(f);
        return;
    case CMD_SENSE_DRIVE: {
        uint8_t st3 = (uint8_t)((f->cmd[1] & 7) | ST3_RY);
        if (d->cyl == 0) st3 |= ST3_T0;
        if (d->heads > 1 || !drive_present(d)) st3 |= ST3_TS;
        if (d->readonly) st3 |= ST3_WP;
        f->res[0] = st3;
        to_result(f, 1);
        return;
    }
    case CMD_RECAL:
        do_seek(f, f->cmd[1] & 1, 0);
        return;
    case CMD_SEEK:
        do_seek(f, f->cmd[1] & 1, f->cmd[2]);
        return;
    case CMD_SENSE_INT:
        if (f->reset_sense > 0) {
            int drv = 4 - f->reset_sense;
            f->reset_sense--;
            f->res[0] = (uint8_t)(ST0_RDYCHG | drv);
            f->res[1] = drv < FDC_DRIVES ? f->drv[drv].cyl : 0;
            to_result(f, 2);
        } else if (f->int_valid) {
            f->int_valid = false;
            f->res[0] = f->int_st0;
            f->res[1] = f->drv[f->int_drive].cyl;
            to_result(f, 2);
        } else {
            f->res[0] = ST0_INVALID;
            to_result(f, 1);
        }
        lower_irq(f);
        return;
    case CMD_READ_ID: {
        int h = (f->cmd[1] >> 2) & 1;
        if (!drive_present(d) || f->rate != d->rate) {
            rw_result(f, ST0_ABNORMAL | (f->cmd[1] & 7), ST1_MA, 0, d->cyl, (uint8_t)h, 1, 2);
            return;
        }
        if (d->sect < 1 || d->sect > d->spt)
            d->sect = 1;
        uint8_t r = d->sect;
        d->sect = (uint8_t)(d->sect % d->spt + 1); /* o disco "gira" */
        f->phase = PH_EXEC;
        f->msr = MSR_CB;
        rw_result(f, (uint8_t)(f->cmd[1] & 7), 0, 0, d->cyl, (uint8_t)h, r, 2);
        return;
    }
    case CMD_DUMPREG:
        f->res[0] = f->drv[0].cyl;
        f->res[1] = f->drv[1].cyl;
        f->res[2] = 0;
        f->res[3] = 0;
        f->res[4] = f->specify[0];
        f->res[5] = f->specify[1];
        f->res[6] = d->spt;
        f->res[7] = (uint8_t)((f->lock ? 0x80 : 0) | (f->perp & 0x7f));
        f->res[8] = f->config;
        f->res[9] = f->pretrk;
        to_result(f, 10);
        return;
    case CMD_VERSION:
        f->res[0] = 0x90; /* 82077/82078 */
        to_result(f, 1);
        return;
    case CMD_PART_ID:
        f->res[0] = 0x41; /* 82078 */
        to_result(f, 1);
        return;
    case CMD_PERPENDICULAR:
        if (f->cmd[1] & 0x80)
            f->perp = f->cmd[1] & 0x7f;
        to_command(f);
        return;
    case CMD_CONFIGURE:
        f->config = f->cmd[2];
        f->pretrk = f->cmd[3];
        to_command(f);
        return;
    default:
        f->res[0] = ST0_INVALID;
        to_result(f, 1);
        return;
    }
}

/* ---------------------------------------------------------------- FIFO */

static uint8_t fifo_read(fdc *f)
{
    if (f->phase == PH_EXEC && f->xfer_pio && !f->xfer_write) {
        uint8_t v = f->buf[f->xfer_pos++];
        lower_irq(f);
        if (f->xfer_pos >= f->xfer_len)
            finish_rw(f);
        return v;
    }
    if (f->phase != PH_RESULT)
        return 0;
    uint8_t v = f->res[f->res_pos++];
    lower_irq(f); /* ler o resultado reconhece a interrupcao */
    if (f->res_pos >= f->res_len)
        to_command(f);
    return v;
}

static void fifo_write(fdc *f, uint8_t v)
{
    if (f->phase == PH_EXEC && f->xfer_pio && f->xfer_write) {
        f->buf[f->xfer_pos++] = v;
        lower_irq(f);
        if (f->xfer_pos >= f->xfer_len) {
            if ((f->cmd[0] & 0x1f) == CMD_FORMAT)
                format_done(f);
            else
                finish_rw(f);
        }
        return;
    }
    if (f->phase == PH_RESULT) { /* novo comando descarta o resultado nao lido */
        to_command(f);
    }
    if (f->phase != PH_CMD)
        return;
    if (f->cmd_pos == 0) {
        f->cmd_len = cmd_length(v);
        f->msr = MSR_RQM | MSR_CB;
    }
    f->cmd[f->cmd_pos++] = v;
    if (f->cmd_pos >= f->cmd_len) {
        f->msr = MSR_CB;
        exec_command(f);
    }
}

/* ---------------------------------------------------------------- portas */

static uint64_t fdc_read(void *opaque, uint64_t off, unsigned size)
{
    UNUSED(size);
    fdc *f = opaque;
    switch (off) {
    case 0: /* SRA (PS/2): bit 7 = interrupcao pendente */
        return f->irq_pending ? 0x80 : 0x00;
    case 1: /* SRB */
        return 0xc0 | (f->dor & 1 ? 0x20 : 0x00);
    case 2: return f->dor;
    case 3: return f->tdr;
    case 4: return f->msr;
    case 5: return fifo_read(f);
    case 7: { /* DIR: linha de troca de midia do drive selecionado */
        fd_drive *d = &f->drv[f->dor & 1];
        return d->changed ? 0x80 : 0x00;
    }
    default: return 0xff;
    }
}

static void fdc_write(void *opaque, uint64_t off, uint64_t val, unsigned size)
{
    UNUSED(size);
    fdc *f = opaque;
    uint8_t v = (uint8_t)val;
    switch (off) {
    case 2: { /* DOR */
        uint8_t old = f->dor;
        f->dor = v;
        if (!(v & DOR_NRESET)) {
            soft_reset(f);
            f->msr = 0;
        } else if (!(old & DOR_NRESET)) {
            /* saiu do reset: interrupcao com 4 SENSE INTERRUPT pendentes */
            soft_reset(f);
            f->reset_sense = 4;
            raise_irq(f);
        }
        update_irq(f);
        break;
    }
    case 3: f->tdr = v & 3; break;
    case 4: /* DSR */
        f->dsr = v;
        f->rate = v & 3;
        if (v & 0x80) { /* reset por software */
            soft_reset(f);
            f->reset_sense = 4;
            raise_irq(f);
        }
        break;
    case 5: fifo_write(f, v); break;
    case 7: f->rate = v & 3; break; /* CCR */
    }
}

const mvm_io_ops fdc_ops = {fdc_read, fdc_write};

/* 0x3f7 (DIR/CCR) fica numa regiao separada: a 0x3f6 e do IDE */
static uint64_t fdc_dir_read(void *opaque, uint64_t off, unsigned size) { UNUSED(off); return fdc_read(opaque, 7, size); }
static void fdc_dir_write(void *opaque, uint64_t off, uint64_t val, unsigned size) { UNUSED(off); fdc_write(opaque, 7, val, size); }
const mvm_io_ops fdc_dir_ops = {fdc_dir_read, fdc_dir_write};

/* ---------------------------------------------------------------- ciclo de vida */

fdc *fdc_new(mvm_vm *vm, irq_line irq6, i8237 *dma)
{
    fdc *f = calloc(1, sizeof(*f));
    f->vm = vm;
    f->irq = irq6;
    f->dma = dma;
    f->buf = malloc(XFER_MAX);
    timer_init(&f->timer, timer_cb, f);
    timer_init(&f->seek_timer, seek_done, f);
    fdc_reset(f);
    return f;
}

/* poe (ou tira, com blk NULL) o disquete do drive; a linha de troca de midia acende */
static void set_media(fd_drive *d, int drive, mvm_blk *blk)
{
    const fd_format *fmt = format_for(blk ? blk->size : 1474560);
    d->blk = blk;
    d->readonly = blk ? blk->readonly : false;
    d->tracks = fmt->tracks;
    d->heads = fmt->heads;
    d->spt = fmt->spt;
    d->rate = fmt->rate;
    d->changed = true;
    d->sect = 1;
    if (blk && fmt->size != blk->size)
        LOGW("disquete %c: tamanho %llu fora do padrao; usando %u trilhas x %u faces x %u setores",
             'A' + drive, (unsigned long long)blk->size, fmt->tracks, fmt->heads, fmt->spt);
}

bool fdc_attach(fdc *f, int drive, mvm_blk *blk)
{
    if (drive < 0 || drive >= FDC_DRIVES || f->drv[drive].exists)
        return false;
    f->drv[drive].exists = true;
    set_media(&f->drv[drive], drive, blk);
    return true;
}

bool fdc_change_media(fdc *f, int drive, mvm_blk *blk)
{
    if (drive < 0 || drive >= FDC_DRIVES || !f->drv[drive].exists)
        return false;
    set_media(&f->drv[drive], drive, blk);
    return true;
}

bool fdc_has_drive(fdc *f, int drive)
{
    return drive >= 0 && drive < FDC_DRIVES && f->drv[drive].exists;
}

void fdc_reset(fdc *f)
{
    f->lock = false;
    soft_reset(f);
    f->dor = DOR_NRESET | DOR_DMAEN;
    f->tdr = 0;
    f->dsr = 0;
    f->rate = 0;
    f->reset_sense = 0;
    f->specify[0] = f->specify[1] = 0;
    for (int i = 0; i < FDC_DRIVES; i++) {
        f->drv[i].cyl = 0;
        f->drv[i].head = 0;
        f->drv[i].sect = 1;
        /* a linha de troca de midia e do drive: o reset do controlador nao mexe nela */
    }
}

void fdc_free(fdc *f)
{
    if (!f)
        return;
    timer_del(f->vm, &f->timer);
    timer_del(f->vm, &f->seek_timer);
    free(f->buf);
    free(f);
}
