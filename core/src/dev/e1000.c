/*
 * Intel 82540EM (PCI 8086:100e), a "e1000": driver nativo no Windows 7/10/Server,
 * no Linux (e1000) e no ReactOS.
 *
 * Aneis de descritores de recepcao/transmissao na RAM do convidado, EEPROM
 * Microwire com o MAC, PHY M88 via MDIC (link sempre em 1 Gbit full duplex).
 * Na transmissao: descritores legados e estendidos (contexto + dados) com
 * calculo de checksum IP/TCP/UDP e segmentacao TCP (TSO), que o Linux liga.
 */
#include "devices.h"

#include <stdlib.h>

/* registradores (deslocamento / 4 no vetor mac[]) */
#define CTRL 0x0000
#define STATUS 0x0008
#define EECD 0x0010
#define EERD 0x0014
#define CTRL_EXT 0x0018
#define MDIC 0x0020
#define ICR 0x00c0
#define ITR 0x00c4
#define ICS 0x00c8
#define IMS 0x00d0
#define IMC 0x00d8
#define RCTL 0x0100
#define TCTL 0x0400
#define RDBAL 0x2800
#define RDBAH 0x2804
#define RDLEN 0x2808
#define RDH 0x2810
#define RDT 0x2818
#define TDBAL 0x3800
#define TDBAH 0x3804
#define TDLEN 0x3808
#define TDH 0x3810
#define TDT 0x3818
#define RAL0 0x5400
#define RAH0 0x5404

#define REGS_SIZE 0x20000

#define ICR_TXDW 0x01
#define ICR_TXQE 0x02
#define ICR_LSC 0x04
#define ICR_RXDMT0 0x10
#define ICR_RXO 0x40
#define ICR_RXT0 0x80

#define RCTL_EN (1u << 1)
#define RCTL_UPE (1u << 3)
#define RCTL_MPE (1u << 4)
#define RCTL_BAM (1u << 15)
#define RCTL_BSEX (1u << 25)
#define RCTL_SECRC (1u << 26)
#define TCTL_EN (1u << 1)

#define EECD_SK 0x01
#define EECD_CS 0x02
#define EECD_DI 0x04
#define EECD_DO 0x08
#define EECD_REQ 0x40
#define EECD_GNT 0x80
#define EECD_PRES 0x100

/* comando dos descritores de transmissao */
#define TXD_EOP 0x01
#define TXD_IC 0x04
#define TXD_RS 0x08
#define TXD_DEXT 0x20
#define TXD_TSE 0x04 /* em dcmd/tucmd estendido */
#define TXD_POPTS_IXSM 0x01
#define TXD_POPTS_TXSM 0x02

#define TX_MAX (64 * 1024 + 256)
#define NET_SEG_MAX 9216 /* um segmento do TSO (cabecalhos + MSS) */

struct e1000 {
    mvm_vm *vm;
    mvm_net *net;
    irq_line irq;
    uint8_t mac_addr[6];
    uint32_t *mac; /* banco de registradores (REGS_SIZE / 4) */
    uint16_t phy[32];
    eeprom93 ee;
    uint32_t ioaddr;
    /* transmissao em andamento */
    uint8_t *tx;
    size_t tx_len;
    bool tx_tse;
    uint8_t tx_popts;
    struct {
        uint8_t ipcss, ipcso, tucss, tucso;
        uint16_t ipcse, tucse;
        uint32_t paylen;
        uint8_t hdrlen;
        uint16_t mss;
        bool tcp, ipv4;
    } ctx;
    uint8_t rxbuf[16384 + 4];
};

#define R(e, reg) ((e)->mac[(reg) >> 2])

static void update_irq(e1000 *e) { irq_set(&e->irq, (R(e, ICR) & R(e, IMS)) ? 1 : 0); }

static void set_ics(e1000 *e, uint32_t v)
{
    R(e, ICR) |= v;
    update_irq(e);
}

