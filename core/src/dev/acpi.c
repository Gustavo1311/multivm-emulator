/*
 * ACPI da maquina "pc": geracao das tabelas (RSDP, RSDT, XSDT, FADT, FACS,
 * DSDT, MADT) e o bloco de gerenciamento de energia (PM1, PM timer, GPE0),
 * no layout do PIIX4 usado pelo QEMU (0x600 / 0xAFE0, SCI na IRQ 9).
 *
 * As tabelas sao entregues de dois jeitos:
 *   - com BIOS: arquivos "etc/acpi/tables" e "etc/acpi/rsdp" no fw_cfg e os
 *     comandos do "etc/table-loader" (formato do BIOSLinker do QEMU), que o
 *     SeaBIOS executa para alocar, ligar ponteiros e calcular checksums;
 *   - no boot direto de kernel: instaladas pelo emulador na RAM (0xE0000).
 */
#include "acpi.h"

#include <stdlib.h>

#include "../acpi/dsdt_aml.h"

/* ------------------------------------------------------------ tabelas */

enum { F_TABLES = 0, F_RSDP = 1 };

static uint32_t add_blob(acpi_tables *t, const void *data, uint32_t len, uint32_t align)
{
    uint32_t off = (t->len + align - 1) & ~(align - 1);
    if (off + len > t->cap) {
        t->cap = (off + len) * 2 + 4096;
        t->data = realloc(t->data, t->cap);
    }
    memset(t->data + t->len, 0, off - t->len);
    memcpy(t->data + off, data, len);
    t->len = off + len;
    return off;
}

static void add_ptr(acpi_tables *t, int dest, uint32_t off, uint8_t size, int src)
{
    if (t->nptr < ACPI_MAX_FIXUPS)
        t->ptr[t->nptr++] = (acpi_ptr_fixup){dest, off, size, src};
}

static void add_cksum(acpi_tables *t, int file, uint32_t start, uint32_t len, uint32_t off)
{
    if (t->nck < ACPI_MAX_FIXUPS)
        t->ck[t->nck++] = (acpi_cksum_fixup){file, off, start, len};
}

/* cabecalho padrao de 36 bytes */
static void header(uint8_t *h, const char *sig, uint32_t len, uint8_t rev)
{
    memcpy(h, sig, 4);
    st_le(h + 4, len, 4);
    h[8] = rev;
    h[9] = 0;
    memcpy(h + 10, "MULTVM", 6);
    memcpy(h + 16, "MVMPC   ", 8);
    st_le(h + 24, 1, 4);
    memcpy(h + 28, "MVM ", 4);
    st_le(h + 32, 1, 4);
}

/* adiciona uma tabela com cabecalho ao blob e registra seu checksum */
static uint32_t add_table(acpi_tables *t, uint8_t *tab, uint32_t len)
{
    uint32_t off = add_blob(t, tab, len, 8);
    add_cksum(t, F_TABLES, off, len, off + 9);
    return off;
}

/* Generic Address Structure de E/S */
static void gas_io(uint8_t *g, uint8_t bits, uint16_t port)
{
    g[0] = 1;     /* espaco de E/S */
    g[1] = bits;
    g[2] = 0;
    g[3] = bits == 8 ? 1 : bits == 16 ? 2 : 3; /* tamanho de acesso */
    st_le(g + 4, port, 8);
}

