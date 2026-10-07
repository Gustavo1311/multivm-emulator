/*
 * Realtek RTL8139C (PCI 10ec:8139): placa de rede de 100 Mbit com driver nativo
 * no Windows XP, no ReactOS e no Linux (8139too).
 *
 * Recepcao: buffer circular continuo em RBSTART (8/16/32/64 KiB + 16), cada
 * quadro com cabecalho de 4 bytes (status, tamanho com CRC) alinhado a 4 bytes;
 * o driver informa o quanto ja leu em CAPR. Transmissao: 4 descritores
 * (TSAD/TSD) usados em rodizio. O modo C+ nao e emulado.
 */
#include "devices.h"

#include <stdlib.h>

/* registradores */
#define IDR0 0x00
#define MAR0 0x08
#define TSD0 0x10
#define TSAD0 0x20
#define RBSTART 0x30
#define CR 0x37
#define CAPR 0x38
#define CBR 0x3a
#define IMR 0x3c
#define ISR 0x3e
#define TCR 0x40
#define RCR 0x44
#define TCTR 0x48
#define MPC 0x4c
#define CR9346 0x50
#define CONFIG0 0x51
#define CONFIG1 0x52
#define MSR 0x58
#define CONFIG3 0x59
#define CONFIG4 0x5a
#define MULINT 0x5c
#define TSAD 0x60
#define BMCR 0x62
#define BMSR 0x64
#define ANAR 0x66
#define ANLPAR 0x68
#define ANER 0x6a

#define CR_BUFE 0x01
#define CR_TE 0x04
#define CR_RE 0x08
#define CR_RST 0x10

#define INT_ROK 0x0001
#define INT_TOK 0x0004
#define INT_RXOVW 0x0010
#define INT_FOVW 0x0040

#define TSD_OWN (1u << 13)
#define TSD_TOK (1u << 15)

#define RCR_AAP 0x01
#define RCR_APM 0x02
#define RCR_AM 0x04
#define RCR_AB 0x08
#define RCR_WRAP 0x80

/* versao do chip em TCR: RTL8139C (0x74 = bits 30-26 e 23-22) */
#define TCR_HWVER ((0x74u >> 2) << 26 | (0x74u & 3) << 22)

struct rtl8139 {
    mvm_vm *vm;
    mvm_net *net;
    irq_line irq;
    uint8_t mac[6];
    uint8_t regs[256];   /* registradores sem efeito colateral */
    uint32_t tsd[4], tsad[4];
    uint32_t rbstart;
    uint16_t capr, cbr, imr, isr;
    uint32_t tcr, rcr;
    uint8_t cr, cr9346;
    eeprom93 ee;
    uint8_t frame[2048];
};

static void update_irq(rtl8139 *r) { irq_set(&r->irq, (r->isr & r->imr) ? 1 : 0); }

static uint32_t rx_buf_size(rtl8139 *r) { return 8192u << ((r->rcr >> 11) & 3); }

static void soft_reset(rtl8139 *r)
{
    r->cr = CR_BUFE;
    r->imr = 0;
    r->isr = 0;
    r->capr = 0xfff0;
    r->cbr = 0;
    r->rcr = 0;
    r->tcr = TCR_HWVER;
    for (int i = 0; i < 4; i++)
        r->tsd[i] = TSD_OWN;
    update_irq(r);
}

void rtl8139_reset(rtl8139 *r)
{
    memset(r->regs, 0, sizeof(r->regs));
    memcpy(r->regs + IDR0, r->mac, 6);
    memset(r->regs + MAR0, 0xff, 8);
    r->regs[CONFIG0] = 0x00;
    r->regs[CONFIG1] = 0x0c;
    r->regs[CONFIG3] = 0x00;
    r->regs[CONFIG4] = 0x00;
    r->rbstart = 0;
    r->cr9346 = 0;
    ee93_reset(&r->ee);
    soft_reset(r);
}

/* espaco livre no buffer circular (entre CBR e o ponto de leitura CAPR+16) */
static uint32_t rx_free(rtl8139 *r)
{
    uint32_t size = rx_buf_size(r);
    uint32_t rd = (uint32_t)(uint16_t)(r->capr + 16) % size;
    uint32_t wr = r->cbr % size;
    if (rd == wr)
        return size; /* vazio */
    return (rd - wr + size) % size;
}