void e1000_reset(e1000 *e)
{
    memset(e->mac, 0, REGS_SIZE);
    R(e, CTRL) = 0x00140240; /* full duplex, link up, 1000 */
    R(e, STATUS) = 0x00000083; /* FD | LU | 1000 Mbit */
    R(e, EECD) = EECD_PRES | EECD_GNT;
    R(e, RAL0) = (uint32_t)(e->mac_addr[0] | e->mac_addr[1] << 8 | e->mac_addr[2] << 16 | (uint32_t)e->mac_addr[3] << 24);
    R(e, RAH0) = (uint32_t)(e->mac_addr[4] | e->mac_addr[5] << 8) | 0x80000000u; /* AV */
    static const uint16_t phy_init[32] = {
        [0] = 0x1140, [1] = 0x796d, [2] = 0x0141, [3] = 0x0c20, [4] = 0x0de1, [5] = 0x45e1,
        [6] = 0x000f, [9] = 0x0e00, [10] = 0x3c00, [15] = 0x3000, [16] = 0x0360, [17] = 0xac00,
        [20] = 0x0c60,
    };
    memcpy(e->phy, phy_init, sizeof(e->phy));
    ee93_reset(&e->ee);
    e->tx_len = 0;
    update_irq(e);
}

/* ---------------------------------------------------------------- recepcao */

static bool accept(e1000 *e, const uint8_t *f)
{
    uint32_t rctl = R(e, RCTL);
    if (rctl & RCTL_UPE)
        return true;
    if (!memcmp(f, "\xff\xff\xff\xff\xff\xff", 6))
        return rctl & RCTL_BAM;
    if (f[0] & 1)
        return true; /* multicast: aceita (sem filtro por hash) */
    return !memcmp(f, e->mac_addr, 6);
}

static uint32_t rx_buf_len(e1000 *e)
{
    uint32_t rctl = R(e, RCTL);
    uint32_t bs = (rctl >> 16) & 3;
    if (rctl & RCTL_BSEX)
        return bs == 1 ? 16384 : bs == 2 ? 8192 : bs == 3 ? 4096 : 2048;
    return bs == 0 ? 2048 : bs == 1 ? 1024 : bs == 2 ? 512 : 256;
}

static void rx_ready(void *opaque)
{
    e1000 *e = opaque;
    if (!(R(e, RCTL) & RCTL_EN)) {
        while (net_rx_pop(e->net, e->rxbuf, sizeof(e->rxbuf)))
            ;
        return;
    }
    uint32_t ndesc = R(e, RDLEN) / 16;
    if (!ndesc)
        return;
    uint64_t base = ((uint64_t)R(e, RDBAH) << 32) | (R(e, RDBAL) & ~0xfu);
    uint32_t blen = rx_buf_len(e);
    bool any = false;
    for (;;) {
        size_t len = net_rx_peek(e->net);
        if (!len)
            break;
        size_t total = len + ((R(e, RCTL) & RCTL_SECRC) ? 0 : 4);
        uint32_t need = (uint32_t)((total + blen - 1) / blen);
        uint32_t head = R(e, RDH) % ndesc, tail = R(e, RDT) % ndesc;
        uint32_t avail = (tail - head + ndesc) % ndesc;
        if (avail < need) /* sem descritores livres: espera o driver (RDT) */
            break;
        len = net_rx_pop(e->net, e->rxbuf, sizeof(e->rxbuf) - 4);
        if (!accept(e, e->rxbuf))
            continue;
        memset(e->rxbuf + len, 0, 4); /* CRC */
        size_t done = 0;
        while (done < total) {
            uint64_t da = base + 16ULL * head;
            uint64_t buf = space_read(&e->vm->mem, da, 8);
            size_t chunk = total - done < blen ? total - done : blen;
            space_memwrite(&e->vm->mem, buf, e->rxbuf + done, chunk);
            done += chunk;
            uint8_t wb[8];
            wb[0] = (uint8_t)chunk;
            wb[1] = (uint8_t)(chunk >> 8);
            wb[2] = wb[3] = 0;              /* checksum */
            wb[4] = done >= total ? 0x03 : 0x01; /* DD | EOP */
            wb[5] = 0;                      /* erros */
            wb[6] = wb[7] = 0;
            space_memwrite(&e->vm->mem, da + 8, wb, 8);
            head = (head + 1) % ndesc;
        }
        R(e, RDH) = head;
        any = true;
    }
    if (any) {
        uint32_t head = R(e, RDH) % ndesc, tail = R(e, RDT) % ndesc;
        uint32_t cause = ICR_RXT0;
        if ((tail - head + ndesc) % ndesc < ndesc / 8)
            cause |= ICR_RXDMT0;
        set_ics(e, cause);
    }
}

