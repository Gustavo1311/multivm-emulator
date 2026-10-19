/*
 * Maquina ARM "virt" (ARM e ARM64), com mapa de memoria no estilo do QEMU:
 *   0x00000000 firmware UEFI (flash 0, somente leitura)   0x04000000 variaveis UEFI (flash 1)
 *   0x08000000 GICv2 (distribuidor) / 0x08010000 (interface de CPU)
 *   0x09000000 PL011   0x09010000 PL031   0x09020000 fw_cfg (ARM64 com UEFI: tabelas ACPI)
 *   0x0a000000 virtio-mmio (0x200 por slot)
 *   0x10000000 framebuffer simples
 *   0x40000000 RAM
 */
#include "../cpu/arm_common.h"
#include "../dev/devices.h"
#include "../dev/pflash.h"
#include "../dev/acpi.h"

#include <stdlib.h>

#define VIRT_FLASH0 0x00000000ULL
#define VIRT_FLASH1 0x04000000ULL
#define VIRT_FLASH_SIZE 0x04000000ULL /* cada banco: 64 MiB, como no QEMU */
#define VIRT_FLASH_SECTOR 0x40000     /* bloco de 256 KiB (QEMU_NOR_BLOCK_SIZE) */
#define VIRT_VARS_SIZE 0xc0000        /* QEMU_VARS.fd: variaveis + FTW */
#define VIRT_GICD 0x08000000ULL
#define VIRT_GICC 0x08010000ULL
#define VIRT_UART 0x09000000ULL
#define VIRT_RTC 0x09010000ULL
#define VIRT_FW_CFG 0x09020000ULL
#define VIRT_VIRTIO 0x0a000000ULL
#define VIRT_VIRTIO_SIZE 0x200ULL
#define VIRT_FB 0x10000000ULL
#define VIRT_RAM 0x40000000ULL

#define IRQ_UART 1
#define IRQ_RTC 2
#define IRQ_VIRTIO 16
#define MAX_VIRTIO 16 /* discos, CDs, rede e som */

typedef struct {
    bool is64;
    void *cpu;
} cpu_ref;

typedef struct virt_machine virt_machine;
typedef struct {
    virt_machine *m;
    int idx;
} timer_ref;

struct virt_machine {
    bool is64;
    int ncpus;
    cpu_ref refs[MVM_MAX_CPUS];
    timer_ref trefs[MVM_MAX_CPUS];
    gicv2 *gic;
    pl011 *uart;
    pl031 *rtc;
    virtio_dev *vdev[MAX_VIRTIO];
    void *vmmio[MAX_VIRTIO];
    int nvdev;
    uint64_t entry;
    uint64_t dtb_addr;
    uint64_t initrd_start, initrd_end;
    bool uefi;          /* firmware UEFI (edk2 ArmVirtQemu) em vez de kernel direto */
    uint8_t *fw_img;    /* imagem do firmware (QEMU_EFI.fd), mapeada em 0 */
    size_t fw_size;
    pflash *vars;       /* variaveis UEFI (flash 1) */
    virtio_dev *kbd, *mouse; /* virtio-input: a placa nao tem PS/2 */
    fw_cfg *fw;              /* so com UEFI */
    acpi_tables acpi;        /* ARM64 com UEFI */
    virtio_dev *gpu;         /* tela com UEFI (virtio-gpu) */
    virtio_dev *scsi;        /* virtio-scsi: CDs e discos conectados com a VM ligada */
};

static void timer_irq(void *opaque, int which, int level)
{
    timer_ref *t = opaque;
    /* PPI 30 = timer fisico nao seguro, 27 = timer virtual (de cada CPU) */
    if (t->m->gic)
        gicv2_set_ppi(t->m->gic, t->idx, which == GT_PHYS ? 30 : 27, level);
}

/* reset: as CPUs extras desligam (o sistema as liga de novo pelo PSCI CPU_ON). Pedido na thread
 * de cada uma; a marca de desligada vale ja, e ela sai da fatia em seguida. */
static void set_powered(mvm_vm *vm, void *cpu, bool on)
{
    virt_machine *m = vm->mach;
    if (m->is64)
        arm64_set_powered(cpu, on);
    else
        arm32_set_powered(cpu, on);
}

static void secondary_reset_work(mvm_vm *vm, void *arg)
{
    (void)arg;
    int i = vm_cpu_index(vm);
    if (i <= 0)
        return;
    vm->cpu_ops->reset(vm->vcpu[i].cpu);
    set_powered(vm, vm->vcpu[i].cpu, false);
}

static void reset_secondaries(mvm_vm *vm)
{
    for (int i = 1; i < vm->ncpus; i++) {
        set_powered(vm, vm->vcpu[i].cpu, false);
        if (atomic_load(&vm->running)) {
            vm_run_on_cpu(vm, i, secondary_reset_work, NULL);
        } else {
            vm->cpu_ops->reset(vm->vcpu[i].cpu);
            set_powered(vm, vm->vcpu[i].cpu, false);
        }
    }
}

