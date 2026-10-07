/*
 * Maquina PC (i386 e x86-64): chipset legado (PIC, PIT, RTC, i8042, COM1),
 * PCI com virtio-blk legado, framebuffer linear em 0xFD000000.
 *
 * Boot:
 *   - bzImage: protocolo de boot do Linux (entrada de 32 bits, boot_params);
 *   - ELF32: modo protegido plano; ELF64: modo longo com mapeamento 1:1;
 *   - firmware (BIOS): vetor de reset F000:FFF0;
 *   - binario bruto: modo real em 0000:7C00 (ou --load-addr).
 */
#include "../cpu/cpu_x86.h"
#include "../dev/acpi.h"
#include "../dev/ahci.h"
#include "../dev/apic.h"
#include "../dev/devices.h"
#include "../dev/hpet.h"
#include "../dev/ide.h"
#include "../dev/pci.h"
#include "../dev/vga.h"

#include <stdlib.h>

#define PC_FB_BASE 0xfd000000ULL
#define PC_BOOT_PARAMS 0x10000ULL
#define PC_CMDLINE 0x20000ULL
#define PC_GDT 0x1000ULL
#define PC_PAGETABLES 0x70000ULL
#define PC_KERNEL 0x100000ULL
#define VIRTIO_IO_BASE 0xc000
#define IDE_BM_BASE 0xc100
/* E/S e MMIO fixos das placas de rede e de som (o SeaBIOS realoca com BIOS) */
#define VIRTIO_NET_IO 0xc800
#define RTL8139_IO 0xc400
#define E1000_IO 0xc500
#define AC97_NAM_IO 0xc600
#define AC97_NABM_IO 0xc700
#define RTL8139_MMIO 0xfebd0000u
#define HDA_MMIO 0xfebe0000u
#define E1000_MMIO 0xfeb80000u
#define VGA_LFB_BASE 0xfc000000u
#define AHCI_ABAR_BASE 0xfebf0000u
#define VGA_VRAM_MB 16
#define DISPLAY_MAX_W 2560
#define DISPLAY_MAX_H 1600

typedef struct {
    i8259 *pic;
    lapic *lapic;
    acpi_pm *pm;
    hpet *hpet;
    ioapic *ioapic;
    i8254 *pit;
    cmos *rtc;
    uart16550 *com1;
    i8042 *kbd;
    pci_bus *pci;
    virtio_dev *vdev[MVM_MAX_DISKS + 2]; /* discos + rede */
    pci_dev *vpci[MVM_MAX_DISKS + 2];
    int nvdev;
    uint8_t *vga;       /* janela 0xA0000-0xBFFFF (sem VGA emulado) */
    uint8_t *bios;
    size_t bios_size;
    uint8_t port92;
    bool bios_mode;     /* firmware + VGA + IDE + fw_cfg */
    pci_dev *piix3;
    fw_cfg *fw;
    vga *vgadev;
    pci_dev *vga_pci;
    ide_ctrl *ide;
    pci_dev *ide_pci;
    int ide_disks, ide_cds;
    ahci *ahci;
    bool booted; /* ja passou pelo primeiro boot (resets seguintes sao quentes) */
    pci_dev *ahci_pci;
    int sata_disks, sata_cds;
    i8237 *dma;
    fdc *fdc;
    rtl8139 *rtl;
    e1000 *e1k;
    ac97 *ac97;
    hda *hda;
    uint8_t fd_cmos[2];  /* tipo de cada drive de disquete no CMOS (0 = ausente) */
    /* roteamento de INTx PCI -> IRQ (registradores PIRQ do PIIX3) */
    int nsrc;
    uint8_t src_pirq[32];
    uint32_t pirq_src[4];
    uint8_t irq_level[16];
} pc_machine;

typedef struct {
    pc_machine *m;
    int src;
} pci_irq_ctx;

static void cpu_intr(void *cpu, int level) { x86_set_intr(cpu, level); }

static int intr_ack(void *opaque)
{
    pc_machine *m = opaque;
    return lapic_ack(m->lapic);
}

/* IRQ ISA: vai ao 8259 e ao IOAPIC (IRQ0 do PIT tambem no pino 2, como no QEMU) */
static void isa_irq_set(void *opaque, int n, int level)
{
    pc_machine *m = opaque;
    /* no modo legado do HPET o PIT e o RTC ficam desligados das IRQs 0 e 8 */
    if ((n == 0 || n == 8) && m->hpet && hpet_legacy(m->hpet))
        return;
    i8259_set_irq(m->pic, n, level);
    ioapic_set_irq(m->ioapic, n, level);
    if (n == 0)
        ioapic_set_irq(m->ioapic, 2, level);
}

static irq_line pic_line(pc_machine *m, int n) { return (irq_line){isa_irq_set, m, n}; }

/* saidas do HPET: IRQ0/IRQ8 (modo legado) ou um pino do IOAPIC */
static void hpet_irq(void *opaque, int line, int level)
{
    pc_machine *m = opaque;
    if (line == HPET_LINE_IRQ0) {
        i8259_set_irq(m->pic, 0, level);
        ioapic_set_irq(m->ioapic, 2, level);
    } else if (line == HPET_LINE_IRQ8) {
        i8259_set_irq(m->pic, 8, level);
        ioapic_set_irq(m->ioapic, 8, level);
    } else {
        ioapic_set_irq(m->ioapic, line, level);
    }
}

/* ------------------------------------------------ roteamento PCI (PIRQ) */

static void pirq_update(pc_machine *m)
{
    uint8_t lvl[16] = {0};
    for (int p = 0; p < 4; p++) {
        uint8_t route = m->piix3 ? m->piix3->cfg[0x60 + p] : 0x80;
        if (!(route & 0x80) && (route & 0x0f) && m->pirq_src[p])
            lvl[route & 0x0f] = 1;
    }
    for (int i = 0; i < 16; i++) {
        if (lvl[i] != m->irq_level[i]) {
            m->irq_level[i] = lvl[i];
            i8259_set_irq(m->pic, i, lvl[i]);
            ioapic_set_irq(m->ioapic, i, lvl[i]);
        }
    }
    /* PIRQA-D tambem nos pinos 16-19 do IOAPIC (GSI usados por tabelas ACPI) */
    for (int p = 0; p < 4; p++)
        ioapic_set_irq(m->ioapic, 16 + p, m->pirq_src[p] != 0);
}

static void pci_irq_set(void *opaque, int src, int level)
{
    pc_machine *m = opaque;
    int p = m->src_pirq[src & 31];
    if (level)
        m->pirq_src[p] |= 1u << src;
    else
        m->pirq_src[p] &= ~(1u << src);
    pirq_update(m);
}