void acpi_build(acpi_tables *t, const acpi_config *cfg)
{
    memset(t, 0, sizeof(*t));

    /* FACS: precisa de alinhamento de 64 bytes; fica no inicio do blob */
    uint8_t facs[64] = {0};
    memcpy(facs, "FACS", 4);
    st_le(facs + 4, 64, 4);
    facs[32] = 1; /* versao */
    uint32_t facs_off = add_blob(t, facs, sizeof(facs), 64);

    /* DSDT (AML compilado de dsdt.asl) */
    uint8_t *dsdt = malloc(sizeof(dsdt_aml));
    memcpy(dsdt, dsdt_aml, sizeof(dsdt_aml));
    dsdt[9] = 0;
    if (cfg->pci_hole_start != 0x80000000u) {
        /* troca o inicio/tamanho da janela PCI (DWordMemory min..len) */
        static const uint8_t pat[16] = {0x00, 0x00, 0x00, 0x80, 0xff, 0xff, 0xbf, 0xfe,
                                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x7e};
        for (size_t i = 0; i + sizeof(pat) <= sizeof(dsdt_aml); i++) {
            if (!memcmp(dsdt + i, pat, sizeof(pat))) {
                st_le(dsdt + i, cfg->pci_hole_start, 4);
                st_le(dsdt + i + 12, 0xfec00000u - cfg->pci_hole_start, 4);
                break;
            }
        }
    }
    /* drives de disquete: troca a marca "NCDF" (Name FDEN no dsdt.asl) */
    for (size_t i = 0; i + 5 <= sizeof(dsdt_aml); i++) {
        if (dsdt[i] == 0x0c && !memcmp(dsdt + i + 1, "NCDF", 4)) {
            st_le(dsdt + i + 1, cfg->floppies, 4);
            break;
        }
    }
    uint32_t dsdt_off = add_table(t, dsdt, sizeof(dsdt_aml));
    free(dsdt);

    /* FADT (revisao 3, ACPI 2.0: 244 bytes) */
    uint8_t fadt[244] = {0};
    header(fadt, "FACP", sizeof(fadt), 3);
    st_le(fadt + 36, facs_off, 4);          /* FIRMWARE_CTRL */
    st_le(fadt + 40, dsdt_off, 4);          /* DSDT */
    fadt[45] = 0;                           /* perfil: nao especificado */
    st_le(fadt + 46, ACPI_SCI_IRQ, 2);
    /* SMI_CMD = 0: o hardware ja esta em modo ACPI (SCI_EN ligado) */
    st_le(fadt + 56, ACPI_PM_BASE, 4);      /* PM1a_EVT */
    st_le(fadt + 64, ACPI_PM_BASE + 4, 4);  /* PM1a_CNT */
    st_le(fadt + 76, ACPI_PM_BASE + 8, 4);  /* PM_TMR */
    st_le(fadt + 80, ACPI_GPE0_BASE, 4);    /* GPE0 */
    fadt[88] = 4;                           /* PM1_EVT_LEN */
    fadt[89] = 2;                           /* PM1_CNT_LEN */
    fadt[91] = 4;                           /* PM_TMR_LEN */
    fadt[92] = 4;                           /* GPE0_BLK_LEN */
    st_le(fadt + 96, 0x0fff, 2);            /* C2 nao suportado */
    st_le(fadt + 98, 0x0fff, 2);            /* C3 nao suportado */
    fadt[108] = 0x32;                       /* registrador de seculo no CMOS */
    st_le(fadt + 109, 0x0003, 2);           /* IAPC_BOOT_ARCH: dispositivos legados + 8042 */
    st_le(fadt + 112, (1u << 0) |           /* WBINVD */
                      (1u << 2) |           /* C1 em todas as CPUs */
                      (1u << 5) |           /* sem botao de suspensao fixo */
                      (1u << 8) |           /* PM timer de 32 bits */
                      (1u << 10),           /* RESET_REG suportado */
          4);
    gas_io(fadt + 116, 8, 0xcf9);           /* RESET_REG */
    fadt[128] = 0x06;                       /* RESET_VALUE */
    /* X_FIRMWARE_CTRL fica 0: com os dois preenchidos a especificacao manda
     * ignorar o de 32 bits, mas alguns SOs antigos so leem FIRMWARE_CTRL */
    st_le(fadt + 140, dsdt_off, 8);         /* X_DSDT */
    gas_io(fadt + 148, 32, ACPI_PM_BASE);
    gas_io(fadt + 172, 16, ACPI_PM_BASE + 4);
    gas_io(fadt + 208, 32, ACPI_PM_BASE + 8);
    gas_io(fadt + 220, 32, ACPI_GPE0_BASE);
    uint32_t fadt_off = add_table(t, fadt, sizeof(fadt));
    add_ptr(t, F_TABLES, fadt_off + 36, 4, F_TABLES);
    add_ptr(t, F_TABLES, fadt_off + 40, 4, F_TABLES);
    add_ptr(t, F_TABLES, fadt_off + 140, 8, F_TABLES);

    /* MADT */
    uint8_t madt[256] = {0};
    uint32_t m = 44;
    st_le(madt + 36, 0xfee00000u, 4);       /* APIC local */
    st_le(madt + 40, 1, 4);                 /* PCAT_COMPAT: ha 8259 */
    madt[m] = 0; madt[m + 1] = 8;           /* APIC local da CPU 0 */
    madt[m + 2] = 0; madt[m + 3] = 0;
    st_le(madt + m + 4, 1, 4);
    m += 8;
    madt[m] = 1; madt[m + 1] = 12;          /* IOAPIC */
    madt[m + 2] = cfg->ioapic_id;
    st_le(madt + m + 4, 0xfec00000u, 4);
    st_le(madt + m + 8, 0, 4);
    m += 12;
    /* IRQ0 do PIT -> GSI 2 (borda) */
    madt[m] = 2; madt[m + 1] = 10; madt[m + 2] = 0; madt[m + 3] = 0;
    st_le(madt + m + 4, 2, 4);
    st_le(madt + m + 8, 0, 2);
    m += 10;
    /* SCI e IRQs PCI: nivel, ativo em alto (como no PIIX do QEMU) */
    static const uint8_t level_irqs[] = {5, ACPI_SCI_IRQ, 10, 11};
    for (unsigned i = 0; i < sizeof(level_irqs); i++) {
        madt[m] = 2; madt[m + 1] = 10; madt[m + 2] = 0; madt[m + 3] = level_irqs[i];
        st_le(madt + m + 4, level_irqs[i], 4);
        st_le(madt + m + 8, 0x000d, 2);
        m += 10;
    }
    /* NMI do APIC local em LINT1, para todas as CPUs */
    madt[m] = 4; madt[m + 1] = 6; madt[m + 2] = 0xff;
    st_le(madt + m + 3, 0, 2);
    madt[m + 5] = 1;
    m += 6;
    header(madt, "APIC", m, 3);
    uint32_t madt_off = add_table(t, madt, m);

    /* HPET */
    uint32_t tabs[4] = {fadt_off, madt_off};
    unsigned nt = 2;
    if (cfg->hpet_block_id) {
        uint8_t hp[56] = {0};
        header(hp, "HPET", sizeof(hp), 1);
        st_le(hp + 36, cfg->hpet_block_id, 4);
        hp[40] = 0;                         /* GAS: memoria */
        hp[41] = 64;
        st_le(hp + 44, 0xfed00000u, 8);
        hp[52] = 0;                         /* HPET numero 0 */
        st_le(hp + 53, 0x0080, 2);          /* tick minimo no modo periodico */
        hp[55] = 0;
        tabs[nt++] = add_table(t, hp, sizeof(hp));
    }

    /* RSDT e XSDT */
    uint8_t rsdt[36 + 4 * 8] = {0};
    uint32_t rlen = 36 + 4 * nt;
    header(rsdt, "RSDT", rlen, 1);
    for (unsigned i = 0; i < nt; i++)
        st_le(rsdt + 36 + 4 * i, tabs[i], 4);
    uint32_t rsdt_off = add_table(t, rsdt, rlen);
    for (unsigned i = 0; i < nt; i++)
        add_ptr(t, F_TABLES, rsdt_off + 36 + 4 * i, 4, F_TABLES);

    uint8_t xsdt[36 + 8 * 8] = {0};
    uint32_t xlen = 36 + 8 * nt;
    header(xsdt, "XSDT", xlen, 1);
    for (unsigned i = 0; i < nt; i++)
        st_le(xsdt + 36 + 8 * i, tabs[i], 8);
    uint32_t xsdt_off = add_table(t, xsdt, xlen);
    for (unsigned i = 0; i < nt; i++)
        add_ptr(t, F_TABLES, xsdt_off + 36 + 8 * i, 8, F_TABLES);

    /* RSDP (ACPI 2.0) */
    uint8_t *r = t->rsdp;
    memcpy(r, "RSD PTR ", 8);
    memcpy(r + 9, "MULTVM", 6);
    r[15] = 2;
    st_le(r + 16, rsdt_off, 4);
    st_le(r + 20, 36, 4);
    st_le(r + 24, xsdt_off, 8);
    add_ptr(t, F_RSDP, 16, 4, F_TABLES);
    add_ptr(t, F_RSDP, 24, 8, F_TABLES);
    add_cksum(t, F_RSDP, 0, 20, 8);   /* checksum ACPI 1.0 */
    add_cksum(t, F_RSDP, 0, 36, 32);  /* checksum estendido */
}