static void cpu_irq(void *opaque, int line, int level)
{
    cpu_ref *r = opaque;
    if (r->is64)
        arm64_set_irq(r->cpu, line, level);
    else
        arm32_set_irq(r->cpu, line, level);
}

/* ------------------------------------------------------------ DTB */

static uint8_t *build_dtb(mvm_vm *vm, virt_machine *m, size_t *len)
{
    fdt_builder f;
    fdt_begin(&f);
    fdt_node(&f, "");
    fdt_prop_u32(&f, "#address-cells", 2);
    fdt_prop_u32(&f, "#size-cells", 2);
    fdt_prop_str(&f, "compatible", "linux,dummy-virt");
    fdt_prop_str(&f, "model", m->is64 ? "MultiVM ARM64 virt" : "MultiVM ARM virt");
    fdt_prop_u32(&f, "interrupt-parent", 1);

    fdt_node(&f, "chosen");
    fdt_prop_str(&f, "bootargs", vm->cfg.cmdline ? vm->cfg.cmdline : "");
    fdt_prop_str(&f, "stdout-path", "/pl011@9000000");
    if (m->initrd_end > m->initrd_start) {
        fdt_prop_u64(&f, "linux,initrd-start", m->initrd_start);
        fdt_prop_u64(&f, "linux,initrd-end", m->initrd_end);
    }
    fdt_end_node(&f);

    char name[64];
    snprintf(name, sizeof(name), "memory@%llx", (unsigned long long)VIRT_RAM);
    fdt_node(&f, name);
    fdt_prop_str(&f, "device_type", "memory");
    uint32_t mem[4] = {0, (uint32_t)VIRT_RAM, (uint32_t)(vm->ram_size >> 32), (uint32_t)vm->ram_size};
    fdt_prop_cells(&f, "reg", mem, 4);
    fdt_end_node(&f);

    if (m->uefi) {
        /* os dois bancos de flash CFI; o edk2 usa o que nao contem o codigo para as variaveis e
         * depois marca o no como "disabled" para o sistema operacional */
        fdt_node(&f, "flash@0");
        fdt_prop_str(&f, "compatible", "cfi-flash");
        uint32_t freg[8] = {0, (uint32_t)VIRT_FLASH0, 0, (uint32_t)VIRT_FLASH_SIZE,
                            0, (uint32_t)VIRT_FLASH1, 0, pflash_size(m->vars)};
        fdt_prop_cells(&f, "reg", freg, 8);
        fdt_prop_u32(&f, "bank-width", 4);
        fdt_end_node(&f);
    }

    fdt_node(&f, "cpus");
    fdt_prop_u32(&f, "#address-cells", 1);
    fdt_prop_u32(&f, "#size-cells", 0);
    for (int i = 0; i < m->ncpus; i++) {
        char cn[16];
        snprintf(cn, sizeof(cn), "cpu@%d", i);
        fdt_node(&f, cn);
        fdt_prop_str(&f, "device_type", "cpu");
        fdt_prop_str(&f, "compatible", m->is64 ? "arm,cortex-a53" : "arm,cortex-a15");
        fdt_prop_u32(&f, "reg", (uint32_t)i);
        if (m->ncpus > 1)
            fdt_prop_str(&f, "enable-method", "psci"); /* as extras ligam pelo PSCI CPU_ON */
        fdt_end_node(&f);
    }
    fdt_end_node(&f);

    fdt_node(&f, "psci");
    const char *psci_compat[] = {"arm,psci-1.0", "arm,psci-0.2", "arm,psci"};
    fdt_prop_strs(&f, "compatible", psci_compat, 3);
    fdt_prop_str(&f, "method", "hvc");
    fdt_prop_u32(&f, "cpu_suspend", 0xc4000001);
    fdt_prop_u32(&f, "cpu_off", 0x84000002);
    fdt_prop_u32(&f, "cpu_on", 0xc4000003);
    fdt_prop_u32(&f, "migrate", 0xc4000005);
    fdt_end_node(&f);

    fdt_node(&f, "timer");
    fdt_prop_str(&f, "compatible", m->is64 ? "arm,armv8-timer" : "arm,armv7-timer");
    uint32_t tf = (((1u << m->ncpus) - 1) << 8) | 4; /* PPI de todas as CPUs, nivel alto */
    uint32_t tirq[12] = {1, 13, tf, 1, 14, tf, 1, 11, tf, 1, 10, tf};
    fdt_prop_cells(&f, "interrupts", tirq, 12);
    fdt_prop_empty(&f, "always-on");
    fdt_end_node(&f);

    fdt_node(&f, "apb-pclk");
    fdt_prop_str(&f, "compatible", "fixed-clock");
    fdt_prop_u32(&f, "#clock-cells", 0);
    fdt_prop_u32(&f, "clock-frequency", 24000000);
    fdt_prop_str(&f, "clock-output-names", "clk24mhz");
    fdt_prop_u32(&f, "phandle", 2);
    fdt_end_node(&f);

    fdt_node(&f, "intc@8000000");
    fdt_prop_str(&f, "compatible", "arm,cortex-a15-gic");
    fdt_prop_u32(&f, "#interrupt-cells", 3);
    fdt_prop_empty(&f, "interrupt-controller");
    uint32_t gicreg[8] = {0, (uint32_t)VIRT_GICD, 0, 0x10000, 0, (uint32_t)VIRT_GICC, 0, 0x2000};
    fdt_prop_cells(&f, "reg", gicreg, 8);
    fdt_prop_u32(&f, "phandle", 1);
    fdt_end_node(&f);

    fdt_node(&f, "pl011@9000000");
    const char *uart_compat[] = {"arm,pl011", "arm,primecell"};
    fdt_prop_strs(&f, "compatible", uart_compat, 2);
    uint32_t ureg[4] = {0, (uint32_t)VIRT_UART, 0, 0x1000};
    fdt_prop_cells(&f, "reg", ureg, 4);
    uint32_t uirq[3] = {0, IRQ_UART, 4};
    fdt_prop_cells(&f, "interrupts", uirq, 3);
    uint32_t clk2[2] = {2, 2};
    fdt_prop_cells(&f, "clocks", clk2, 2);
    const char *clkn[] = {"uartclk", "apb_pclk"};
    fdt_prop_strs(&f, "clock-names", clkn, 2);
    fdt_end_node(&f);

    fdt_node(&f, "pl031@9010000");
    const char *rtc_compat[] = {"arm,pl031", "arm,primecell"};
    fdt_prop_strs(&f, "compatible", rtc_compat, 2);
    uint32_t rreg[4] = {0, (uint32_t)VIRT_RTC, 0, 0x1000};
    fdt_prop_cells(&f, "reg", rreg, 4);
    uint32_t rirq[3] = {0, IRQ_RTC, 4};
    fdt_prop_cells(&f, "interrupts", rirq, 3);
    fdt_prop_u32(&f, "clocks", 2);
    fdt_prop_str(&f, "clock-names", "apb_pclk");
    fdt_end_node(&f);

    for (int i = 0; i < m->nvdev; i++) {
        uint64_t base = VIRT_VIRTIO + VIRT_VIRTIO_SIZE * (uint64_t)i;
        snprintf(name, sizeof(name), "virtio_mmio@%llx", (unsigned long long)base);
        fdt_node(&f, name);
        fdt_prop_str(&f, "compatible", "virtio,mmio");
        uint32_t vreg[4] = {0, (uint32_t)base, 0, (uint32_t)VIRT_VIRTIO_SIZE};
        fdt_prop_cells(&f, "reg", vreg, 4);
        uint32_t virq[3] = {0, (uint32_t)(IRQ_VIRTIO + i), 1};
        fdt_prop_cells(&f, "interrupts", virq, 3);
        fdt_prop_empty(&f, "dma-coherent");
        fdt_end_node(&f);
    }

    if (m->fw) {
        snprintf(name, sizeof(name), "fw-cfg@%llx", (unsigned long long)VIRT_FW_CFG);
        fdt_node(&f, name);
        fdt_prop_str(&f, "compatible", "qemu,fw-cfg-mmio");
        uint32_t creg[4] = {0, (uint32_t)VIRT_FW_CFG, 0, 0x18};
        fdt_prop_cells(&f, "reg", creg, 4);
        fdt_prop_empty(&f, "dma-coherent");
        fdt_end_node(&f);
    }

    if (vm->fb && !m->uefi) { /* com UEFI a tela e a virtio-gpu (GOP do firmware) */
        snprintf(name, sizeof(name), "framebuffer@%llx", (unsigned long long)VIRT_FB);
        fdt_node(&f, name);
        fdt_prop_str(&f, "compatible", "simple-framebuffer");
        uint32_t freg[4] = {0, (uint32_t)VIRT_FB, 0, (uint32_t)vm->fb_size};
        fdt_prop_cells(&f, "reg", freg, 4);
        fdt_prop_u32(&f, "width", vm->fb_w);
        fdt_prop_u32(&f, "height", vm->fb_h);
        fdt_prop_u32(&f, "stride", vm->fb_stride);
        fdt_prop_str(&f, "format", "x8r8g8b8");
        fdt_end_node(&f);
    }

    fdt_end_node(&f);
    return fdt_finish(&f, len);
}