static bool accept(rtl8139 *r, const uint8_t *f)
{
    if (r->rcr & RCR_AAP)
        return true;
    if (!memcmp(f, "\xff\xff\xff\xff\xff\xff", 6))
        return r->rcr & RCR_AB;
    if (f[0] & 1)
        return r->rcr & RCR_AM;
    return (r->rcr & RCR_APM) && !memcmp(f, r->regs + IDR0, 6);
}

/* MVM_NIC_DEBUG=1: registra os acessos aos registradores (depuracao de drivers) */
static int nic_debug = -1;
static int nic_debug_n;

static void dbg(const char *op, uint64_t off, unsigned size, uint64_t v)
{
    if (nic_debug < 0) {
        const char *e = getenv("MVM_NIC_DEBUG");
        nic_debug = e ? atoi(e) : 0;
    }
    if (nic_debug == 2 && strcmp(op, "cheio")) /* 2: so os eventos de buffer cheio */
        return;
    if (nic_debug && nic_debug_n++ < 20000)
        LOGI("rtl8139 %s %02llx/%u = %llx", op, (unsigned long long)off, size, (unsigned long long)v);
}

static void rx_ready(void *opaque)
{
    rtl8139 *r = opaque;
    if (!(r->cr & CR_RE)) {
        /* recepcao desligada: descarta o que chegar */
        while (net_rx_pop(r->net, r->frame, sizeof(r->frame)))
            ;
        return;
    }
    bool any = false;
    for (;;) {
        size_t len = net_rx_peek(r->net);
        if (!len)
            break;
        uint32_t need = ((uint32_t)len + 4 + 4 + 3) & ~3u;
        if (rx_free(r) <= need) {
            /* cheio: o quadro espera na fila ate o driver liberar espaco (CAPR). Como a
             * placa real, avisa com RXOVW: o driver so le o buffer quando ha interrupcao
             * de recepcao, e sem pacote novo o ROK nao voltaria. */
            if (!(r->isr & INT_RXOVW)) {
                r->isr |= INT_RXOVW;
                any = true;
            }
            dbg("cheio", r->cbr, (unsigned)need, ((uint64_t)r->capr << 32) | rx_free(r));
            break;
        }
        len = net_rx_pop(r->net, r->frame, sizeof(r->frame));
        if (!accept(r, r->frame))
            continue;
        uint32_t size = rx_buf_size(r);
        uint16_t status = 0x0001; /* ROK */
        if (!memcmp(r->frame, "\xff\xff\xff\xff\xff\xff", 6))
            status |= 0x2000;
        else if (r->frame[0] & 1)
            status |= 0x8000;
        else
            status |= 0x4000;
        uint8_t hdr[4];
        hdr[0] = (uint8_t)status;
        hdr[1] = (uint8_t)(status >> 8);
        hdr[2] = (uint8_t)(len + 4);
        hdr[3] = (uint8_t)((len + 4) >> 8);
        uint8_t crc[4] = {0};
        /* grava cabecalho + quadro + CRC. Com RCR bit 7 (WRAP, "sem volta" no Linux) o
         * quadro segue continuo alem do fim (o driver reserva essa sobra); sem ele,
         * continua no inicio do buffer. */
        uint32_t off = r->cbr % size;
        uint8_t pkt[sizeof(r->frame) + 8];
        memcpy(pkt, hdr, 4);
        memcpy(pkt + 4, r->frame, len);
        memcpy(pkt + 4 + len, crc, 4);
        size_t total = len + 8;
        if ((r->rcr & RCR_WRAP) || off + total <= size) {
            space_memwrite(&r->vm->mem, r->rbstart + off, pkt, total);
        } else {
            size_t first = size - off;
            space_memwrite(&r->vm->mem, r->rbstart + off, pkt, first);
            space_memwrite(&r->vm->mem, r->rbstart, pkt + first, total - first);
        }
        r->cbr = (uint16_t)((r->cbr + need) % size);
        r->cr &= (uint8_t)~CR_BUFE;
        r->isr |= INT_ROK;
        any = true;
    }
    if (any)
        update_irq(r);
}