/* linha de interrupcao para um dispositivo PCI (slot, pino 1=INTA) */
static irq_line pci_line(pc_machine *m, int slot, int pin, int *routed_irq)
{
    int src = m->nsrc++ & 31;
    int pirq = (slot + pin - 1) & 3;
    m->src_pirq[src] = (uint8_t)pirq;
    if (routed_irq)
        *routed_irq = m->piix3 ? m->piix3->cfg[0x60 + pirq] & 0x0f : 11;
    return (irq_line){pci_irq_set, m, src};
}

static void piix3_cfg_write(pci_dev *d, unsigned off, uint32_t val, unsigned size)
{
    (void)val;
    pc_machine *m = d->priv;
    if (off < 0x64 && off + size > 0x60)
        pirq_update(m);
}

/* --------------------------------------------------------- portas diversas */

static uint64_t sysctl_read(void *opaque, uint64_t off, unsigned size)
{
    mvm_vm *vm = opaque;
    pc_machine *m = vm->mach;
    (void)size;
    switch (off) {
    case 0x61: return i8254_port61_read(m->pit);
    case 0x92: return m->port92;
    default: return 0xff;
    }
}

static void sysctl_write(void *opaque, uint64_t off, uint64_t v, unsigned size)
{
    mvm_vm *vm = opaque;
    pc_machine *m = vm->mach;
    (void)size;
    switch (off) {
    case 0x61: i8254_port61_write(m->pit, (uint8_t)v); break;
    case 0x92:
        x86_set_a20(vm->cpu, v & 2);
        if (v & 1) {
            LOGI("porta 0x92: reset");
            vm_request_guest_reset(vm);
        }
        m->port92 = (uint8_t)(v & ~1u);
        break;
    default: break;
    }
}

static const mvm_io_ops sysctl_ops = {sysctl_read, sysctl_write};

static uint64_t dummy_read(void *opaque, uint64_t off, unsigned size) { (void)opaque; (void)off; (void)size; return 0xff; }
static void dummy_write(void *opaque, uint64_t off, uint64_t v, unsigned size) { (void)opaque; (void)off; (void)v; (void)size; }
static const mvm_io_ops dummy_ops = {dummy_read, dummy_write};

static void debugcon_write(void *opaque, uint64_t off, uint64_t v, unsigned size)
{
    static char line[256];
    static size_t n;
    (void)opaque; (void)off; (void)size;
    char ch = (char)v;
    if (ch == '\n' || n == sizeof(line) - 1) {
        line[n] = 0;
        LOGD("debugcon: %s", line);
        n = 0;
    } else {
        line[n++] = ch;
    }
}
static const mvm_io_ops debugcon_ops = {dummy_read, debugcon_write};

static void exit_write(void *opaque, uint64_t off, uint64_t v, unsigned size)
{
    mvm_vm *vm = opaque;
    (void)off; (void)size;
    LOGI("porta de saida de depuracao: codigo %llu", (unsigned long long)v);
    vm_request_shutdown(vm);
}
static const mvm_io_ops exit_ops = {dummy_read, exit_write};

static void reset_write(void *opaque, uint64_t off, uint64_t v, unsigned size)
{
    mvm_vm *vm = opaque;
    (void)off; (void)size;
    if (v & 4) {
        LOGI("porta 0xCF9: reset");
        vm_request_guest_reset(vm);
    }
}
static const mvm_io_ops reset_ops = {dummy_read, reset_write};

/* porta de comando do i8042 (0x64) mapeada no offset 4 do dispositivo */
static uint64_t kbdc_read(void *o, uint64_t off, unsigned sz) { (void)off; return i8042_ops.read(o, 4, sz); }
static void kbdc_write(void *o, uint64_t off, uint64_t v, unsigned sz) { (void)off; i8042_ops.write(o, 4, v, sz); }
static const mvm_io_ops kbdc_ops = {kbdc_read, kbdc_write};

/* janela VGA (apenas memoria) */
static uint64_t vga_read(void *opaque, uint64_t off, unsigned size) { return ld_le((uint8_t *)opaque + off, size); }
static void vga_write(void *opaque, uint64_t off, uint64_t v, unsigned size) { st_le((uint8_t *)opaque + off, v, size); }
static const mvm_io_ops vga_ops = {vga_read, vga_write};

/* ------------------------------------------------------------------ boot */

static void cmos_setup(mvm_vm *vm, pc_machine *m)
{
    uint64_t ext_kb = vm->ram_size > (1 << 20) ? (vm->ram_size >> 10) - 1024 : 0;
    if (ext_kb > 65535) ext_kb = 65535;
    cmos_set(m->rtc, 0x15, 640 & 0xff);
    cmos_set(m->rtc, 0x16, 640 >> 8);
    cmos_set(m->rtc, 0x17, (uint8_t)ext_kb);
    cmos_set(m->rtc, 0x18, (uint8_t)(ext_kb >> 8));
    cmos_set(m->rtc, 0x30, (uint8_t)ext_kb);
    cmos_set(m->rtc, 0x31, (uint8_t)(ext_kb >> 8));
    uint64_t above16 = vm->ram_size > (16 << 20) ? (vm->ram_size - (16 << 20)) >> 16 : 0;
    if (above16 > 65535) above16 = 65535;
    cmos_set(m->rtc, 0x34, (uint8_t)above16);
    cmos_set(m->rtc, 0x35, (uint8_t)(above16 >> 8));
    /* equipamento: coprocessador, VGA; bit 0 e bits 6-7 = disquetes (quantidade - 1) */
    int nfd = (m->fd_cmos[0] != 0) + (m->fd_cmos[1] != 0);
    cmos_set(m->rtc, 0x14, (uint8_t)(0x06 | (nfd ? 0x01 | ((nfd - 1) << 6) : 0)));
    cmos_set(m->rtc, 0x10, (uint8_t)((m->fd_cmos[0] << 4) | m->fd_cmos[1]));
    cmos_set(m->rtc, 0x3d, 0x02); /* ordem de boot: disco */
}

static void write_gdt(mvm_vm *vm)
{
    uint64_t gdt[6] = {
        0,
        0,
        0x00cf9a000000ffffULL, /* 0x10: codigo 32 bits */
        0x00cf92000000ffffULL, /* 0x18: dados */
        0,
        0x00af9a000000ffffULL, /* 0x28: codigo 64 bits */
    };
    memcpy(vm->ram + PC_GDT, gdt, sizeof(gdt));
}

static void add_e820(uint8_t *bp, int *n, uint64_t addr, uint64_t size, uint32_t type)
{
    uint8_t *e = bp + 0x2d0 + 20 * (*n);
    st_le(e, addr, 8);
    st_le(e + 8, size, 8);
    st_le(e + 16, type, 4);
    (*n)++;
}