/* ------------------------------------------------------------ carga */

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

/* Descompacta kernels gzip e EFI zboot (gzip). Retorna novo buffer ou o mesmo. */
static uint8_t *unwrap_kernel(uint8_t *img, size_t *len)
{
    if (is_gzip(img, *len)) {
        size_t out;
        uint8_t *u = gunzip(img, *len, &out);
        if (u) {
            LOGI("kernel gzip descompactado: %zu -> %zu bytes", *len, out);
            free(img);
            *len = out;
            return u;
        }
    }
    if (*len > 64 && img[0] == 'M' && img[1] == 'Z' && !memcmp(img + 4, "zimg", 4)) {
        uint32_t off = (uint32_t)ld_le(img + 8, 4), sz = (uint32_t)ld_le(img + 12, 4);
        if (!memcmp(img + 0x18, "gzip", 4) && (size_t)off + sz <= *len) {
            size_t out;
            uint8_t *u = gunzip(img + off, sz, &out);
            if (u) {
                LOGI("kernel EFI zboot descompactado: %zu bytes", out);
                free(img);
                *len = out;
                return u;
            }
        }
        LOGW("kernel EFI zboot com compressao nao suportada");
    }
    return img;
}

/* UEFI: como o QEMU sem -kernel, o DTB fica no inicio da RAM (PcdDeviceTreeInitialBaseAddress)
 * e a CPU comeca no inicio da flash 0 */