/* ---------------------------------------------------------------- transmissao */

static uint16_t csum_range(const uint8_t *p, size_t from, size_t to)
{
    uint32_t sum = 0;
    for (size_t i = from; i + 1 < to; i += 2)
        sum += (uint32_t)(p[i] << 8 | p[i + 1]);
    if ((to - from) & 1)
        sum += (uint32_t)(p[to - 1] << 8);
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/* checksums pedidos pelo driver (popts IXSM/TXSM) */
static void tx_checksums(e1000 *e, uint8_t *p, size_t len)
{
    if ((e->tx_popts & TXD_POPTS_IXSM) && e->ctx.ipcso + 2u <= len) {
        size_t end = e->ctx.ipcse ? (size_t)e->ctx.ipcse + 1 : len;
        if (end > len) end = len;
        put16(p + e->ctx.ipcso, 0);
        put16(p + e->ctx.ipcso, csum_range(p, e->ctx.ipcss, end));
    }
    if ((e->tx_popts & TXD_POPTS_TXSM) && e->ctx.tucso + 2u <= len) {
        size_t end = e->ctx.tucse ? (size_t)e->ctx.tucse + 1 : len;
        if (end > len) end = len;
        /* o campo ja traz a soma do pseudo-cabecalho */
        put16(p + e->ctx.tucso, csum_range(p, e->ctx.tucss, end));
    }
}

static void tx_send_tso(e1000 *e)
{
    uint8_t *p = e->tx;
    size_t hl = e->ctx.hdrlen;
    if (hl >= e->tx_len || !e->ctx.mss) {
        net_send(e->net, p, e->tx_len);
        return;
    }
    size_t pay = e->tx_len - hl;
    uint32_t seq0 = e->ctx.tcp ? (uint32_t)(p[e->ctx.tucss + 4] << 24 | p[e->ctx.tucss + 5] << 16 |
                                            p[e->ctx.tucss + 6] << 8 | p[e->ctx.tucss + 7]) : 0;
    uint16_t id0 = (uint16_t)(p[e->ctx.ipcss + 4] << 8 | p[e->ctx.ipcss + 5]);
    uint8_t flags0 = e->ctx.tcp ? p[e->ctx.tucss + 13] : 0;
    /* soma do pseudo-cabecalho sem o tamanho, deixada pelo driver */
    uint16_t pseudo = (uint16_t)(p[e->ctx.tucso] << 8 | p[e->ctx.tucso + 1]);
    uint8_t seg[NET_SEG_MAX];
    int nseg = 0;
    for (size_t off = 0; off < pay; off += e->ctx.mss, nseg++) {
        size_t n = pay - off < e->ctx.mss ? pay - off : e->ctx.mss;
        if (hl + n > sizeof(seg))
            break;
        memcpy(seg, p, hl);
        memcpy(seg + hl, p + hl + off, n);
        size_t slen = hl + n;
        if (e->ctx.ipv4) {
            put16(seg + e->ctx.ipcss + 2, (uint16_t)(slen - e->ctx.ipcss));
            put16(seg + e->ctx.ipcss + 4, (uint16_t)(id0 + nseg));
        } else { /* IPv6: payload length */
            put16(seg + e->ctx.ipcss + 4, (uint16_t)(slen - e->ctx.ipcss - 40));
        }
        if (e->ctx.tcp) {
            uint32_t sq = seq0 + (uint32_t)off;
            seg[e->ctx.tucss + 4] = (uint8_t)(sq >> 24);
            seg[e->ctx.tucss + 5] = (uint8_t)(sq >> 16);
            seg[e->ctx.tucss + 6] = (uint8_t)(sq >> 8);
            seg[e->ctx.tucss + 7] = (uint8_t)sq;
            if (off + n < pay)
                seg[e->ctx.tucss + 13] = flags0 & (uint8_t)~0x09; /* FIN/PSH so no ultimo */
        } else { /* UDP */
            put16(seg + e->ctx.tucss + 4, (uint16_t)(slen - e->ctx.tucss));
        }
        if (e->ctx.ipv4 && (e->tx_popts & TXD_POPTS_IXSM)) {
            size_t ihl = (size_t)(seg[e->ctx.ipcss] & 15) * 4;
            put16(seg + e->ctx.ipcso, 0);
            put16(seg + e->ctx.ipcso, csum_range(seg, e->ctx.ipcss, e->ctx.ipcss + ihl));
        }
        if (e->tx_popts & TXD_POPTS_TXSM) {
            /* pseudo-cabecalho + tamanho deste segmento */
            uint32_t sum = pseudo + (uint32_t)(slen - e->ctx.tucss);
            while (sum >> 16)
                sum = (sum & 0xffff) + (sum >> 16);
            put16(seg + e->ctx.tucso, (uint16_t)sum);
            put16(seg + e->ctx.tucso, csum_range(seg, e->ctx.tucss, slen));
        }
        net_send(e->net, seg, slen);
    }
}

static void tx_packet_done(e1000 *e)
{
    if (e->tx_tse)
        tx_send_tso(e);
    else {
        tx_checksums(e, e->tx, e->tx_len);
        net_send(e->net, e->tx, e->tx_len);
    }
    e->tx_len = 0;
    e->tx_tse = false;
    e->tx_popts = 0;
}

static void tx_run(e1000 *e)
{
    if (!(R(e, TCTL) & TCTL_EN))
        return;
    uint32_t ndesc = R(e, TDLEN) / 16;
    if (!ndesc)
        return;
    uint64_t base = ((uint64_t)R(e, TDBAH) << 32) | (R(e, TDBAL) & ~0xfu);
    uint32_t cause = 0;
    for (int guard = 0; R(e, TDH) % ndesc != R(e, TDT) % ndesc && guard < 4096; guard++) {
        uint32_t head = R(e, TDH) % ndesc;
        uint64_t da = base + 16ULL * head;
        uint8_t d[16];
        space_memread(&e->vm->mem, da, d, 16);
        uint64_t addr = ld_le(d, 8);
        uint32_t lower = (uint32_t)ld_le(d + 8, 4);
        uint8_t cmd = (uint8_t)(lower >> 24);
        bool ext = cmd & TXD_DEXT;
        unsigned dtyp = (lower >> 20) & 0xf;
        if (ext && dtyp == 0) { /* descritor de contexto */
            e->ctx.ipcss = d[0];
            e->ctx.ipcso = d[1];
            e->ctx.ipcse = (uint16_t)ld_le(d + 2, 2);
            e->ctx.tucss = d[4];
            e->ctx.tucso = d[5];
            e->ctx.tucse = (uint16_t)ld_le(d + 6, 2);
            e->ctx.paylen = lower & 0xfffff;
            e->ctx.hdrlen = d[13];
            e->ctx.mss = (uint16_t)ld_le(d + 14, 2);
            uint8_t tucmd = cmd;
            e->ctx.tcp = tucmd & 0x01;  /* TCP (senao UDP) */
            e->ctx.ipv4 = tucmd & 0x02; /* IP = IPv4 */
        } else {
            size_t len = ext ? (lower & 0xfffff) : (lower & 0xffff);
            if (ext) {
                e->tx_popts = d[13];
                if (cmd & TXD_TSE)
                    e->tx_tse = true;
            }
            if (e->tx_len + len <= TX_MAX) {
                space_memread(&e->vm->mem, addr, e->tx + e->tx_len, len);
                e->tx_len += len;
            }
            if (cmd & TXD_EOP) {
                if (!ext && (cmd & TXD_IC)) { /* checksum legado: em CSO, a partir de CSS */
                    uint8_t cso = d[10], css = d[13];
                    if (cso + 2u <= e->tx_len && css < e->tx_len) {
                        put16(e->tx + cso, 0);
                        put16(e->tx + cso, csum_range(e->tx, css, e->tx_len));
                    }
                    net_send(e->net, e->tx, e->tx_len);
                    e->tx_len = 0;
                } else {
                    tx_packet_done(e);
                }
            }
        }
        if (cmd & TXD_RS) { /* escreve DD no status */
            uint8_t st = d[12] | 0x01;
            space_write(&e->vm->mem, da + 12, st, 1);
        }
        R(e, TDH) = (head + 1) % ndesc;
        cause |= ICR_TXDW;
    }
    if (cause) {
        cause |= ICR_TXQE;
        set_ics(e, cause);
    }
}

/* ---------------------------------------------------------------- registradores */

static uint32_t reg_read(e1000 *e, uint32_t off)
{
    switch (off) {
    case ICR: {
        uint32_t v = R(e, ICR);
        R(e, ICR) = 0; /* ler limpa */
        update_irq(e);
        return v;
    }
    case EECD:
        return (R(e, EECD) & ~EECD_DO) | (e->ee.dout ? EECD_DO : 0);
    case IMC:
    case ICS:
        return 0;
    default:
        if (off >= 0x4000 && off < 0x4100) { /* estatisticas: zeradas ao ler */
            uint32_t v = R(e, off);
            R(e, off) = 0;
            return v;
        }
        return off < REGS_SIZE ? R(e, off) : 0;
    }
}

static void reg_write(e1000 *e, uint32_t off, uint32_t v)
{
    switch (off) {
    case CTRL:
        if (v & (1u << 26)) { /* RST */
            e1000_reset(e);
            return;
        }
        R(e, CTRL) = v & ~(1u << 26);
        return;
    case STATUS:
        return;
    case EECD: {
        uint32_t keep = EECD_PRES;
        if (v & EECD_REQ)
            keep |= EECD_GNT;
        R(e, EECD) = (v & (EECD_SK | EECD_CS | EECD_DI | EECD_REQ)) | keep;
        ee93_write(&e->ee, v & EECD_CS, v & EECD_SK, v & EECD_DI);
        return;
    }
    case EERD:
        if (v & 1) { /* leitura direta: palavra em 15:8, dado em 31:16, pronto no bit 4 */
            unsigned a = (v >> 8) & 0x3f;
            R(e, EERD) = ((uint32_t)e->ee.data[a] << 16) | (a << 8) | 0x10;
        }
        return;
    case MDIC: {
        unsigned reg = (v >> 16) & 31, phyaddr = (v >> 21) & 31, op = (v >> 26) & 3;
        uint32_t r = v & ~(0xffffu | (1u << 30));
        if (phyaddr != 1) {
            r |= 1u << 30; /* erro: so existe o PHY 1 */
        } else if (op == 2) {
            r |= e->phy[reg];
        } else if (op == 1) {
            if (reg == 0 && (v & 0x8000)) /* reset do PHY */
                e->phy[0] = 0x1140;
            else if (reg != 1 && reg != 2 && reg != 3)
                e->phy[reg] = (uint16_t)v;
            r |= v & 0xffff;
        }
        R(e, MDIC) = r | (1u << 28); /* pronto */
        return;
    }
    case ICR: /* escrever 1 limpa */
        R(e, ICR) &= ~v;
        update_irq(e);
        return;
    case ICS:
        set_ics(e, v);
        return;
    case IMS:
        R(e, IMS) |= v;
        update_irq(e);
        return;
    case IMC:
        R(e, IMS) &= ~v;
        update_irq(e);
        return;
    case RCTL:
        R(e, RCTL) = v;
        if (v & RCTL_EN)
            rx_ready(e);
        return;
    case RDT:
        R(e, RDT) = v & 0xffff;
        rx_ready(e); /* descritores novos */
        return;
    case RDH:
    case TDH:
        R(e, off) = v & 0xffff;
        return;
    case TDT:
        R(e, TDT) = v & 0xffff;
        tx_run(e);
        return;
    case TCTL:
        R(e, TCTL) = v;
        tx_run(e);
        return;
    case RAL0:
    case RAH0:
        R(e, off) = v;
        {
            uint32_t lo = R(e, RAL0), hi = R(e, RAH0);
            e->mac_addr[0] = (uint8_t)lo;
            e->mac_addr[1] = (uint8_t)(lo >> 8);
            e->mac_addr[2] = (uint8_t)(lo >> 16);
            e->mac_addr[3] = (uint8_t)(lo >> 24);
            e->mac_addr[4] = (uint8_t)hi;
            e->mac_addr[5] = (uint8_t)(hi >> 8);
        }
        return;
    default:
        if (off < REGS_SIZE)
            R(e, off) = v;
        return;
    }
}

static uint64_t mmio_read(void *opaque, uint64_t off, unsigned size)
{
    e1000 *e = opaque;
    uint32_t v = reg_read(e, (uint32_t)off & ~3u);
    return (v >> (8 * (off & 3))) & (size >= 4 ? 0xffffffffu : ((1u << (8 * size)) - 1));
}

static void mmio_write(void *opaque, uint64_t off, uint64_t val, unsigned size)
{
    e1000 *e = opaque;
    uint32_t o = (uint32_t)off & ~3u;
    uint32_t v = (uint32_t)val;
    if (size < 4) { /* escrita parcial: junta com o valor atual */
        unsigned sh = 8 * (off & 3);
        uint32_t mask = ((1u << (8 * size)) - 1) << sh;
        v = (R(e, o) & ~mask) | ((v << sh) & mask);
    }
    reg_write(e, o, v);
}

const mvm_io_ops e1000_mmio_ops = {mmio_read, mmio_write};

/* BAR de E/S: IOADDR (0) seleciona o registrador, IODATA (4) le/escreve */
static uint64_t io_read(void *opaque, uint64_t off, unsigned size)
{
    e1000 *e = opaque;
    (void)size;
    if (off < 4)
        return e->ioaddr;
    if (off < 8)
        return reg_read(e, e->ioaddr & 0x1fffc);
    return 0;
}

static void io_write(void *opaque, uint64_t off, uint64_t val, unsigned size)
{
    e1000 *e = opaque;
    (void)size;
    if (off < 4)
        e->ioaddr = (uint32_t)val;
    else if (off < 8)
        reg_write(e, e->ioaddr & 0x1fffc, (uint32_t)val);
}

const mvm_io_ops e1000_io_ops = {io_read, io_write};

e1000 *e1000_new(mvm_vm *vm, mvm_net *net, irq_line irq)
{
    e1000 *e = calloc(1, sizeof(*e));
    e->vm = vm;
    e->net = net;
    e->irq = irq;
    e->mac = calloc(1, REGS_SIZE);
    e->tx = malloc(TX_MAX);
    net_get_mac(net, e->mac_addr);
    /* EEPROM do 82540EM (modelo do QEMU) com o MAC e o checksum 0xBABA */
    static const uint16_t tmpl[64] = {
        0x0000, 0x0000, 0x0000, 0x0000, 0xffff, 0x0000, 0x0000, 0x0000,
        0x3000, 0x1000, 0x6403, 0x100e, 0x8086, 0x100e, 0x8086, 0x3040,
        0x0008, 0x2000, 0x7e14, 0x0048, 0x1000, 0x00d8, 0x0000, 0x2700,
        0x6cc9, 0x3150, 0x0722, 0x040b, 0x0984, 0x0000, 0xc000, 0x0706,
        0x1008, 0x0000, 0x0f04, 0x7fff, 0x4d01, 0xffff, 0xffff, 0xffff,
        0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
        0x0100, 0x4000, 0x121c, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
        0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0x0000,
    };
    memcpy(e->ee.data, tmpl, sizeof(tmpl));
    for (int i = 0; i < 3; i++)
        e->ee.data[i] = (uint16_t)(e->mac_addr[2 * i] | e->mac_addr[2 * i + 1] << 8);
    uint16_t sum = 0;
    for (int i = 0; i < 63; i++)
        sum = (uint16_t)(sum + e->ee.data[i]);
    e->ee.data[63] = (uint16_t)(0xbaba - sum);
    e1000_reset(e);
    net_set_client(net, rx_ready, e);
    return e;
}

void e1000_free(e1000 *e)
{
    if (!e)
        return;
    free(e->mac);
    free(e->tx);
    free(e);
}