static int boot_bzimage(mvm_vm *vm, uint8_t *img, size_t len, char *err, size_t errlen)
{
    unsigned setup_sects = img[0x1f1] ? img[0x1f1] : 4;
    uint16_t ver = (uint16_t)ld_le(img + 0x206, 2);
    size_t setup_size = (setup_sects + 1) * 512;
    if (ver < 0x0202 || setup_size >= len) {
        snprintf(err, errlen, "bzImage antigo demais (protocolo %x)", ver);
        return -1;
    }
    size_t ksize = len - setup_size;
    if (PC_KERNEL + ksize > vm->ram_size) {
        snprintf(err, errlen, "kernel nao cabe na RAM");
        return -1;
    }
    memcpy(vm->ram + PC_KERNEL, img + setup_size, ksize);

    uint8_t *bp = vm->ram + PC_BOOT_PARAMS;
    memset(bp, 0, 4096);
    size_t hdr_end = 0x202 + img[0x201];
    if (hdr_end > 0x280) hdr_end = 0x280;
    memcpy(bp + 0x1f1, img + 0x1f1, hdr_end - 0x1f1);
    bp[0x210] = 0xff;                         /* type_of_loader */
    bp[0x211] |= 0x80;                        /* CAN_USE_HEAP */
    st_le(bp + 0x224, 0xfe00, 2);             /* heap_end_ptr */
    st_le(bp + 0x1fa, 0xffff, 2);             /* vid_mode */
    const char *cmd = vm->cfg.cmdline ? vm->cfg.cmdline : "";
    size_t cl = strlen(cmd);
    if (cl > 4095) cl = 4095;
    memcpy(vm->ram + PC_CMDLINE, cmd, cl);
    vm->ram[PC_CMDLINE + cl] = 0;
    st_le(bp + 0x228, PC_CMDLINE, 4);

    if (vm->cfg.initrd) {
        size_t rlen;
        uint8_t *rd = load_file(vm->cfg.initrd, &rlen, err, errlen);
        if (!rd) return -1;
        uint64_t max = ver >= 0x0203 ? ld_le(img + 0x22c, 4) : 0x37ffffff;
        uint64_t top = vm->ram_size < max + 1 ? vm->ram_size : max + 1;
        uint64_t addr = (top - rlen) & ~0xfffffULL;
        if (addr < PC_KERNEL + ksize * 4 || rlen > top) {
            snprintf(err, errlen, "initrd nao cabe na RAM");
            free(rd);
            return -1;
        }
        memcpy(vm->ram + addr, rd, rlen);
        st_le(bp + 0x218, addr, 4);
        st_le(bp + 0x21c, rlen, 4);
        free(rd);
        LOGI("initrd em 0x%llx (%zu bytes)", (unsigned long long)addr, rlen);
    }

    int n = 0;
    add_e820(bp, &n, 0, 0x9fc00, 1);
    add_e820(bp, &n, 0x9fc00, 0x400, 2);
    add_e820(bp, &n, 0xe0000, 0x20000, 2);
    add_e820(bp, &n, 0x100000, vm->ram_size - 0x100000, 1);
    bp[0x1e8] = (uint8_t)n;

    if (vm->fb) { /* screen_info: framebuffer linear (VLFB) */
        bp[0x0f] = 0x23;
        st_le(bp + 0x12, vm->fb_w, 2);
        st_le(bp + 0x14, vm->fb_h, 2);
        st_le(bp + 0x16, 32, 2);
        st_le(bp + 0x18, (uint32_t)PC_FB_BASE, 4);
        st_le(bp + 0x1c, (uint32_t)((vm->fb_size + 0xffff) >> 16), 4);
        st_le(bp + 0x24, vm->fb_stride, 2);
        bp[0x26] = 8; bp[0x27] = 16; bp[0x28] = 8; bp[0x29] = 8;
        bp[0x2a] = 8; bp[0x2b] = 0; bp[0x2c] = 8; bp[0x2d] = 24;
        st_le(bp + 0x32, 1, 2);
    }

    write_gdt(vm);
    x86_setup_flat32(vm->cpu, (uint32_t)PC_KERNEL, (uint32_t)PC_GDT, (uint32_t)PC_BOOT_PARAMS);
    LOGI("bzImage (protocolo %x.%02x) carregado: %zu bytes", ver >> 8, ver & 0xff, ksize);
    return 0;
}

static void build_identity_pt(mvm_vm *vm)
{
    /* PML4 -> PDPT -> 4 PDs com paginas de 2 MiB cobrindo 4 GiB */
    uint8_t *pt = vm->ram + PC_PAGETABLES;
    memset(pt, 0, 6 * 4096);
    uint64_t pml4 = PC_PAGETABLES, pdpt = pml4 + 4096, pd = pdpt + 4096;
    st_le(pt, pdpt | 3, 8);
    for (int i = 0; i < 4; i++) {
        st_le(pt + 4096 + 8 * i, (pd + 4096ULL * i) | 3, 8);
        for (int j = 0; j < 512; j++) {
            uint64_t addr = ((uint64_t)i << 30) | ((uint64_t)j << 21);
            st_le(pt + 8192 + 4096 * i + 8 * j, addr | 0x83, 8);
        }
    }
}

/* ------------------------------------------------------------ ACPI */

#define PC_ACPI_RSDP 0xe0000ULL   /* area 0xE0000-0xFFFFF, reservada no e820 */
#define PC_ACPI_TABLES 0xe0040ULL

static void acpi_config_for(mvm_vm *vm, acpi_config *c)
{
    c->pci_hole_start = vm->ram_size > 0x80000000ULL ? 0xc0000000u : 0x80000000u;
    c->ioapic_id = 0;
    c->hpet_block_id = hpet_block_id();
    pc_machine *m = vm->mach;
    c->floppies = (uint8_t)((m->fd_cmos[0] ? 1 : 0) | (m->fd_cmos[1] ? 2 : 0));
}