static int load_firmware_boot(mvm_vm *vm, virt_machine *m, char *err, size_t errlen)
{
    memset(vm->ram, 0, vm->ram_size);
    m->initrd_start = m->initrd_end = 0;
    size_t dlen;
    uint8_t *dtb = vm->cfg.dtb ? load_file(vm->cfg.dtb, &dlen, err, errlen) : build_dtb(vm, m, &dlen);
    if (!dtb)
        return -1;
    if (dlen > vm->ram_size / 2) {
        snprintf(err, errlen, "DTB grande demais");
        free(dtb);
        return -1;
    }
    memcpy(vm->ram, dtb, dlen);
    free(dtb);
    m->entry = VIRT_FLASH0;
    m->dtb_addr = VIRT_RAM;
    vm->cpu_ops->reset(vm->cpu);
    reset_secondaries(vm);
    if (m->is64)
        arm64_set_entry(vm->cpu, m->entry, m->dtb_addr);
    else
        arm32_set_entry(vm->cpu, (uint32_t)m->entry, 0, 0xffffffffu, (uint32_t)m->dtb_addr);
    return 0;
}

static int load_boot(mvm_vm *vm, virt_machine *m, char *err, size_t errlen)
{
    if (m->uefi)
        return load_firmware_boot(vm, m, err, errlen);
    memset(vm->ram, 0, vm->ram_size);
    uint64_t kend = VIRT_RAM;
    m->entry = VIRT_RAM;
    m->initrd_start = m->initrd_end = 0;

    if (vm->cfg.kernel) {
        size_t len;
        uint8_t *img = load_file(vm->cfg.kernel, &len, err, errlen);
        if (!img)
            return -1;
        img = unwrap_kernel(img, &len);
        if (elf_probe(img, len)) {
            elf_info ei;
            if (!elf_load(&vm->mem, img, len, &ei, err, errlen)) {
                free(img);
                return -1;
            }
            m->entry = ei.entry;
            kend = ei.high;
            LOGI("ELF carregado: entrada 0x%llx", (unsigned long long)ei.entry);
        } else if (m->is64 && len > 64 && ld_le(img + 0x38, 4) == 0x644d5241) {
            uint64_t text_off = ld_le(img + 8, 8), image_size = ld_le(img + 16, 8);
            uint64_t base = VIRT_RAM + (text_off ? 0 : 0);
            uint64_t load = base + text_off;
            if (!image_size)
                image_size = len;
            if (load - VIRT_RAM + image_size > vm->ram_size) {
                snprintf(err, errlen, "kernel nao cabe na RAM");
                free(img);
                return -1;
            }
            memcpy(vm->ram + (load - VIRT_RAM), img, len);
            m->entry = load;
            kend = load + image_size;
            LOGI("Image ARM64 carregada em 0x%llx (%zu bytes)", (unsigned long long)load, len);
        } else if (!m->is64 && len > 0x30 && ld_le(img + 0x24, 4) == 0x016f2818) {
            uint64_t load = VIRT_RAM + 0x8000;
            if (0x8000 + len > vm->ram_size) {
                snprintf(err, errlen, "kernel nao cabe na RAM");
                free(img);
                return -1;
            }
            memcpy(vm->ram + 0x8000, img, len);
            m->entry = load;
            /* o zImage descompacta para ~4x o tamanho */
            kend = load + len * 4;
            LOGI("zImage ARM carregado em 0x%llx (%zu bytes)", (unsigned long long)load, len);
        } else {
            uint64_t load = vm->cfg.raw_load_addr ? vm->cfg.raw_load_addr : VIRT_RAM;
            if (load < VIRT_RAM || load - VIRT_RAM + len > vm->ram_size) {
                snprintf(err, errlen, "binario bruto fora da RAM");
                free(img);
                return -1;
            }
            memcpy(vm->ram + (load - VIRT_RAM), img, len);
            m->entry = load;
            kend = load + len;
            LOGI("binario bruto carregado em 0x%llx", (unsigned long long)load);
        }
        free(img);
    }

    /* initrd: a partir de 128 MiB (ou metade da RAM), acima do kernel */
    uint64_t next = VIRT_RAM + (vm->ram_size / 2 < (128ULL << 20) ? vm->ram_size / 2 : (128ULL << 20));
    if (next < align_up(kend, 1 << 21))
        next = align_up(kend, 1 << 21);
    if (vm->cfg.initrd) {
        size_t len;
        uint8_t *rd = load_file(vm->cfg.initrd, &len, err, errlen);
        if (!rd)
            return -1;
        if (next - VIRT_RAM + len + (2 << 20) > vm->ram_size) {
            snprintf(err, errlen, "initrd nao cabe na RAM (%zu bytes)", len);
            free(rd);
            return -1;
        }
        memcpy(vm->ram + (next - VIRT_RAM), rd, len);
        m->initrd_start = next;
        m->initrd_end = next + len;
        next = align_up(m->initrd_end, 1 << 12);
        free(rd);
        LOGI("initrd em 0x%llx-0x%llx", (unsigned long long)m->initrd_start, (unsigned long long)m->initrd_end);
    }

    size_t dlen;
    uint8_t *dtb;
    if (vm->cfg.dtb) {
        dtb = load_file(vm->cfg.dtb, &dlen, err, errlen);
        if (!dtb)
            return -1;
    } else {
        dtb = build_dtb(vm, m, &dlen);
    }
    m->dtb_addr = align_up(next, 1 << 21);
    if (m->dtb_addr - VIRT_RAM + dlen > vm->ram_size) {
        snprintf(err, errlen, "sem espaco para o DTB");
        free(dtb);
        return -1;
    }
    memcpy(vm->ram + (m->dtb_addr - VIRT_RAM), dtb, dlen);
    free(dtb);

    vm->cpu_ops->reset(vm->cpu);
    reset_secondaries(vm);
    if (m->is64)
        arm64_set_entry(vm->cpu, m->entry, m->dtb_addr);
    else
        arm32_set_entry(vm->cpu, (uint32_t)m->entry, 0, 0xffffffffu, (uint32_t)m->dtb_addr);
    return 0;
}

