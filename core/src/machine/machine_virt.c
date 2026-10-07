/*
 * Maquina ARM "virt" (ARM e ARM64), com mapa de memoria no estilo do QEMU:
 *   0x08000000 GICv2 (distribuidor) / 0x08010000 (interface de CPU)
 *   0x09000000 PL011   0x09010000 PL031
 *   0x0a000000 virtio-mmio (0x200 por slot)
 *   0x10000000 framebuffer simples
 *   0x40000000 RAM
 */
#include "../cpu/arm_common.h"
#include "../dev/devices.h"

#include <stdlib.h>

#define VIRT_GICD 0x08000000ULL
#define VIRT_GICC 0x08010000ULL
#define VIRT_UART 0x09000000ULL
#define VIRT_RTC 0x09010000ULL
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

typedef struct {
    bool is64;
    cpu_ref ref;
    gicv2 *gic;
    pl011 *uart;
    pl031 *rtc;
    virtio_dev *vdev[MAX_VIRTIO];
    void *vmmio[MAX_VIRTIO];
    int nvdev;
    uint64_t entry;
    uint64_t dtb_addr;
    uint64_t initrd_start, initrd_end;
} virt_machine;


static void timer_irq(void *opaque, int which, int level)
{
    virt_machine *m = opaque;
    /* PPI 30 = timer fisico nao seguro, 27 = timer virtual */
    gicv2_set_irq(m->gic, which == GT_PHYS ? 30 : 27, level);
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

    fdt_node(&f, "cpus");
    fdt_prop_u32(&f, "#address-cells", 1);
    fdt_prop_u32(&f, "#size-cells", 0);
    fdt_node(&f, "cpu@0");
    fdt_prop_str(&f, "device_type", "cpu");
    fdt_prop_str(&f, "compatible", m->is64 ? "arm,cortex-a53" : "arm,cortex-a15");
    fdt_prop_u32(&f, "reg", 0);
    fdt_end_node(&f);
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
    uint32_t tirq[12] = {1, 13, 0x104, 1, 14, 0x104, 1, 11, 0x104, 1, 10, 0x104};
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

    if (vm->fb) {
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

static int load_boot(mvm_vm *vm, virt_machine *m, char *err, size_t errlen)
{
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

    arm_hooks hooks = {.opaque = m, .timer_irq = timer_irq};
    if (m->is64) {
        vm->cpu = arm64_cpu_new(vm, &hooks);
        vm->cpu_ops = &arm64_cpu_ops;
    } else {
        vm->cpu = arm32_cpu_new(vm, &hooks);
        vm->cpu_ops = &arm32_cpu_ops;
    }
    m->ref.is64 = m->is64;
    m->ref.cpu = vm->cpu;
    m->gic = gicv2_new(vm, cpu_irq, &m->ref);

    space_add_ram(&vm->mem, VIRT_RAM, vm->ram_size, vm->ram, false, "ram");
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
    for (int i = 0; i < MVM_MAX_DISKS && m->nvdev < MAX_VIRTIO; i++) {
        if (!vm->disks[i])
            continue;
        virtio_dev *d = virtio_blk_new(vm, vm->disks[i]);
        int slot = m->nvdev++;
        d->irq = (irq_line){gicv2_set_irq, m->gic, 32 + IRQ_VIRTIO + slot};
        m->vdev[slot] = d;
        m->vmmio[slot] = virtio_mmio_wrap(d);
        space_add_io(&vm->mem, VIRT_VIRTIO + VIRT_VIRTIO_SIZE * (uint64_t)slot, VIRT_VIRTIO_SIZE,
                     &virtio_mmio_ops, m->vmmio[slot], "virtio-mmio");
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

    if (vm->fb) {
        mvm_region *r = space_add_ram(&vm->mem, VIRT_FB, vm->fb_size, vm->fb, false, "framebuffer");
        if (r)
            r->dirty_gen = &vm->fb_gen;
    }

    return load_boot(vm, m, err, errlen);
}

static void virt_reset(mvm_vm *vm)
{
    virt_machine *m = vm->mach;
    char err[256];
    gicv2_reset(m->gic);
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

static void virt_destroy(mvm_vm *vm)
{
    virt_machine *m = vm->mach;
    if (!m)
        return;
    for (int i = 0; i < m->nvdev; i++) {
        virtio_mmio_unwrap(m->vmmio[i]);
        virtio_dev_free(m->vdev[i]);
    }
    free(m->gic);
    free(m->uart);
    free(m->rtc);
    free(m);
    vm->mach = NULL;
}

const mvm_machine_ops machine_virt_ops = {
    .name = "virt",
    .init = virt_init,
    .reset = virt_reset,
    .poll = virt_poll,
    .input = NULL,
    .destroy = virt_destroy,
};