static int load_boot(mvm_vm *vm, pc_machine *m, char *err, size_t errlen)
{
    /* Com BIOS, um reset preserva a RAM como num PC real (e no QEMU): o SeaBIOS
     * consulta o CMOS 0x0F e pode retomar por um vetor guardado na RAM. So a
     * copia da BIOS em 0xE0000-0xFFFFF e restaurada abaixo. */
    if (!(m->bios_mode && m->booted))
        memset(vm->ram, 0, vm->ram_size);
    m->booted = true;
    if (m->vga)
        memset(m->vga, 0, 0x20000);
    x86_cpu *cpu = vm->cpu;
    vm->cpu_ops->reset(cpu);
    x86_set_a20(cpu, true);
    cmos_setup(vm, m);

    if (m->bios) {
        size_t low = m->bios_size < 0x20000 ? m->bios_size : 0x20000;
        memcpy(vm->ram + 0x100000 - low, m->bios + m->bios_size - low, low);
    }

    if (!vm->cfg.kernel) {
        if (!m->bios) {
            snprintf(err, errlen, "nenhum kernel ou firmware informado");
            return -1;
        }
        x86_setup_realmode(cpu, 0xf000, 0xfff0);
        cpu->seg[S_CS].base = 0xffff0000;
        return 0;
    }

    size_t len;
    uint8_t *img = load_file(vm->cfg.kernel, &len, err, errlen);
    if (!img)
        return -1;
    int r = 0;
    if (len > 0x260 && !memcmp(img + 0x202, "HdrS", 4) && ld_le(img + 0x1fe, 2) == 0xaa55) {
        r = boot_bzimage(vm, img, len, err, errlen);
        if (r == 0) { /* tabelas ACPI para o kernel (ele procura o RSDP em 0xE0000) */
            acpi_config ac;
            acpi_tables at;
            acpi_config_for(vm, &ac);
            acpi_build(&at, &ac);
            acpi_install(&at, vm->ram, PC_ACPI_RSDP, PC_ACPI_TABLES);
            const char *dump = getenv("MVM_ACPI_DUMP"); /* depuracao: grava a area das tabelas */
            if (dump) {
                FILE *f = fopen(dump, "wb");
                if (f) {
                    fwrite(vm->ram + PC_ACPI_RSDP, 1, (size_t)(PC_ACPI_TABLES - PC_ACPI_RSDP) + at.len, f);
                    fclose(f);
                }
            }
            acpi_tables_free(&at);
            st_le(vm->ram + PC_BOOT_PARAMS + 0x070, PC_ACPI_RSDP, 8); /* acpi_rsdp_addr */
        }
    } else if (elf_probe(img, len)) {
        elf_info ei;
        if (!elf_load(&vm->mem, img, len, &ei, err, errlen)) {
            r = -1;
        } else {
            write_gdt(vm);
            if (ei.is64) {
                if (!cpu->cpuid_lm) {
                    snprintf(err, errlen, "ELF de 64 bits requer a arquitetura x86_64");
                    r = -1;
                } else {
                    build_identity_pt(vm);
                    x86_setup_long64(cpu, ei.entry, PC_PAGETABLES, (uint32_t)PC_GDT, 0);
                }
            } else {
                x86_setup_flat32(cpu, (uint32_t)ei.entry, (uint32_t)PC_GDT, 0);
            }
            LOGI("ELF%d carregado: entrada 0x%llx", ei.is64 ? 64 : 32, (unsigned long long)ei.entry);
        }
    } else {
        uint64_t addr = vm->cfg.raw_load_addr ? vm->cfg.raw_load_addr : 0x7c00;
        if (addr + len > vm->ram_size) {
            snprintf(err, errlen, "binario bruto fora da RAM");
            r = -1;
        } else {
            memcpy(vm->ram + addr, img, len);
            if (addr < 0x100000)
                x86_setup_realmode(cpu, (uint16_t)((addr >> 4) & 0xf000), (uint16_t)(addr & 0xffff));
            else
                x86_setup_flat32(cpu, (uint32_t)addr, (uint32_t)PC_GDT, 0);
            write_gdt(vm);
            LOGI("binario bruto em 0x%llx", (unsigned long long)addr);
        }
    }
    free(img);
    return r;
}

/* ------------------------------------------------------------ ciclo de vida */

/* o disco i existe (com midia, ou CD/disquete vazio) */
static bool has_drive(mvm_vm *vm, int i)
{
    if (vm->disks[i])
        return true;
    mvm_disk_type t = vm->cfg.disks[i].type;
    return vm->cfg.disks[i].empty &&
           (t == MVM_DISK_CDROM || t == MVM_DISK_SATA_CDROM || t == MVM_DISK_FLOPPY);
}

static int ide_kind(pc_machine *m, int bus, int unit) { return m->ide ? ide_drive_kind(m->ide, bus, unit) : 0; }

/* ------------------------------------------------------------ fw_cfg */

static void setup_fw_cfg(mvm_vm *vm, pc_machine *m)
{
    fw_cfg *f = m->fw;
    fw_cfg_add_u64(f, 0x03, vm->ram_size);        /* RAM_SIZE */
    uint16_t one = 1, zero16 = 0;
    fw_cfg_add_bytes(f, 0x05, &one, 2);            /* NB_CPUS */
    fw_cfg_add_bytes(f, 0x0f, &one, 2);            /* MAX_CPUS */
    fw_cfg_add_bytes(f, 0x0e, &zero16, 2);         /* BOOT_MENU */
    uint64_t numa[1] = {0};
    fw_cfg_add_bytes(f, 0x0d, numa, 8);            /* NUMA: 0 nos */

    uint8_t e820[20];
    st_le(e820, 0, 8);
    st_le(e820 + 8, vm->ram_size, 8);
    st_le(e820 + 16, 1, 4);
    fw_cfg_add_file(f, "etc/e820", e820, sizeof(e820));

    uint16_t menu = 0;
    fw_cfg_add_file(f, "etc/show-boot-menu", &menu, 2);
    uint32_t fail_wait = 10000;
    fw_cfg_add_file(f, "etc/boot-fail-wait", &fail_wait, 4);

    /* ordem de boot: 'c' = discos, 'd' = CD-ROM, 'a' = disquete */
    const char *order = vm->cfg.boot_order;
    if (!order || !*order)
        order = (m->ide_cds || m->sata_cds) ? "dca" : "cda";
    char boot[1024];
    size_t n = 0;
    for (const char *o = order; *o && n + 200 < sizeof(boot); o++) {
        for (int bus = 0; bus < 2; bus++)
            for (int unit = 0; unit < 2; unit++) {
                int kind = ide_kind(m, bus, unit);
                if ((*o == 'd' && kind == 2) || (*o == 'c' && kind == 1))
                    n += (size_t)snprintf(boot + n, sizeof(boot) - n, "/pci@i0cf8/ide@1,1/drive@%d/disk@%d\n", bus, unit);
            }
        if (m->ahci)
            for (int port = 0; port < AHCI_PORTS; port++) {
                int kind = ahci_port_kind(m->ahci, port);
                if ((*o == 'd' && kind == 2) || (*o == 'c' && kind == 1))
                    n += (size_t)snprintf(boot + n, sizeof(boot) - n, "/pci@i0cf8/sata@%x/drive@%x/disk@0\n",
                                          m->ahci_pci->slot, port);
            }
        if (*o == 'a')
            for (int d = 0; d < 2; d++)
                if (m->fd_cmos[d])
                    n += (size_t)snprintf(boot + n, sizeof(boot) - n, "/pci@i0cf8/isa@1/fdc@03f0/floppy@%d\n", d);
        if (*o == 'c')
            for (int i = 0; i < m->nvdev; i++)
                n += (size_t)snprintf(boot + n, sizeof(boot) - n, "/pci@i0cf8/scsi@%x\n", m->vpci[i]->slot);
    }
    fw_cfg_add_file(f, "bootorder", boot, (uint32_t)n);

    acpi_config ac;
    acpi_tables at;
    acpi_config_for(vm, &ac);
    acpi_build(&at, &ac);
    acpi_add_to_fw_cfg(&at, f);
    acpi_tables_free(&at);
}

