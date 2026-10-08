/* PCI: barramento e dispositivos. */
#ifndef MVM_PCI_H
#define MVM_PCI_H

#include "devices.h"

#define PCI_MAX_DEV 32

struct pci_bus;

typedef struct pci_dev {
    struct pci_bus *bus;
    int slot, fn;
    uint8_t cfg[256];
    uint8_t cfg_init[256];             /* configuracao apos pci_finalize (restaurada no reset) */
    uint32_t bar_size[6];
    bool bar_io[6];
    bool bar_mapped[6];
    uint64_t bar_base[6];
    const mvm_io_ops *bar_ops[6];
    void *bar_opaque[6];
    uint8_t *bar_host[6];              /* BAR de memoria respaldado por RAM (ex.: VRAM) */
    _Atomic uint32_t *bar_gen[6];      /* contador de escrita para BAR de RAM */
    uint64_t *bar_dirty[6];            /* paginas sujas do BAR de RAM (ou NULL) */
    uint32_t default_bar[6];
    uint16_t default_cmd;
    void (*cfg_write)(struct pci_dev *d, unsigned off, uint32_t val, unsigned size);
    void *priv;
    /* ROM de expansao (BAR 0x30) */
    uint8_t *rom;
    uint32_t rom_size;
    bool rom_mapped;
    uint64_t rom_base;
} pci_dev;

typedef struct pci_bus {
    mvm_vm *vm;
    uint32_t addr;
    pci_dev *dev[PCI_MAX_DEV];
    int ndev;
} pci_bus;

extern const mvm_io_ops pci_host_ops; /* portas 0xCF8-0xCFF */

pci_bus *pci_bus_new(mvm_vm *vm);
void pci_bus_free(pci_bus *b);
pci_dev *pci_add(pci_bus *b, int slot, uint16_t vendor, uint16_t device, uint32_t class_rev);
pci_dev *pci_add_fn(pci_bus *b, int slot, int fn, uint16_t vendor, uint16_t device, uint32_t class_rev);
/* ROM de expansao: os dados sao copiados; o tamanho e arredondado para potencia de 2 */
void pci_set_rom(pci_dev *d, const uint8_t *data, size_t len);
void pci_set_bar(pci_dev *d, int i, uint32_t size, bool io, const mvm_io_ops *ops, void *opaque, uint32_t addr);
void pci_set_ram_bar(pci_dev *d, int i, uint32_t size, uint8_t *host, _Atomic uint32_t *gen, uint64_t *dirty,
                     bool prefetch, uint32_t addr);
void pci_finalize(pci_dev *d);
void pci_reset_dev(pci_dev *d);

pci_dev *virtio_pci_add(pci_bus *b, int slot, virtio_dev *v, int irq, uint16_t io_base);
void virtio_pci_free(pci_dev *d);

#endif
