/*
 * Controlador SATA AHCI 1.2 (como o ICH9 AHCI, PCI 8086:2922, classe 01.06.01).
 * ABAR = BAR5 (4 KiB). Seis portas; cada uma com um disco ATA ou um CD ATAPI.
 * Os comandos ATA/ATAPI sao executados pelo nucleo do IDE (ide_port_exec); aqui
 * ficam os registradores, a lista de comandos, os FIS recebidos e a PRDT.
 * Os comandos sao executados de forma sincrona quando o SO escreve em PxCI.
 */
#include "ahci.h"

#include <stdlib.h>

#include "ide.h"

/* registradores globais */
#define HBA_CAP 0x00
#define HBA_GHC 0x04
#define HBA_IS 0x08
#define HBA_PI 0x0c
#define HBA_VS 0x10
#define HBA_CAP2 0x24
#define HBA_BOHC 0x28

#define GHC_HR (1u << 0)
#define GHC_IE (1u << 1)
#define GHC_AE (1u << 31)

/* registradores de porta (offset relativo a 0x100 + 0x80 * n) */
#define PX_CLB 0x00
#define PX_CLBU 0x04
#define PX_FB 0x08
#define PX_FBU 0x0c
#define PX_IS 0x10
#define PX_IE 0x14
#define PX_CMD 0x18
#define PX_TFD 0x20
#define PX_SIG 0x24
#define PX_SSTS 0x28
#define PX_SCTL 0x2c
#define PX_SERR 0x30
#define PX_SACT 0x34
#define PX_CI 0x38
#define PX_SNTF 0x3c

#define CMD_ST (1u << 0)
#define CMD_SUD (1u << 1)
#define CMD_POD (1u << 2)
#define CMD_CLO (1u << 3)
#define CMD_FRE (1u << 4)
#define CMD_FR (1u << 14)
#define CMD_CR (1u << 15)
#define CMD_ATAPI (1u << 24)

#define IS_DHRS (1u << 0)
#define IS_PSS (1u << 1)
#define IS_DPS (1u << 5)
#define IS_PCS (1u << 6)
#define IS_TFES (1u << 30)

#define ST_ERR 0x01
#define ST_DRQ 0x08
#define ST_BSY 0x80

typedef struct {
    struct ahci *h;
    int n;
    ide_port *dev; /* NULL = porta sem dispositivo */
    uint32_t clb, clbu, fb, fbu, is, ie, cmd, tfd, sig, sctl, serr, sact, ci, sntf;
} ahci_port;

struct ahci {
    mvm_vm *vm;
    irq_line irq;
    uint32_t ghc, is;
    int level;
    ahci_port port[AHCI_PORTS];
};

static uint64_t clb_addr(ahci_port *p) { return ((uint64_t)p->clbu << 32) | p->clb; }
static uint64_t fb_addr(ahci_port *p) { return ((uint64_t)p->fbu << 32) | p->fb; }

static void update_irq(ahci *h)
{
    uint32_t is = 0;
    for (int i = 0; i < AHCI_PORTS; i++)
        if (h->port[i].is & h->port[i].ie)
            is |= 1u << i;
    h->is |= is;
    int level = (h->ghc & GHC_IE) && h->is;
    if (level != h->level) {
        h->level = level;
        irq_set(&h->irq, level);
    }
}

/* grava um FIS na area de FIS recebidos da porta */
static void post_fis(ahci_port *p, unsigned off, const uint8_t *fis, unsigned len)
{
    if (!(p->cmd & CMD_FRE))
        return;
    space_memwrite(&p->h->vm->mem, fb_addr(p) + off, fis, len);
}