static void transmit(rtl8139 *r, int i)
{
    uint32_t len = r->tsd[i] & 0x1fff;
    if (!(r->cr & CR_TE) || !len || len > sizeof(r->frame)) {
        r->tsd[i] |= TSD_OWN;
        return;
    }
    space_memread(&r->vm->mem, r->tsad[i], r->frame, len);
    net_send(r->net, r->frame, len);
    r->tsd[i] |= TSD_OWN | TSD_TOK;
    r->isr |= INT_TOK;
    update_irq(r);
}

static uint32_t reg_read(rtl8139 *r, unsigned off)
{
    if (off >= TSD0 && off < TSD0 + 16)
        return r->tsd[(off - TSD0) / 4] >> (8 * (off & 3));
    if (off >= TSAD0 && off < TSAD0 + 16)
        return r->tsad[(off - TSAD0) / 4] >> (8 * (off & 3));
    if (off >= RBSTART && off < RBSTART + 4)
        return r->rbstart >> (8 * (off - RBSTART));
    if (off >= TCR && off < TCR + 4)
        return r->tcr >> (8 * (off - TCR));
    if (off >= RCR && off < RCR + 4)
        return r->rcr >> (8 * (off - RCR));
    switch (off) {
    case CR: return r->cr;
    case CAPR: return r->capr & 0xff;
    case CAPR + 1: return r->capr >> 8;
    case CBR: return r->cbr & 0xff;
    case CBR + 1: return r->cbr >> 8;
    case IMR: return r->imr & 0xff;
    case IMR + 1: return r->imr >> 8;
    case ISR: return r->isr & 0xff;
    case ISR + 1: return r->isr >> 8;
    case CR9346: return (r->cr9346 & ~1u) | (r->ee.dout ? 1 : 0);
    case MSR: return 0x10; /* link ok (LINKB=0), 100 Mbit */
    case TSAD:
    case TSAD + 1: {
        uint16_t v = 0;
        for (int i = 0; i < 4; i++) {
            if (r->tsd[i] & TSD_OWN) v |= (uint16_t)(1 << i);
            if (r->tsd[i] & TSD_TOK) v |= (uint16_t)(1 << (12 + i));
        }
        return off == TSAD ? (v & 0xff) : (v >> 8);
    }
    case BMCR: return 0x00;       /* 0x1000 = autonegociacao */
    case BMCR + 1: return 0x10;
    case BMSR: return 0x2d;       /* link ok, autonegociacao completa */
    case BMSR + 1: return 0x78;
    case ANAR: return 0xe1;
    case ANAR + 1: return 0x05;
    case ANLPAR: return 0xe1;
    case ANLPAR + 1: return 0x45;
    case ANER: return 0x01;
    default: return r->regs[off & 0xff];
    }
}