void acpi_tables_free(acpi_tables *t)
{
    free(t->data);
    t->data = NULL;
}

static uint8_t *file_ptr(acpi_tables *t, int f) { return f == F_RSDP ? t->rsdp : t->data; }

static void apply_cksum(acpi_tables *t, const acpi_cksum_fixup *c)
{
    uint8_t *p = file_ptr(t, c->file);
    p[c->off] = 0;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < c->len; i++)
        sum = (uint8_t)(sum + p[c->start + i]);
    p[c->off] = (uint8_t)-sum;
}

void acpi_install(acpi_tables *t, uint8_t *ram, uint64_t rsdp_addr, uint64_t tables_addr)
{
    for (int i = 0; i < t->nptr; i++) {
        const acpi_ptr_fixup *f = &t->ptr[i];
        uint8_t *p = file_ptr(t, f->dest) + f->off;
        uint64_t base = f->src == F_RSDP ? rsdp_addr : tables_addr;
        st_le(p, ld_le(p, f->size) + base, f->size);
    }
    for (int i = 0; i < t->nck; i++)
        apply_cksum(t, &t->ck[i]);
    memcpy(ram + tables_addr, t->data, t->len);
    memcpy(ram + rsdp_addr, t->rsdp, sizeof(t->rsdp));
}