/* ------------------------------------------------------------ ciclo de vida */

static int virt_init(mvm_vm *vm, char *err, size_t errlen)
{
    virt_machine *m = calloc(1, sizeof(*m));
    vm->mach = m;
    m->is64 = vm->cfg.arch == MVM_ARCH_ARM64;
    vm->ram_base = VIRT_RAM;

    /* varias CPUs: a 0 liga no boot; as outras pelo PSCI CPU_ON */
    m->ncpus = vm->cfg.cpus > 1 ? (int)vm->cfg.cpus : 1;
    void *gic_cpus[MVM_MAX_CPUS];
    for (int i = 0; i < m->ncpus; i++) {
        m->trefs[i] = (timer_ref){m, i};
        arm_hooks hooks = {.opaque = &m->trefs[i], .timer_irq = timer_irq};
        void *cpu = m->is64 ? arm64_cpu_new(vm, &hooks) : arm32_cpu_new(vm, &hooks);
        int idx = vm_add_cpu(vm, cpu);
        if (m->is64) {
            arm64_set_index(cpu, idx, m->ncpus);
            if (idx > 0)
                arm64_set_powered(cpu, false);
        } else {
            arm32_set_index(cpu, idx, m->ncpus);
            if (idx > 0)
                arm32_set_powered(cpu, false);
        }
        m->refs[i] = (cpu_ref){m->is64, cpu};
        gic_cpus[i] = &m->refs[i];
    }
    vm->cpu_ops = m->is64 ? &arm64_cpu_ops : &arm32_cpu_ops;
    m->gic = gicv2_new(vm, cpu_irq, gic_cpus, m->ncpus);
    if (m->ncpus > 1)
        LOGI("virt: %d processadores", m->ncpus);

    space_add_ram(&vm->mem, VIRT_RAM, vm->ram_size, vm->ram, false, "ram");

    /* firmware UEFI (QEMU_EFI.fd / AAVMF_CODE.fd): com --bios e sem --kernel */
    if (vm->cfg.firmware && !vm->cfg.kernel) {
        m->fw_img = load_file(vm->cfg.firmware, &m->fw_size, err, errlen);
        if (!m->fw_img)
            return -1;
        if (m->fw_size < 0x10000 || m->fw_size > VIRT_FLASH_SIZE) {
            snprintf(err, errlen, "tamanho de firmware invalido (%zu)", m->fw_size);
            return -1;
        }
        /* o edk2 comeca com um desvio para 0x1000 (b 0x1000 em A64 ou A32) */
        uint32_t w0 = (uint32_t)ld_le(m->fw_img, 4);
        bool a64 = w0 == 0x14000400, a32 = w0 == 0xea0003fe;
        if ((m->is64 && a32) || (!m->is64 && a64)) {
            snprintf(err, errlen, "firmware UEFI de %s numa VM %s", a64 ? "ARM64" : "ARM 32 bits",
                     m->is64 ? "ARM64" : "ARM 32 bits");
            return -1;
        }
        size_t map = (m->fw_size + 0xfff) & ~(size_t)0xfff;
        if (map != m->fw_size) {
            uint8_t *n = realloc(m->fw_img, map);
            if (!n) {
                snprintf(err, errlen, "sem memoria para o firmware");
                return -1;
            }
            memset(n + m->fw_size, 0xff, map - m->fw_size);
            m->fw_img = n;
            m->fw_size = map;
        }
        space_add_ram(&vm->mem, VIRT_FLASH0, m->fw_size, m->fw_img, true, "uefi-code");
        if (vm->cfg.uefi_vars) {
            m->vars = pflash_open_ex(vm->cfg.uefi_vars, (uint32_t)VIRT_FLASH_SIZE, 4, VIRT_FLASH_SECTOR, err, errlen);
            if (!m->vars)
                return -1;
        } else {
            LOGW("sem arquivo de variaveis UEFI: alteracoes valem so ate fechar a VM");
            m->vars = pflash_new_blank(VIRT_VARS_SIZE, 4, VIRT_FLASH_SECTOR);
        }
        space_add_io(&vm->mem, VIRT_FLASH1, pflash_size(m->vars), &pflash_ops, m->vars, "uefi-vars");
        m->uefi = true;
        LOGI("firmware UEFI ARM: %zu KiB, variaveis %u KiB", m->fw_size >> 10, pflash_size(m->vars) >> 10);
    } else if (vm->cfg.uefi_vars) {
        snprintf(err, errlen, "variaveis UEFI so valem com firmware UEFI (--bios sem --kernel)");
        return -1;
    }
    space_add_io(&vm->mem, VIRT_GICD, 0x10000, &gicv2_dist_ops, m->gic, "gicd");
    space_add_io(&vm->mem, VIRT_GICC, 0x10000, &gicv2_cpu_ops, m->gic, "gicc");
    m->uart = pl011_new(vm, &vm->serial, (irq_line){gicv2_set_irq, m->gic, 32 + IRQ_UART});
    space_add_io(&vm->mem, VIRT_UART, 0x1000, &pl011_ops, m->uart, "pl011");
    m->rtc = pl031_new(vm, (irq_line){gicv2_set_irq, m->gic, 32 + IRQ_RTC});
    space_add_io(&vm->mem, VIRT_RTC, 0x1000, &pl031_ops, m->rtc, "pl031");

    for (int i = 0; i < MVM_MAX_DISKS; i++)
        if (vm->disks[i] && vm->cfg.disks[i].type == MVM_DISK_FLOPPY) {
            snprintf(err, errlen, "a maquina virt (ARM) nao tem controlador de disquete");
            return -1;
        }
    /* discos: virtio-blk (/dev/vda...). CDs (MVM_DISK_CDROM): virtio-scsi, que troca a midia e aceita
     * discos novos com a VM ligada. Com UEFI o controlador SCSI sempre existe, com um CD (mesmo vazio),
     * logo depois dos discos: assim a posicao da rede nao muda quando a ISO sai */
    bool want_scsi = m->uefi;
    for (int i = 0; i < MVM_MAX_DISKS; i++)
        if (vm->cfg.disks[i].type == MVM_DISK_CDROM && (vm->disks[i] || vm->cfg.disks[i].empty))
            want_scsi = true;
    for (int i = 0; i < MVM_MAX_DISKS && m->nvdev < MAX_VIRTIO; i++) {
        if (!vm->disks[i] || vm->cfg.disks[i].type == MVM_DISK_CDROM)
            continue;
        virtio_dev *d = virtio_blk_new(vm, vm->disks[i]);
        int slot = m->nvdev++;
        d->irq = (irq_line){gicv2_set_irq, m->gic, 32 + IRQ_VIRTIO + slot};
        m->vdev[slot] = d;
        m->vmmio[slot] = virtio_mmio_wrap(d);
        virtio_set_own_lock(d); /* leitura do disco sem a bql: as outras CPUs seguem */
        space_add_io(&vm->mem, VIRT_VIRTIO + VIRT_VIRTIO_SIZE * (uint64_t)slot, VIRT_VIRTIO_SIZE,
                     &virtio_mmio_ops, m->vmmio[slot], "virtio-mmio")->nolock = true;
        vm_media_add(vm, MVM_MEDIA_HDD, MVM_BUS_VIRTIO, slot, i);
    }
    if (want_scsi && m->nvdev < MAX_VIRTIO) {
        virtio_dev *d = virtio_scsi_new(vm);
        int slot = m->nvdev++;
        d->irq = (irq_line){gicv2_set_irq, m->gic, 32 + IRQ_VIRTIO + slot};
        m->vdev[slot] = d;
        m->vmmio[slot] = virtio_mmio_wrap(d);
        virtio_set_own_lock(d);
        space_add_io(&vm->mem, VIRT_VIRTIO + VIRT_VIRTIO_SIZE * (uint64_t)slot, VIRT_VIRTIO_SIZE,
                     &virtio_mmio_ops, m->vmmio[slot], "virtio-scsi")->nolock = true;
        m->scsi = d;
        int ncd = 0;
        for (int i = 0; i < MVM_MAX_DISKS; i++) {
            if (vm->cfg.disks[i].type != MVM_DISK_CDROM || (!vm->disks[i] && !vm->cfg.disks[i].empty))
                continue;
            int t = virtio_scsi_attach(d, vm->disks[i], true);
            if (t < 0)
                break;
            vm_media_add(vm, MVM_MEDIA_CD, MVM_BUS_SCSI, t, i);
            ncd++;
        }
        if (!ncd) { /* drive vazio para inserir uma ISO depois */
            int t = virtio_scsi_attach(d, NULL, true);
            if (t >= 0)
                vm_media_add(vm, MVM_MEDIA_CD, MVM_BUS_SCSI, t, -1);
        }
    }

    /* rede: virtio-net (o DT ganha o no virtio_mmio automaticamente) */
    if (vm->net && m->nvdev < MAX_VIRTIO) {
        virtio_dev *d = virtio_net_new(vm, vm->net);
        int slot = m->nvdev++;
        d->irq = (irq_line){gicv2_set_irq, m->gic, 32 + IRQ_VIRTIO + slot};
        m->vdev[slot] = d;
        m->vmmio[slot] = virtio_mmio_wrap(d);
        space_add_io(&vm->mem, VIRT_VIRTIO + VIRT_VIRTIO_SIZE * (uint64_t)slot, VIRT_VIRTIO_SIZE,
                     &virtio_mmio_ops, m->vmmio[slot], "virtio-net");
    }

    /* som: virtio-sound */
    if (vm->audio && m->nvdev < MAX_VIRTIO) {
        virtio_dev *d = virtio_snd_new(vm);
        int slot = m->nvdev++;
        d->irq = (irq_line){gicv2_set_irq, m->gic, 32 + IRQ_VIRTIO + slot};
        m->vdev[slot] = d;
        m->vmmio[slot] = virtio_mmio_wrap(d);
        space_add_io(&vm->mem, VIRT_VIRTIO + VIRT_VIRTIO_SIZE * (uint64_t)slot, VIRT_VIRTIO_SIZE,
                     &virtio_mmio_ops, m->vmmio[slot], "virtio-snd");
    }

    /* teclado e mouse (virtio-input) depois dos discos, para nao mudar /dev/vda... */
    for (int k = 0; k < 2 && m->nvdev < MAX_VIRTIO; k++) {
        virtio_dev *d = virtio_input_new(vm, k);
        int slot = m->nvdev++;
        d->irq = (irq_line){gicv2_set_irq, m->gic, 32 + IRQ_VIRTIO + slot};
        m->vdev[slot] = d;
        m->vmmio[slot] = virtio_mmio_wrap(d);
        space_add_io(&vm->mem, VIRT_VIRTIO + VIRT_VIRTIO_SIZE * (uint64_t)slot, VIRT_VIRTIO_SIZE,
                     &virtio_mmio_ops, m->vmmio[slot], k ? "virtio-mouse" : "virtio-kbd");
        if (k)
            m->mouse = d;
        else
            m->kbd = d;
    }

    if (vm->fb && !m->uefi) {
        mvm_region *r = space_add_ram(&vm->mem, VIRT_FB, vm->fb_size, vm->fb, false, "framebuffer");
        if (r)
            r->dirty_gen = &vm->fb_gen;
    }

    if (m->uefi) {
        /* tela: virtio-gpu (o edk2 tem GOP para ela e o Linux carrega o virtio_gpu sozinho) */
        uint32_t pw = vm->fb_w ? vm->fb_w : 1024, ph = vm->fb_h ? vm->fb_h : 768;
        if (pw < 640 || ph < 480) { /* o console grafico do edk2 precisa de 640x480 */
            pw = 1024;
            ph = 768;
        }
        /* o buffer cabe ate 2560x1600: o convidado pode escolher outra resolucao, e o app pode pedir
         * a da tela do celular com a VM ligada */
        uint64_t need = (uint64_t)pw * ph * 4;
        if (need < 2560ull * 1600 * 4)
            need = 2560ull * 1600 * 4;
        if (!vm->fb || vm->fb_size < need) {
            free(vm->fb);
            vm->fb_size = (need + 0xffff) & ~0xffffULL;
            vm->fb = calloc(1, vm->fb_size);
        }
        vm->fb_w = pw;
        vm->fb_h = ph;
        vm->fb_stride = pw * 4;
        if (m->nvdev < MAX_VIRTIO) {
            virtio_dev *d = virtio_gpu_new(vm, pw, ph);
            int slot = m->nvdev++;
            d->irq = (irq_line){gicv2_set_irq, m->gic, 32 + IRQ_VIRTIO + slot};
            m->vdev[slot] = d;
            m->vmmio[slot] = virtio_mmio_wrap(d);
            space_add_io(&vm->mem, VIRT_VIRTIO + VIRT_VIRTIO_SIZE * (uint64_t)slot, VIRT_VIRTIO_SIZE,
                         &virtio_mmio_ops, m->vmmio[slot], "virtio-gpu");
            m->gpu = d;
        }
        if (m->is64) {
            /* com o table-loader no fw_cfg o edk2 entrega ACPI (e nao o DT) ao sistema, como no QEMU: o SPCR
             * faz da serial o console preferido desde o inicio e o Linux nao liga o console de video tty0
             * junto (com DT, o Alpine abria dois logins no mesmo terminal e as teclas se dividiam) */
            m->fw = fw_cfg_new(vm);
            space_add_io(&vm->mem, VIRT_FW_CFG, 0x18, &fw_cfg_mmio_ops, m->fw, "fw_cfg");
            acpi_arm_config ac = {
                .gicd = VIRT_GICD, .gicc = VIRT_GICC, .uart = VIRT_UART, .uart_gsiv = 32 + IRQ_UART,
                .fw_cfg = VIRT_FW_CFG, .virtio_base = VIRT_VIRTIO, .virtio_size = (uint32_t)VIRT_VIRTIO_SIZE,
                .virtio_gsiv = 32 + IRQ_VIRTIO, .nvirtio = (uint32_t)m->nvdev, .timer_gsiv = {29, 30, 27, 26},
                .ncpus = (uint32_t)m->ncpus,
            };
            acpi_build_arm(&m->acpi, &ac);
            acpi_add_to_fw_cfg(&m->acpi, m->fw);
        }
    }

    return load_boot(vm, m, err, errlen);
}