static void post_d2h(ahci_port *p, const ide_port_result *r, bool irq_bit)
{
    uint8_t f[20] = {0};
    f[0] = 0x34;                       /* D2H register FIS */
    f[1] = irq_bit ? 0x40 : 0;
    f[2] = r->status;
    f[3] = r->error;
    f[4] = r->sector;
    f[5] = r->lcyl;
    f[6] = r->hcyl;
    f[7] = r->device;
    f[8] = r->hob_sector;
    f[9] = r->hob_lcyl;
    f[10] = r->hob_hcyl;
    f[12] = r->nsector;
    f[13] = r->hob_nsector;
    post_fis(p, 0x40, f, sizeof(f));
    p->tfd = ((uint32_t)r->error << 8) | r->status;
}

static void post_pio_setup(ahci_port *p, const ide_port_result *r, uint32_t bytes)
{
    uint8_t f[20] = {0};
    f[0] = 0x5f;                       /* PIO Setup FIS */
    f[1] = 0x40 | 0x20;                /* I, D (dispositivo para host) */
    f[2] = r->status;
    f[3] = r->error;
    f[4] = r->sector;
    f[5] = r->lcyl;
    f[6] = r->hcyl;
    f[7] = r->device;
    f[12] = r->nsector;
    f[15] = r->status;                 /* E_Status */
    f[16] = (uint8_t)bytes;
    f[17] = (uint8_t)(bytes >> 8);
    post_fis(p, 0x20, f, sizeof(f));
}

/* assinatura inicial (D2H) apos reset/conexao */
static void post_signature(ahci_port *p)
{
    if (!p->dev)
        return;
    uint32_t sig = ide_port_signature(p->dev);
    ide_port_result r = {0};
    r.status = ide_port_kind(p->dev) == 2 ? 0x00 : 0x50;
    r.error = 1;
    r.nsector = (uint8_t)sig;
    r.sector = (uint8_t)(sig >> 8);
    r.lcyl = (uint8_t)(sig >> 16);
    r.hcyl = (uint8_t)(sig >> 24);
    post_d2h(p, &r, false);
    p->sig = sig;
}

/* PRDT: entradas de 16 bytes (endereco de 64 bits, DBC em bits 0-21) */
typedef struct {
    mvm_space *mem;
    uint64_t prdt;
    uint32_t n, idx;
    uint64_t addr;
    uint32_t left;
} ahci_sg;