/* comandos do BIOSLinker (hw/acpi/bios-linker-loader.c do QEMU) */
#define LOADER_ALLOCATE 1
#define LOADER_ADD_POINTER 2
#define LOADER_ADD_CHECKSUM 3
#define LOADER_ZONE_HIGH 1
#define LOADER_ZONE_FSEG 2
#define LOADER_FILESZ 56

static const char *const fnames[] = {"etc/acpi/tables", "etc/acpi/rsdp"};

void acpi_add_to_fw_cfg(acpi_tables *t, fw_cfg *f)
{
    int n = 2 + t->nptr + t->nck;
    uint8_t *ld = calloc((size_t)n, 128);
    uint8_t *e = ld;
    /* RSDP na area 0xE0000-0xFFFFF; as tabelas na memoria alta */
    st_le(e, LOADER_ALLOCATE, 4);
    strncpy((char *)e + 4, fnames[F_RSDP], LOADER_FILESZ);
    st_le(e + 60, 16, 4);
    e[64] = LOADER_ZONE_FSEG;
    e += 128;
    st_le(e, LOADER_ALLOCATE, 4);
    strncpy((char *)e + 4, fnames[F_TABLES], LOADER_FILESZ);
    st_le(e + 60, 64, 4);
    e[64] = LOADER_ZONE_HIGH;
    e += 128;
    for (int i = 0; i < t->nptr; i++, e += 128) {
        st_le(e, LOADER_ADD_POINTER, 4);
        strncpy((char *)e + 4, fnames[t->ptr[i].dest], LOADER_FILESZ);
        strncpy((char *)e + 60, fnames[t->ptr[i].src], LOADER_FILESZ);
        st_le(e + 116, t->ptr[i].off, 4);
        e[120] = t->ptr[i].size;
    }
    for (int i = 0; i < t->nck; i++, e += 128) {
        st_le(e, LOADER_ADD_CHECKSUM, 4);
        strncpy((char *)e + 4, fnames[t->ck[i].file], LOADER_FILESZ);
        st_le(e + 60, t->ck[i].off, 4);
        st_le(e + 64, t->ck[i].start, 4);
        st_le(e + 68, t->ck[i].len, 4);
    }
    fw_cfg_add_file(f, fnames[F_TABLES], t->data, t->len);
    fw_cfg_add_file(f, fnames[F_RSDP], t->rsdp, sizeof(t->rsdp));
    fw_cfg_add_file(f, "etc/table-loader", ld, (uint32_t)(n * 128));
    free(ld);
}