static void virt_reset(mvm_vm *vm)
{
    virt_machine *m = vm->mach;
    char err[256];
    gicv2_reset(m->gic);
    if (m->vars)
        pflash_reset(m->vars);
    for (int i = 0; i < m->nvdev; i++)
        virtio_reset(m->vdev[i]);
    if (load_boot(vm, m, err, sizeof(err)) < 0)
        vm_fatal(vm, "falha ao recarregar: %s", err);
}

static void virt_poll(mvm_vm *vm)
{
    virt_machine *m = vm->mach;
    pl011_poll(m->uart);
    net_poll(vm->net);
}

static void virt_input(mvm_vm *vm, const mvm_input_event *ev)
{
    virt_machine *m = vm->mach;
    if (ev->type == 0) {
        if (m->kbd)
            virtio_input_key(m->kbd, ev->code, ev->pressed);
    } else if (ev->type == 1) {
        if (m->mouse)
            virtio_input_mouse(m->mouse, ev->dx, ev->dy, ev->dz, ev->buttons);
    } else if (ev->type == 3) {
        if (m->gpu)
            virtio_gpu_set_preferred(m->gpu, (uint16_t)ev->dx, (uint16_t)ev->dy);
    } else if (ev->type == 2) {
        /* botao de energia: a placa nao tem ACPI; a tecla KEY_POWER faz o Linux desligar */
        if (m->kbd) {
            virtio_input_key(m->kbd, 116, true);
            virtio_input_key(m->kbd, 116, false);
        }
    }
}