static char *default_vgabios(const char *bios)
{
    const char *slash = strrchr(bios, '/');
    size_t dl = slash ? (size_t)(slash - bios + 1) : 0;
    char *p = malloc(dl + 32);
    memcpy(p, bios, dl);
    strcpy(p + dl, "vgabios-stdvga.bin");
    return p;
}

static int pc_init(mvm_vm *vm, char *err, size_t errlen)
{
    pc_machine *m = calloc(1, sizeof(*m));
    vm->mach = m;
    vm->ram_base = 0;
    vm->cpu = x86_cpu_new(vm, vm->cfg.arch == MVM_ARCH_X86_64);
    vm->cpu_ops = &x86_cpu_ops;
    x86_set_intr_ack(vm->cpu, intr_ack, m);
    m->bios_mode = vm->cfg.firmware && !vm->cfg.kernel;

    if (vm->cfg.firmware) {
        m->bios = load_file(vm->cfg.firmware, &m->bios_size, err, errlen);
        if (!m->bios)
            return -1;
        if (m->bios_size < 0x10000 || m->bios_size > 0x400000 || (m->bios_size & 0xffff)) {
            snprintf(err, errlen, "tamanho de BIOS invalido (%zu)", m->bios_size);
            return -1;
        }
        space_add_ram(&vm->mem, 0x100000000ULL - m->bios_size, m->bios_size, m->bios, true, "bios");
    }

    m->lapic = lapic_new(vm, vm->cpu, cpu_intr, NULL);
    m->pic = i8259_new(vm, lapic_pic_intr, m->lapic);
    lapic_set_pic(m->lapic, m->pic);
    m->ioapic = ioapic_new(vm, m->lapic);
    x86_set_apic(vm->cpu, m->lapic, lapic_get_base, lapic_set_base, lapic_get_tpr, lapic_set_tpr);
    space_add_io(&vm->mem, LAPIC_MMIO_BASE, 0x1000, &lapic_mmio_ops, m->lapic, "lapic");
    space_add_io(&vm->mem, IOAPIC_MMIO_BASE, 0x1000, &ioapic_mmio_ops, m->ioapic, "ioapic");
    m->hpet = hpet_new(vm, hpet_irq, m);
    space_add_io(&vm->mem, HPET_MMIO_BASE, 0x400, &hpet_mmio_ops, m->hpet, "hpet");
    m->pit = i8254_new(vm, pic_line(m, 0));
    m->rtc = cmos_new(vm, pic_line(m, 8));
    m->com1 = uart16550_new(vm, &vm->serial, pic_line(m, 4));
    m->kbd = i8042_new(vm, pic_line(m, 1), pic_line(m, 12));

    space_add_io(&vm->io, 0x20, 2, &i8259_master_ops, m->pic, "pic1");
    space_add_io(&vm->io, 0xa0, 2, &i8259_slave_ops, m->pic, "pic2");
    space_add_io(&vm->io, 0x4d0, 2, &i8259_elcr_ops, m->pic, "elcr");
    space_add_io(&vm->io, 0x40, 4, &i8254_ops, m->pit, "pit");
    space_add_io(&vm->io, 0x60, 1, &i8042_ops, m->kbd, "i8042-data");
    space_add_io(&vm->io, 0x64, 1, &kbdc_ops, m->kbd, "i8042-cmd");
    space_add_io(&vm->io, 0x61, 1, &sysctl_ops, vm, "port61");
    space_add_io(&vm->io, 0x70, 2, &cmos_ops, m->rtc, "rtc");
    space_add_io(&vm->io, 0x80, 1, &dummy_ops, NULL, "post");
    space_add_io(&vm->io, 0x92, 1, &sysctl_ops, vm, "port92");
    space_add_io(&vm->io, 0x3f8, 8, &uart16550_ops, m->com1, "com1");
    space_add_io(&vm->io, 0x402, 1, &debugcon_ops, NULL, "debugcon");
    space_add_io(&vm->io, 0xe9, 1, &debugcon_ops, NULL, "debugcon-e9");
    space_add_io(&vm->io, 0x501, 1, &exit_ops, vm, "debug-exit");
    m->pm = acpi_pm_new(vm, pic_line(m, ACPI_SCI_IRQ));
    space_add_io(&vm->io, ACPI_PM_BASE, 0x40, &acpi_pm_ops, m->pm, "acpi-pm");
    space_add_io(&vm->io, ACPI_GPE0_BASE, 4, &acpi_gpe_ops, m->pm, "acpi-gpe");
    space_add_io(&vm->io, 0xcf9, 1, &reset_ops, vm, "rst");
    /* controladores DMA 8237 e registradores de pagina */
    m->dma = i8237_new(vm);
    space_add_io(&vm->io, 0x00, 0x10, &i8237_dma1_ops, m->dma, "dma1");
    space_add_io(&vm->io, 0x81, 0x0f, &i8237_page_ops, m->dma, "dma-page");
    space_add_io(&vm->io, 0xc0, 0x20, &i8237_dma2_ops, m->dma, "dma2");

    /* controlador de disquete, so quando ha disquetes */
    for (int i = 0; i < MVM_MAX_DISKS; i++)
        if (has_drive(vm, i) && vm->cfg.disks[i].type == MVM_DISK_FLOPPY && !m->fdc) {
            m->fdc = fdc_new(vm, pic_line(m, 6), m->dma);
            space_add_io(&vm->io, 0x3f0, 6, &fdc_ops, m->fdc, "fdc");
            space_add_io(&vm->io, 0x3f7, 1, &fdc_dir_ops, m->fdc, "fdc-dir");
        }

    m->pci = pci_bus_new(vm);
    space_add_io(&vm->io, 0xcf8, 8, &pci_host_ops, m->pci, "pci");
    pci_finalize(pci_add(m->pci, 0, 0x8086, 0x1237, 0x06000002)); /* i440FX */
    m->piix3 = pci_add(m->pci, 1, 0x8086, 0x7000, 0x06010000);     /* PIIX3 ISA */
    m->piix3->cfg[0x0e] = 0x80; /* multifuncao */
    m->piix3->priv = m;
    m->piix3->cfg_write = piix3_cfg_write;
    /* PIRQA-D: 10, 10, 11, 11 (como o SeaBIOS); com BIOS, ele reprograma */
    static const uint8_t pirq_default[4] = {10, 10, 11, 11};
    for (int i = 0; i < 4; i++)
        m->piix3->cfg[0x60 + i] = m->bios_mode ? 0x80 : pirq_default[i];
    pci_finalize(m->piix3);

    /* discos: IDE (BIOS ou tipo explicito) ou virtio-blk */
    bool need_ide = m->bios_mode;
    for (int i = 0; i < MVM_MAX_DISKS; i++)
        if (has_drive(vm, i) && (vm->cfg.disks[i].type == MVM_DISK_IDE || vm->cfg.disks[i].type == MVM_DISK_CDROM))
            need_ide = true;
    if (need_ide) {
        m->ide = ide_new(vm, pic_line(m, 14), pic_line(m, 15));
        m->ide_pci = pci_add_fn(m->pci, 1, 1, 0x8086, 0x7010, 0x01018000);
        m->ide_pci->cfg[0x41] = 0x80; /* canais primario/secundario habilitados */
        m->ide_pci->cfg[0x43] = 0x80;
        st_le(m->ide_pci->cfg + 4, 5, 2); /* E/S + bus master */
        pci_set_bar(m->ide_pci, 4, 16, true, &ide_bmdma_ops, m->ide, IDE_BM_BASE);
        pci_finalize(m->ide_pci);
        space_add_io(&vm->io, 0x1f0, 8, &ide_cmd_ops, ide_bus_opaque(m->ide, 0), "ide0");
        space_add_io(&vm->io, 0x3f6, 1, &ide_ctl_ops, ide_bus_opaque(m->ide, 0), "ide0-ctl");
        space_add_io(&vm->io, 0x170, 8, &ide_cmd_ops, ide_bus_opaque(m->ide, 1), "ide1");
        space_add_io(&vm->io, 0x376, 1, &ide_ctl_ops, ide_bus_opaque(m->ide, 1), "ide1-ctl");
    }

    int slot = 2;
    if (m->bios_mode) { /* VGA em 00:02.0, como no QEMU */
        m->vgadev = vga_new(vm, VGA_VRAM_MB);
        m->vga_pci = pci_add(m->pci, slot++, 0x1234, 0x1111, 0x03000002);
        st_le(m->vga_pci->cfg + 0x2c, 0x1af4, 2);
        st_le(m->vga_pci->cfg + 0x2e, 0x1100, 2);
        st_le(m->vga_pci->cfg + 4, 3, 2);
        pci_set_ram_bar(m->vga_pci, 0, VGA_VRAM_MB << 20, vga_vram(m->vgadev), vga_vram_gen(m->vgadev), true,
                        VGA_LFB_BASE);
        char *vpath = vm->cfg.vga_bios ? strdup(vm->cfg.vga_bios) : default_vgabios(vm->cfg.firmware);
        size_t vlen;
        char verr[256];
        uint8_t *vrom = load_file(vpath, &vlen, verr, sizeof(verr));
        if (vrom) {
            pci_set_rom(m->vga_pci, vrom, vlen);
            free(vrom);
        } else {
            LOGW("VGA sem BIOS de video: %s", verr);
        }
        free(vpath);
        pci_finalize(m->vga_pci);
        space_add_io(&vm->io, 0x3b0, 0x30, &vga_io_ops, m->vgadev, "vga");
        space_add_io(&vm->io, 0x1ce, 2, &vga_vbe_ops, m->vgadev, "vbe");
        /* a tela passa a ser o framebuffer do VGA (tamanho muda com o modo) */
        free(vm->fb);
        vm->fb_size = (uint64_t)DISPLAY_MAX_W * DISPLAY_MAX_H * 4;
        vm->fb = calloc(1, vm->fb_size);
        vm->fb_w = 720;
        vm->fb_h = 400;
        vm->fb_stride = 720 * 4;
    }

    /* controlador AHCI (SATA), quando algum disco pede ou o IDE (4 vagas) nao comporta todos */
    bool need_ahci = false;
    int n_ide = 0;
    for (int i = 0; i < MVM_MAX_DISKS; i++) {
        if (!has_drive(vm, i))
            continue;
        mvm_disk_type t = vm->cfg.disks[i].type;
        if (t == MVM_DISK_SATA || t == MVM_DISK_SATA_CDROM)
            need_ahci = true;
        if (t == MVM_DISK_IDE || t == MVM_DISK_CDROM || (t == MVM_DISK_AUTO && m->bios_mode))
            n_ide++;
    }
    if (n_ide > 4)
        need_ahci = true;
    if (need_ahci) {
        int irq;
        m->ahci = ahci_new(vm, pci_line(m, slot, 1, &irq));
        m->ahci_pci = pci_add(m->pci, slot++, 0x8086, 0x2922, 0x01060102); /* ICH9 AHCI */
        st_le(m->ahci_pci->cfg + 0x2c, 0x1af4, 2);
        st_le(m->ahci_pci->cfg + 0x2e, 0x1100, 2);
        m->ahci_pci->cfg[0x3c] = (uint8_t)irq;
        m->ahci_pci->cfg[0x3d] = 1;
        if (!m->bios_mode)
            st_le(m->ahci_pci->cfg + 4, 6, 2); /* memoria + bus master */
        pci_set_bar(m->ahci_pci, 5, AHCI_ABAR_SIZE, false, &ahci_mmio_ops, m->ahci, AHCI_ABAR_BASE);
        pci_finalize(m->ahci_pci);
    }

    if (vm->ram_size > 0xa0000) {
        space_add_ram(&vm->mem, 0, 0xa0000, vm->ram, false, "ram-baixa");
        if (m->vgadev) {
            space_add_io(&vm->mem, 0xa0000, 0x20000, &vga_mem_ops, m->vgadev, "vga-janela");
        } else {
            m->vga = calloc(1, 0x20000);
            space_add_io(&vm->mem, 0xa0000, 0x20000, &vga_ops, m->vga, "vga");
        }
        space_add_ram(&vm->mem, 0xc0000, vm->ram_size - 0xc0000, vm->ram + 0xc0000, false, "ram");
    }

    int ide_next = 0;
    for (int i = 0; i < MVM_MAX_DISKS; i++) {
        if (!has_drive(vm, i))
            continue;
        mvm_disk_type t = vm->cfg.disks[i].type;
        if (t == MVM_DISK_AUTO)
            t = m->bios_mode ? MVM_DISK_IDE : MVM_DISK_VIRTIO;
        if (t == MVM_DISK_FLOPPY) {
            int drive = m->fd_cmos[0] ? 1 : 0;
            if (m->fd_cmos[drive] || !fdc_attach(m->fdc, drive, vm->disks[i])) {
                snprintf(err, errlen, "disquetes demais (maximo 2)");
                return -1;
            }
            /* drive vazio: 1,44 MB */
            m->fd_cmos[drive] = (uint8_t)fdc_cmos_type(vm->disks[i] ? vm->disks[i]->size : 1474560);
            vm_media_add(vm, MVM_MEDIA_FLOPPY, MVM_BUS_FDC, drive, i);
            continue;
        }
        if ((t == MVM_DISK_IDE || t == MVM_DISK_CDROM) && ide_next >= 4) {
            /* IDE cheio: o excedente vai para o AHCI (guests sem driver AHCI nao o veem) */
            LOGW("IDE cheio (4 dispositivos): disco %d vai para o SATA/AHCI", i);
            t = t == MVM_DISK_CDROM ? MVM_DISK_SATA_CDROM : MVM_DISK_SATA;
        }
        if (t == MVM_DISK_SATA || t == MVM_DISK_SATA_CDROM) {
            int port = m->sata_disks + m->sata_cds;
            if (port >= AHCI_PORTS || !ahci_attach(m->ahci, port, vm->disks[i], t == MVM_DISK_SATA_CDROM)) {
                snprintf(err, errlen, "discos SATA demais (maximo %d)", AHCI_PORTS);
                return -1;
            }
            if (t == MVM_DISK_SATA_CDROM) m->sata_cds++;
            else m->sata_disks++;
            vm_media_add(vm, t == MVM_DISK_SATA_CDROM ? MVM_MEDIA_CD : MVM_MEDIA_HDD, MVM_BUS_SATA, port, i);
            continue;
        }
        if (t == MVM_DISK_IDE || t == MVM_DISK_CDROM) {
            /* ordem: primario mestre/escravo, secundario mestre/escravo */
            while (ide_next < 4 && !ide_attach(m->ide, ide_next / 2, ide_next % 2, vm->disks[i], t == MVM_DISK_CDROM))
                ide_next++;
            if (ide_next >= 4) {
                snprintf(err, errlen, "sem vaga no IDE");
                return -1;
            }
            vm_media_add(vm, t == MVM_DISK_CDROM ? MVM_MEDIA_CD : MVM_MEDIA_HDD, MVM_BUS_IDE, ide_next, i);
            ide_next++;
            if (t == MVM_DISK_CDROM) m->ide_cds++;
            else m->ide_disks++;
            continue;
        }
        if (!vm->disks[i])
            continue; /* virtio sem midia nao existe */
        virtio_dev *d = virtio_blk_new(vm, vm->disks[i]);
        int irq;
        d->irq = pci_line(m, slot, 1, &irq);
        m->vdev[m->nvdev] = d;
        m->vpci[m->nvdev] = virtio_pci_add(m->pci, slot++, d, irq, (uint16_t)(VIRTIO_IO_BASE + 0x40 * m->nvdev));
        m->nvdev++;
        vm_media_add(vm, MVM_MEDIA_HDD, MVM_BUS_VIRTIO, m->nvdev - 1, i);
    }
    /* placa de rede */
    if (vm->net) {
        int irq;
        pci_dev *p = NULL;
        switch (vm->cfg.net.model) {
        case MVM_NIC_RTL8139:
            m->rtl = rtl8139_new(vm, vm->net, pci_line(m, slot, 1, &irq));
            /* revisao 0x10: o Linux usa o 8139too (com 0x20+ seria o 8139cp, modo C+) */
            p = pci_add(m->pci, slot++, 0x10ec, 0x8139, 0x02000010);
            pci_set_bar(p, 0, 0x100, true, &rtl8139_ops, m->rtl, RTL8139_IO);
            pci_set_bar(p, 1, 0x100, false, &rtl8139_ops, m->rtl, RTL8139_MMIO);
            break;
        case MVM_NIC_E1000:
            m->e1k = e1000_new(vm, vm->net, pci_line(m, slot, 1, &irq));
            p = pci_add(m->pci, slot++, 0x8086, 0x100e, 0x02000003);
            pci_set_bar(p, 0, 0x20000, false, &e1000_mmio_ops, m->e1k, E1000_MMIO);
            pci_set_bar(p, 1, 0x40, true, &e1000_io_ops, m->e1k, E1000_IO);
            break;
        default: {
            virtio_dev *d = virtio_net_new(vm, vm->net);
            d->irq = pci_line(m, slot, 1, &irq);
            m->vdev[m->nvdev] = d;
            m->vpci[m->nvdev] = virtio_pci_add(m->pci, slot++, d, irq, VIRTIO_NET_IO);
            m->nvdev++;
            break;
        }
        }
        if (p) {
            st_le(p->cfg + 0x2c, 0x1af4, 2);
            st_le(p->cfg + 0x2e, 0x1100, 2);
            p->cfg[0x3c] = (uint8_t)irq;
            p->cfg[0x3d] = 1;
            if (!m->bios_mode)
                st_le(p->cfg + 4, 7, 2); /* E/S + memoria + bus master */
            pci_finalize(p);
        }
    }

    /* placa de som */
    if (vm->audio) {
        int irq;
        pci_dev *p;
        if (vm->cfg.audio.model == MVM_SND_HDA) {
            m->hda = hda_new(vm, pci_line(m, slot, 1, &irq));
            p = pci_add(m->pci, slot++, 0x8086, 0x2668, 0x04030001);
            pci_set_bar(p, 0, 0x4000, false, &hda_ops, m->hda, HDA_MMIO);
        } else {
            m->ac97 = ac97_new(vm, pci_line(m, slot, 1, &irq));
            p = pci_add(m->pci, slot++, 0x8086, 0x2415, 0x04010001);
            pci_set_bar(p, 0, 0x100, true, &ac97_nam_ops, m->ac97, AC97_NAM_IO);
            pci_set_bar(p, 1, 0x40, true, &ac97_nabm_ops, m->ac97, AC97_NABM_IO);
        }
        st_le(p->cfg + 0x2c, 0x1af4, 2);
        st_le(p->cfg + 0x2e, 0x1100, 2);
        p->cfg[0x3c] = (uint8_t)irq;
        p->cfg[0x3d] = 1;
        if (!m->bios_mode)
            st_le(p->cfg + 4, 7, 2);
        pci_finalize(p);
    }

    /* IRQs PCI sao sensiveis a nivel */
    if (!m->bios_mode)
        i8259_elcr_ops.write(m->pic, 1, 0x0c, 1);

    if (m->bios_mode) {
        m->fw = fw_cfg_new(vm);
        space_add_io(&vm->io, 0x510, 12, &fw_cfg_ops, m->fw, "fw_cfg");
        setup_fw_cfg(vm, m);
    } else if (vm->fb) {
        mvm_region *r = space_add_ram(&vm->mem, PC_FB_BASE, vm->fb_size, vm->fb, false, "framebuffer");
        if (r)
            r->dirty_gen = &vm->fb_gen;
    }
    return load_boot(vm, m, err, errlen);
}