/* ------------------------------------------------------------ PM (PIIX4) */

#define PM_TMR_HZ 3579545LL

#define STS_TMR (1u << 0)
#define STS_PWRBTN (1u << 8)
#define STS_WAK (1u << 15)
#define CNT_SCI_EN (1u << 0)
#define CNT_SLP_EN (1u << 13)

struct acpi_pm {
    mvm_vm *vm;
    irq_line sci;
    uint16_t sts, en, cnt;
    uint16_t gpe_sts, gpe_en;
    mvm_timer tmr;
    int level;
};

static uint32_t pm_timer(acpi_pm *p) { return (uint32_t)((__int128)mvm_now(p->vm) * PM_TMR_HZ / 1000000000LL); }

static void pm_update(acpi_pm *p)
{
    bool fixed = (p->sts & p->en & (STS_TMR | STS_PWRBTN | (1u << 5) | (1u << 10))) != 0;
    bool gpe = (p->gpe_sts & p->gpe_en) != 0;
    int level = (p->cnt & CNT_SCI_EN) && (fixed || gpe);
    if (level != p->level) {
        p->level = level;
        irq_set(&p->sci, level);
    }
}

/* com TMR_EN, TMR_STS e ligado quando o bit 31 do contador muda */
static void pm_timer_arm(acpi_pm *p)
{
    timer_del(p->vm, &p->tmr);
    if (!(p->en & STS_TMR))
        return;
    uint64_t now_ticks = (uint64_t)((__int128)mvm_now(p->vm) * PM_TMR_HZ / 1000000000LL);
    uint64_t next = (now_ticks | 0x7fffffffULL) + 1;
    int64_t ns = (int64_t)((__int128)next * 1000000000LL / PM_TMR_HZ) + 1;
    timer_mod(p->vm, &p->tmr, ns);
}

static void pm_timer_cb(void *opaque)
{
    acpi_pm *p = opaque;
    p->sts |= STS_TMR;
    pm_update(p);
    pm_timer_arm(p);
}

static uint16_t pm_reg16(acpi_pm *p, unsigned reg)
{
    switch (reg) {
    case 0: return p->sts;
    case 2: return p->en;
    case 4: return p->cnt;
    default: return 0;
    }
}

static void pm_write16(acpi_pm *p, unsigned reg, uint16_t v, uint16_t mask)
{
    switch (reg) {
    case 0: /* escreve 1 para limpar */
        p->sts &= (uint16_t)~(v & mask);
        break;
    case 2:
        p->en = (uint16_t)((p->en & ~mask) | (v & mask));
        pm_timer_arm(p);
        break;
    case 4: {
        uint16_t cnt = (uint16_t)((p->cnt & ~mask) | (v & mask));
        if (cnt & CNT_SLP_EN) {
            unsigned typ = (cnt >> 10) & 7;
            cnt &= (uint16_t)~CNT_SLP_EN;
            if (typ == 0) { /* _S5 */
                LOGI("ACPI: desligamento (S5)");
                {   /* quem pediu (pilha simbolizada do convidado) */
                    extern void x86_debug_dump(void *cpu);
                    x86_debug_dump(p->vm->cpu);
                }
                vm_request_shutdown(p->vm);
            } else {
                LOGW("ACPI: estado de suspensao %u nao suportado", typ);
                p->sts |= STS_WAK;
            }
        }
        p->cnt = cnt;
        break;
    }
    default: break;
    }
    pm_update(p);
}