static void reg_write(rtl8139 *r, unsigned off, uint8_t v)
{
    if (off >= TSD0 && off < TSD0 + 16) {
        int i = (off - TSD0) / 4;
        unsigned sh = 8 * (off & 3);
        r->tsd[i] = (r->tsd[i] & ~(0xffu << sh)) | ((uint32_t)v << sh);
        return; /* a transmissao comeca na escrita completa (ver mmio_write) */
    }
    if (off >= TSAD0 && off < TSAD0 + 16) {
        int i = (off - TSAD0) / 4;
        unsigned sh = 8 * (off & 3);
        r->tsad[i] = (r->tsad[i] & ~(0xffu << sh)) | ((uint32_t)v << sh);
        return;
    }
    if (off >= RBSTART && off < RBSTART + 4) {
        unsigned sh = 8 * (off - RBSTART);
        r->rbstart = (r->rbstart & ~(0xffu << sh)) | ((uint32_t)v << sh);
        return;
    }
    if (off >= TCR && off < TCR + 4) {
        unsigned sh = 8 * (off - TCR);
        r->tcr = (r->tcr & ~(0xffu << sh)) | ((uint32_t)v << sh);
        r->tcr = (r->tcr & ~(0x7cc00000u)) | TCR_HWVER; /* versao so leitura */
        return;
    }
    if (off >= RCR && off < RCR + 4) {
        unsigned sh = 8 * (off - RCR);
        r->rcr = (r->rcr & ~(0xffu << sh)) | ((uint32_t)v << sh);
        return;
    }
    switch (off) {
    case CR:
        if (v & CR_RST) {
            soft_reset(r);
            return;
        }
        r->cr = (uint8_t)((r->cr & CR_BUFE) | (v & (CR_TE | CR_RE)));
        if (r->cr & CR_RE)
            rx_ready(r);
        return;
    case CAPR:
        r->capr = (uint16_t)((r->capr & 0xff00) | v);
        return;
    case CAPR + 1:
        r->capr = (uint16_t)((r->capr & 0x00ff) | (v << 8));
        if ((uint16_t)(r->capr + 16) % rx_buf_size(r) == r->cbr % rx_buf_size(r))
            r->cr |= CR_BUFE;
        rx_ready(r); /* espaco liberado */
        return;
    case IMR:
        r->imr = (uint16_t)((r->imr & 0xff00) | v);
        update_irq(r);
        return;
    case IMR + 1:
        r->imr = (uint16_t)((r->imr & 0x00ff) | (v << 8));
        update_irq(r);
        return;
    case ISR: /* escrever 1 limpa */
        r->isr &= (uint16_t)~v;
        update_irq(r);
        return;
    case ISR + 1:
        r->isr &= (uint16_t)~(v << 8);
        update_irq(r);
        return;
    case CR9346:
        r->cr9346 = v & 0xfe;
        if ((v >> 6) == 1) { /* auto-load: recarrega o MAC da EEPROM e volta ao modo normal */
            for (int i = 0; i < 3; i++) {
                r->regs[IDR0 + 2 * i] = (uint8_t)r->ee.data[7 + i];
                r->regs[IDR0 + 2 * i + 1] = (uint8_t)(r->ee.data[7 + i] >> 8);
            }
            r->cr9346 &= 0x3e;
            return;
        }
        if ((v >> 6) == 2) /* modo de programacao: pinos da EEPROM */
            ee93_write(&r->ee, v & 0x08, v & 0x04, v & 0x02);
        else if ((v >> 6) == 0 && r->ee.cs)
            ee93_write(&r->ee, false, false, false);
        return;
    case MPC:
        r->regs[MPC] = r->regs[MPC + 1] = r->regs[MPC + 2] = r->regs[MPC + 3] = 0;
        return;
    default:
        if (off < 6) { /* IDR: o MAC pode ser trocado com 9346CR em modo de configuracao */
            r->regs[off] = v;
            return;
        }
        r->regs[off & 0xff] = v;
        return;
    }
}

static uint64_t mmio_read(void *opaque, uint64_t off, unsigned size)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < size; i++)
        v |= (uint64_t)(reg_read(opaque, (unsigned)(off + i) & 0xff) & 0xff) << (8 * i);
    dbg("le", off, size, v);
    return v;
}

static void mmio_write(void *opaque, uint64_t off, uint64_t val, unsigned size)
{
    rtl8139 *r = opaque;
    dbg("escreve", off, size, val);
    for (unsigned i = 0; i < size; i++)
        reg_write(r, (unsigned)(off + i) & 0xff, (uint8_t)(val >> (8 * i)));
    /* escrita em TSDn (com OWN zerado) dispara a transmissao do descritor */
    if (off >= TSD0 && off < TSD0 + 16 && (off & 3) == 0 && size == 4) {
        int i = (int)(off - TSD0) / 4;
        r->tsd[i] &= ~(TSD_TOK);
        if (!(r->tsd[i] & TSD_OWN))
            transmit(r, i);
    }
}

const mvm_io_ops rtl8139_ops = {mmio_read, mmio_write};

rtl8139 *rtl8139_new(mvm_vm *vm, mvm_net *net, irq_line irq)
{
    rtl8139 *r = calloc(1, sizeof(*r));
    r->vm = vm;
    r->net = net;
    r->irq = irq;
    net_get_mac(net, r->mac);
    /* EEPROM: id 0x8129, ids PCI e o MAC nas palavras 7-9 */
    r->ee.data[0] = 0x8129;
    r->ee.data[1] = 0x10ec;
    r->ee.data[2] = 0x8139;
    for (int i = 0; i < 3; i++)
        r->ee.data[7 + i] = (uint16_t)(r->mac[2 * i] | r->mac[2 * i + 1] << 8);
    rtl8139_reset(r);
    net_set_client(net, rx_ready, r);
    return r;
}

void rtl8139_free(rtl8139 *r) { free(r); }