static void virt_destroy(mvm_vm *vm)
{
    virt_machine *m = vm->mach;
    if (!m)
        return;
    for (int i = 0; i < m->nvdev; i++) {
        virtio_mmio_unwrap(m->vmmio[i]);
        virtio_dev_free(m->vdev[i]);
    }
    pflash_free(m->vars);
    fw_cfg_free(m->fw);
    acpi_tables_free(&m->acpi);
    free(m->fw_img);
    free(m->gic);
    free(m->uart);
    free(m->rtc);
    free(m);
    vm->mach = NULL;
}

/* ------------------------------------------------------------ midias com a VM ligada */

static bool virt_media_change(mvm_vm *vm, int slot, mvm_blk *blk, char *err, size_t errlen)
{
    virt_machine *m = vm->mach;
    vm_media *md = &vm->media[slot];
    if (md->bus != MVM_BUS_SCSI || !m->scsi) {
        snprintf(err, errlen, "disco virtio-blk nao pode ser trocado com a VM ligada");
        return false;
    }
    if (!virtio_scsi_change(m->scsi, md->unit, blk, md->kind == MVM_MEDIA_CD)) {
        snprintf(err, errlen, "o drive SCSI %d nao aceitou a troca", md->unit);
        return false;
    }
    return true;
}