static void pc_reset(mvm_vm *vm)
{
    pc_machine *m = vm->mach;
    char err[256];
    i8259_reset(m->pic);
    lapic_reset(m->lapic);
    ioapic_reset(m->ioapic);
    cmos_reset(m->rtc);
    acpi_pm_reset(m->pm);
    hpet_reset(m->hpet);
    if (m->ahci)
        ahci_reset(m->ahci);
    i8237_reset(m->dma);
    if (m->rtl)
        rtl8139_reset(m->rtl);
    if (m->e1k)
        e1000_reset(m->e1k);
    if (m->ac97)
        ac97_reset(m->ac97);
    if (m->hda)
        hda_reset(m->hda);
    if (m->fdc)
        fdc_reset(m->fdc);
    memset(m->irq_level, 0, sizeof(m->irq_level));
    if (!m->bios_mode)
        i8259_elcr_ops.write(m->pic, 1, 0x0c, 1);
    i8254_reset(m->pit);
    for (int i = 0; i < m->nvdev; i++)
        virtio_reset(m->vdev[i]);
    for (int i = 0; i < m->pci->ndev; i++)
        pci_reset_dev(m->pci->dev[i]);
    if (m->bios_mode)
        for (int i = 0; i < 4; i++)
            m->piix3->cfg[0x60 + i] = 0x80;
    pirq_update(m);
    if (m->ide)
        ide_reset(m->ide);
    if (m->vgadev)
        vga_reset(m->vgadev);
    if (load_boot(vm, m, err, sizeof(err)) < 0)
        vm_fatal(vm, "falha ao reiniciar: %s", err);
}