static uint64_t pm_read(void *opaque, uint64_t off, unsigned size)
{
    acpi_pm *p = opaque;
    if (off >= 8 && off < 12) { /* PM timer */
        uint32_t t = pm_timer(p);
        return (t >> (8 * (off - 8))) & (size >= 4 ? 0xffffffffu : (1u << (8 * size)) - 1);
    }
    uint64_t v = 0;
    for (unsigned i = 0; i < size; i++) {
        unsigned o = (unsigned)off + i;
        v |= (uint64_t)((pm_reg16(p, o & ~1u) >> (8 * (o & 1))) & 0xff) << (8 * i);
    }
    return v;
}

static void pm_write(void *opaque, uint64_t off, uint64_t v, unsigned size)
{
    acpi_pm *p = opaque;
    for (unsigned i = 0; i < size;) {
        unsigned o = (unsigned)off + i;
        if (!(o & 1) && size - i >= 2) {
            pm_write16(p, o, (uint16_t)(v >> (8 * i)), 0xffff);
            i += 2;
        } else {
            unsigned sh = 8 * (o & 1);
            pm_write16(p, o & ~1u, (uint16_t)(((v >> (8 * i)) & 0xff) << sh), (uint16_t)(0xff << sh));
            i++;
        }
    }
}

const mvm_io_ops acpi_pm_ops = {pm_read, pm_write};

static uint64_t gpe_read(void *opaque, uint64_t off, unsigned size)
{
    acpi_pm *p = opaque;
    uint32_t regs = p->gpe_sts | ((uint32_t)p->gpe_en << 16);
    return (regs >> (8 * off)) & (size >= 4 ? 0xffffffffu : (1u << (8 * size)) - 1);
}

static void gpe_write(void *opaque, uint64_t off, uint64_t v, unsigned size)
{
    acpi_pm *p = opaque;
    for (unsigned i = 0; i < size && off + i < 4; i++) {
        unsigned o = (unsigned)off + i;
        uint8_t b = (uint8_t)(v >> (8 * i));
        if (o < 2)
            p->gpe_sts &= (uint16_t)~(b << (8 * o));
        else
            p->gpe_en = (uint16_t)((p->gpe_en & ~(0xff << (8 * (o - 2)))) | (b << (8 * (o - 2))));
    }
    pm_update(p);
}

const mvm_io_ops acpi_gpe_ops = {gpe_read, gpe_write};

void acpi_pm_reset(acpi_pm *p)
{
    timer_del(p->vm, &p->tmr);
    p->sts = p->en = 0;
    p->cnt = CNT_SCI_EN; /* sem SMI: o hardware ja inicia em modo ACPI */
    p->gpe_sts = p->gpe_en = 0;
    pm_update(p);
}

acpi_pm *acpi_pm_new(mvm_vm *vm, irq_line sci)
{
    acpi_pm *p = calloc(1, sizeof(*p));
    p->vm = vm;
    p->sci = sci;
    timer_init(&p->tmr, pm_timer_cb, p);
    acpi_pm_reset(p);
    return p;
}

void acpi_pm_free(acpi_pm *p)
{
    if (!p)
        return;
    timer_del(p->vm, &p->tmr);
    free(p);
}

void acpi_pm_power_button(acpi_pm *p)
{
    p->sts |= STS_PWRBTN;
    pm_update(p);
}