static int virt_media_hotplug(mvm_vm *vm, mvm_blk *blk, char *err, size_t errlen)
{
    virt_machine *m = vm->mach;
    if (!m->scsi) {
        snprintf(err, errlen, "a VM nao tem controlador virtio-scsi: o disco so entra no proximo boot");
        return -1;
    }
    /* reaproveita um drive de disco SCSI ja desconectado */
    for (int i = 0; i < MVM_MAX_MEDIA; i++) {
        vm_media *md = &vm->media[i];
        if (md->used && md->bus == MVM_BUS_SCSI && md->kind == MVM_MEDIA_HDD && !md->blk) {
            if (!virtio_scsi_change(m->scsi, md->unit, blk, false))
                break;
            return i;
        }
    }
    int t = virtio_scsi_free_target(m->scsi);
    if (t < 0) {
        snprintf(err, errlen, "o controlador SCSI nao tem mais vagas");
        return -1;
    }
    int slot = vm_media_add(vm, MVM_MEDIA_HDD, MVM_BUS_SCSI, t, -1);
    if (slot < 0 || !virtio_scsi_change(m->scsi, t, blk, false)) {
        snprintf(err, errlen, "nao consegui conectar o disco");
        return -1;
    }
    return slot;
}

const mvm_machine_ops machine_virt_ops = {
    .name = "virt",
    .init = virt_init,
    .reset = virt_reset,
    .poll = virt_poll,
    .input = virt_input,
    .destroy = virt_destroy,
    .media_change = virt_media_change,
    .media_hotplug = virt_media_hotplug,
};