static void pc_poll(mvm_vm *vm)
{
    pc_machine *m = vm->mach;
    uart16550_poll(m->com1);
    net_poll(vm->net);
    static int dbg = -1;
    static int64_t last;
    if (dbg < 0) {
        const char *e = getenv("MVM_PC_DEBUG"); /* MVM_PC_DEBUG=N: estado a cada N s (o 1o apos N s) */
        dbg = e ? (atoi(e) > 0 ? atoi(e) : 5) : 0;
    }
    if (dbg) {
        extern void x86_debug_dump(void *cpu);
        extern int64_t host_clock_ns(void);
        int64_t now = host_clock_ns();
        if (!last)
            last = now;
        if (now - last > dbg * 1000000000LL) {
            last = now;
            x86_debug_dump(vm->cpu);
            i8259_debug_dump(m->pic);
            lapic_debug_dump(m->lapic);
            i8254_debug_dump(m->pit);
            uart16550_debug_dump(m->com1);
            if (m->ide)
                ide_debug_dump(m->ide);
            if (m->vgadev)
                vga_debug_dump(m->vgadev);
        }
    }
}

static size_t pc_text_screen(mvm_vm *vm, char *buf, size_t len)
{
    pc_machine *m = vm->mach;
    return m->vgadev ? vga_text(m->vgadev, buf, len) : 0;
}

