/* ACPI: tabelas e bloco de gerenciamento de energia da maquina "pc". */
#ifndef MVM_ACPI_H
#define MVM_ACPI_H

#include "devices.h"

#define ACPI_PM_BASE 0x600
#define ACPI_GPE0_BASE 0xafe0
#define ACPI_SCI_IRQ 9
#define ACPI_MAX_FIXUPS 32

typedef struct {
    uint32_t pci_hole_start; /* inicio da janela de memoria PCI (0x80000000 ou 0xC0000000) */
    uint8_t ioapic_id;
    uint32_t hpet_block_id; /* 0 = sem HPET */
    uint8_t floppies;       /* bit 0 = A:, bit 1 = B: (0 = sem controlador de disquete) */
} acpi_config;

/* ponteiro dentro de um arquivo que recebe o endereco base de outro */
typedef struct { int dest; uint32_t off; uint8_t size; int src; } acpi_ptr_fixup;
typedef struct { int file; uint32_t off, start, len; } acpi_cksum_fixup;

typedef struct {
    uint8_t *data;      /* FACS, DSDT, FADT, MADT, RSDT, XSDT (enderecos relativos) */
    uint32_t len, cap;
    uint8_t rsdp[36];
    acpi_ptr_fixup ptr[ACPI_MAX_FIXUPS];
    int nptr;
    acpi_cksum_fixup ck[ACPI_MAX_FIXUPS];
    int nck;
} acpi_tables;

void acpi_build(acpi_tables *t, const acpi_config *cfg);
void acpi_tables_free(acpi_tables *t);
/* boot direto: grava as tabelas ja ligadas na RAM do convidado */
void acpi_install(acpi_tables *t, uint8_t *ram, uint64_t rsdp_addr, uint64_t tables_addr);
/* com BIOS: arquivos e comandos do table-loader para o SeaBIOS */
void acpi_add_to_fw_cfg(acpi_tables *t, fw_cfg *f);

typedef struct acpi_pm acpi_pm;
extern const mvm_io_ops acpi_pm_ops;  /* 0x600, 0x40 bytes */
extern const mvm_io_ops acpi_gpe_ops; /* 0xAFE0, 4 bytes */
acpi_pm *acpi_pm_new(mvm_vm *vm, irq_line sci);
void acpi_pm_free(acpi_pm *p);
void acpi_pm_reset(acpi_pm *p);
void acpi_pm_power_button(acpi_pm *p);

#endif