static uint32_t ahci_xfer(void *opaque, uint8_t *data, uint32_t len, bool to_host)
{
    ahci_sg *s = opaque;
    uint32_t done = 0;
    while (done < len) {
        if (!s->left) {
            if (s->idx >= s->n)
                break;
            uint64_t e = s->prdt + 16ULL * s->idx++;
            uint32_t lo = (uint32_t)space_read(s->mem, e, 4), hi = (uint32_t)space_read(s->mem, e + 4, 4);
            uint32_t dw3 = (uint32_t)space_read(s->mem, e + 12, 4);
            s->addr = ((uint64_t)hi << 32) | (lo & ~1u);
            s->left = (dw3 & 0x3fffff) + 1;
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

static void exec_slot(ahci_port *p, int slot)
{
    mvm_space *mem = &p->h->vm->mem;
    uint64_t hdr = clb_addr(p) + 32ULL * (unsigned)slot;
    uint32_t dw0 = (uint32_t)space_read(mem, hdr, 4);
    uint64_t ctba = ((uint64_t)space_read(mem, hdr + 12, 4) << 32) | ((uint32_t)space_read(mem, hdr + 8, 4) & ~0x7fu);
    uint8_t cfis[64], acmd[16];
    space_memread(mem, ctba, cfis, sizeof(cfis));
    space_memread(mem, ctba + 0x40, acmd, sizeof(acmd));
    ide_port_result r = {0};

    if (cfis[0] != 0x27 || !(cfis[1] & 0x80)) {
        /* FIS de controle (SRST etc.): conclui sem comando */
        if (cfis[0] == 0x27 && (cfis[15] & 0x04) == 0 && p->dev) { /* fim de SRST */
            ide_port_reset(p->dev);
            post_signature(p);
            p->tfd = ide_port_kind(p->dev) == 2 ? 0x100 : 0x150;
        }
        space_write(mem, hdr + 4, 0, 4);
        p->ci &= ~(1u << slot);
        return;
    }
    if (!p->dev) {
        r.status = 0x41;
        r.error = 0x04;
        post_d2h(p, &r, true);
        p->is |= IS_DHRS | IS_TFES;
    } else {
        ahci_sg sg = {mem, ctba + 0x80, dw0 >> 16, 0, 0, 0};
        ide_port_exec(p->dev, cfis, acmd, ahci_xfer, &sg, &r);
        space_write(mem, hdr + 4, r.bytes, 4); /* PRDBC */
        if (r.pio) {
            post_pio_setup(p, &r, r.bytes);
            p->is |= IS_PSS;
        }
        post_d2h(p, &r, true);
        p->is |= IS_DHRS;
        if (r.status & ST_ERR)
            p->is |= IS_TFES;
        if (dw0 & (1u << 9)) /* PRD interrupt no ultimo descritor: DPS */
            p->is |= IS_DPS;
    }
    p->ci &= ~(1u << slot);
}

static void port_run(ahci_port *p)
{
    if (!(p->cmd & CMD_ST))
        return;
    for (int slot = 0; slot < 32 && p->ci; slot++) {
        if (!(p->ci & (1u << slot)))
            continue;
        exec_slot(p, slot);
        if (p->is & IS_TFES) { /* erro: o SO deve reiniciar a porta (ST=0) */
            break;
        }
    }
    update_irq(p->h);
}

static void port_reset(ahci_port *p)
{
    p->is = p->ie = 0;
    p->cmd = CMD_SUD | CMD_POD | (1u << 18); /* HPCP: porta aceita hot-plug */
    p->serr = p->sact = p->ci = p->sntf = 0;
    p->sctl = 0;
    if (p->dev) {
        ide_port_reset(p->dev);
        p->sig = ide_port_signature(p->dev);
        p->tfd = ide_port_kind(p->dev) == 2 ? 0x100 : 0x150;
        if (ide_port_kind(p->dev) == 2)
            p->cmd |= CMD_ATAPI;
    } else {
        p->sig = 0xffffffffu;
        p->tfd = 0x7f;
    }
}

static uint32_t port_read(ahci_port *p, unsigned reg)
{
    switch (reg) {
    case PX_CLB: return p->clb;
    case PX_CLBU: return p->clbu;
    case PX_FB: return p->fb;
    case PX_FBU: return p->fbu;
    case PX_IS: return p->is;
    case PX_IE: return p->ie;
    case PX_CMD: {
        uint32_t v = p->cmd & ~(CMD_FR | CMD_CR);
        if (p->cmd & CMD_FRE) v |= CMD_FR;
        if (p->cmd & CMD_ST) v |= CMD_CR;
        return v;
    }
    case PX_TFD: return p->tfd;
    case PX_SIG: return p->sig;
    case PX_SSTS: return p->dev && (p->sctl & 0xf) != 1 ? 0x113 : 0; /* IPM ativo, Gen1, dispositivo presente */
    case PX_SCTL: return p->sctl;
    case PX_SERR: return p->serr;
    case PX_SACT: return p->sact;
    case PX_CI: return p->ci;
    case PX_SNTF: return p->sntf;
    default: return 0;
    }
}

static void port_write(ahci_port *p, unsigned reg, uint32_t v)
{
    switch (reg) {
    case PX_CLB: p->clb = v & ~0x3ffu; break;
    case PX_CLBU: p->clbu = v; break;
    case PX_FB: p->fb = v & ~0xffu; break;
    case PX_FBU: p->fbu = v; break;
    case PX_IS: p->is &= ~v; update_irq(p->h); break;
    case PX_IE: p->ie = v & 0xfdc000ffu; update_irq(p->h); break;
    case PX_CMD: {
        uint32_t old = p->cmd;
        p->cmd = v & ~(CMD_FR | CMD_CR | CMD_CLO | 0x1f00u); /* FR/CR/CCS sao so leitura */
        if (v & CMD_CLO) /* command list override: limpa BSY e DRQ */
            p->tfd &= ~(uint32_t)(ST_BSY | ST_DRQ);
        if (!(p->cmd & CMD_ST))
            p->ci = 0;
        if ((p->cmd & CMD_FRE) && !(old & CMD_FRE))
            post_signature(p);
        if ((p->cmd & CMD_ST) && !(old & CMD_ST))
            port_run(p);
        break;
    }
    case PX_SCTL: {
        uint32_t old = p->sctl;
        p->sctl = v;
        if ((old & 0xf) == 1 && (v & 0xf) == 0) { /* fim do COMRESET */
            if (p->dev) {
                ide_port_reset(p->dev);
                p->tfd = ide_port_kind(p->dev) == 2 ? 0x100 : 0x150;
                post_signature(p);
                p->serr |= 1u << 26; /* DIAG.X: dispositivo presente mudou */
                p->is |= IS_PCS;
                update_irq(p->h);
            }
        }
        break;
    }
    case PX_SERR: p->serr &= ~v; break;
    case PX_SACT: p->sact |= v; break;
    case PX_CI:
        p->ci |= v;
        port_run(p);
        break;
    case PX_SNTF: p->sntf &= ~v; break;
    default: break;
    }
}

static uint32_t reg_read(ahci *h, unsigned off)
{
    if (off >= 0x100 && off < 0x100 + 0x80 * AHCI_PORTS)
        return port_read(&h->port[(off - 0x100) >> 7], (off - 0x100) & 0x7f);
    switch (off) {
    case HBA_CAP:
        return (AHCI_PORTS - 1) | (31u << 8) | (1u << 18) /* so AHCI */ | (1u << 20) /* Gen1 */ |
               (1u << 24) /* CLO */ | (1u << 31) /* 64 bits */;
    case HBA_GHC: return h->ghc | GHC_AE;
    case HBA_IS: return h->is;
    case HBA_PI: return (1u << AHCI_PORTS) - 1;
    case HBA_VS: return 0x00010200;
    case HBA_CAP2: return 0;
    case HBA_BOHC: return 0;
    default: return 0;
    }
}

static void hba_reset(ahci *h)
{
    h->ghc = GHC_AE;
    h->is = 0;
    for (int i = 0; i < AHCI_PORTS; i++)
        port_reset(&h->port[i]);
    update_irq(h);
}

static void reg_write(ahci *h, unsigned off, uint32_t v)
{
    if (off >= 0x100 && off < 0x100 + 0x80 * AHCI_PORTS) {
        port_write(&h->port[(off - 0x100) >> 7], (off - 0x100) & 0x7f, v);
        return;
    }
    switch (off) {
    case HBA_GHC:
        if (v & GHC_HR) {
            hba_reset(h);
            return;
        }
        h->ghc = (v & GHC_IE) | GHC_AE;
        update_irq(h);
        break;
    case HBA_IS:
        h->is &= ~v;
        update_irq(h);
        break;
    default: break;
    }
}

static uint64_t ahci_read(void *opaque, uint64_t off, unsigned size)
{
    ahci *h = opaque;
    uint32_t v = reg_read(h, (unsigned)off & ~3u);
    if (size == 8)
        return v | ((uint64_t)reg_read(h, ((unsigned)off & ~3u) + 4) << 32);
    v >>= 8 * (off & 3);
    return size >= 4 ? v : v & ((1u << (8 * size)) - 1);
}

static void ahci_write(void *opaque, uint64_t off, uint64_t v, unsigned size)
{
    ahci *h = opaque;
    if (size == 8) {
        reg_write(h, (unsigned)off, (uint32_t)v);
        reg_write(h, (unsigned)off + 4, (uint32_t)(v >> 32));
        return;
    }
    if (size < 4) { /* acessos parciais: le-modifica-escreve (nao em registradores RW1C) */
        unsigned o = (unsigned)off & ~3u, sh = 8 * (unsigned)(off & 3);
        uint32_t m = ((1u << (8 * size)) - 1) << sh;
        unsigned r = o >= 0x100 ? ((o - 0x100) & 0x7f) : o;
        bool rw1c = o >= 0x100 ? (r == PX_IS || r == PX_SERR || r == PX_CI || r == PX_SACT) : o == HBA_IS;
        uint32_t cur = rw1c ? 0 : reg_read(h, o);
        reg_write(h, o, (cur & ~m) | (((uint32_t)v << sh) & m));
        return;
    }
    reg_write(h, (unsigned)off, (uint32_t)v);
}

const mvm_io_ops ahci_mmio_ops = {ahci_read, ahci_write};

ahci *ahci_new(mvm_vm *vm, irq_line irq)
{
    ahci *h = calloc(1, sizeof(*h));
    h->vm = vm;
    h->irq = irq;
    for (int i = 0; i < AHCI_PORTS; i++) {
        h->port[i].h = h;
        h->port[i].n = i;
    }
    hba_reset(h);
    return h;
}

bool ahci_attach(ahci *h, int port, mvm_blk *blk, bool cdrom)
{
    if (port < 0 || port >= AHCI_PORTS || h->port[port].dev)
        return false;
    h->port[port].dev = ide_port_new(h->vm, blk, cdrom);
    port_reset(&h->port[port]);
    return true;
}

/* sinaliza ao SO que a porta mudou (conexao ou desconexao) */
static void port_changed(ahci_port *p, bool connected)
{
    p->serr |= (1u << 16) | (connected ? (1u << 26) : 0); /* DIAG.N (PhyRdy), DIAG.X (troca) */
    p->is |= (1u << 22) | (connected ? IS_PCS : 0);      /* PRCS, PCS */
    if (connected) {
        p->sig = ide_port_signature(p->dev);
        p->tfd = ide_port_kind(p->dev) == 2 ? 0x100 : 0x150;
        if (ide_port_kind(p->dev) == 2)
            p->cmd |= CMD_ATAPI;
        else
            p->cmd &= ~CMD_ATAPI;
    } else {
        p->sig = 0xffffffffu;
        p->tfd = 0x7f;
        p->ci = 0;
        p->sact = 0;
    }
    update_irq(p->h);
}

bool ahci_hotplug(ahci *h, int port, mvm_blk *blk)
{
    if (port < 0 || port >= AHCI_PORTS || h->port[port].dev || !blk)
        return false;
    h->port[port].dev = ide_port_new(h->vm, blk, false);
    port_changed(&h->port[port], true);
    return true;
}

bool ahci_unplug(ahci *h, int port)
{
    if (port < 0 || port >= AHCI_PORTS || !h->port[port].dev)
        return false;
    ide_port_free(h->port[port].dev); /* o blk continua com quem chamou */
    h->port[port].dev = NULL;
    port_changed(&h->port[port], false);
    return true;
}

bool ahci_change_media(ahci *h, int port, mvm_blk *blk)
{
    if (port < 0 || port >= AHCI_PORTS || !h->port[port].dev)
        return false;
    return ide_port_change_media(h->port[port].dev, blk);
}

int ahci_free_port(ahci *h)
{
    for (int i = 0; i < AHCI_PORTS; i++)
        if (!h->port[i].dev)
            return i;
    return -1;
}

int ahci_port_kind(ahci *h, int port)
{
    if (port < 0 || port >= AHCI_PORTS || !h->port[port].dev)
        return 0;
    return ide_port_kind(h->port[port].dev);
}

void ahci_reset(ahci *h) { hba_reset(h); }

void ahci_free(ahci *h)
{
    if (!h)
        return;
    for (int i = 0; i < AHCI_PORTS; i++)
        ide_port_free(h->port[i].dev);
    free(h);
}