static void pc_input(mvm_vm *vm, const mvm_input_event *ev)
{
    pc_machine *m = vm->mach;
    if (ev->type == 0)
        i8042_key(m->kbd, ev->code, ev->pressed);
    else if (ev->type == 2)
        acpi_pm_power_button(m->pm);
    else
        i8042_mouse(m->kbd, ev->dx, ev->dy, ev->dz, ev->buttons);
}

static void pc_destroy(mvm_vm *vm)
{
    pc_machine *m = vm->mach;
    if (!m)
        return;
    for (int i = 0; i < m->nvdev; i++) {
        virtio_pci_free(m->vpci[i]);
        virtio_dev_free(m->vdev[i]);
    }
    pci_bus_free(m->pci);
    if (m->pit && vm->timers)
        i8254_reset(m->pit);
    if (m->rtc)
        cmos_reset(m->rtc);
    ide_free(m->ide);
    lapic_free(m->lapic);
    acpi_pm_free(m->pm);
    hpet_free(m->hpet);
    ahci_free(m->ahci);
    fdc_free(m->fdc);
    rtl8139_free(m->rtl);
    e1000_free(m->e1k);
    ac97_free(m->ac97);
    hda_free(m->hda);
    free(m->dma);
    ioapic_free(m->ioapic);
    vga_free(m->vgadev);
    fw_cfg_free(m->fw);
    free(m->pic);
    free(m->pit);
    free(m->rtc);
    free(m->com1);
    free(m->kbd);
    free(m->vga);
    free(m->bios);
    free(m);
    vm->mach = NULL;
}

/* ------------------------------------------------------------ midias com a VM ligada */

static bool pc_media_change(mvm_vm *vm, int slot, mvm_blk *blk, char *err, size_t errlen)
{
    pc_machine *m = vm->mach;
    vm_media *md = &vm->media[slot];
    switch (md->bus) {
    case MVM_BUS_IDE:
        if (md->kind != MVM_MEDIA_CD) {
            snprintf(err, errlen, "o controlador IDE nao aceita trocar disco rigido com a VM ligada");
            return false;
        }
        return ide_change_media(m->ide, md->unit / 2, md->unit % 2, blk);
    case MVM_BUS_SATA:
        if (md->kind == MVM_MEDIA_CD)
            return ahci_change_media(m->ahci, md->unit, blk);
        return blk ? ahci_hotplug(m->ahci, md->unit, blk) : ahci_unplug(m->ahci, md->unit);
    case MVM_BUS_FDC:
        return fdc_change_media(m->fdc, md->unit, blk);
    default:
        snprintf(err, errlen, "disco virtio nao pode ser trocado com a VM ligada");
        return false;
    }
}

static int pc_media_hotplug(mvm_vm *vm, mvm_blk *blk, char *err, size_t errlen)
{
    pc_machine *m = vm->mach;
    if (!m->ahci) {
        snprintf(err, errlen, "a VM nao tem controlador SATA: o disco so entra no proximo boot");
        return -1;
    }
    int port = ahci_free_port(m->ahci);
    if (port < 0) {
        snprintf(err, errlen, "todas as portas SATA estao ocupadas");
        return -1;
    }
    int slot = vm_media_add(vm, MVM_MEDIA_HDD, MVM_BUS_SATA, port, -1);
    if (slot < 0 || !ahci_hotplug(m->ahci, port, blk)) {
        snprintf(err, errlen, "nao consegui conectar o disco");
        return -1;
    }
    return slot;
}

const mvm_machine_ops machine_pc_ops = {
    .name = "pc",
    .init = pc_init,
    .reset = pc_reset,
    .poll = pc_poll,
    .input = pc_input,
    .text_screen = pc_text_screen,
    .destroy = pc_destroy,
    .media_change = pc_media_change,
    .media_hotplug = pc_media_hotplug,
};
