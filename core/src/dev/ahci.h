/* Controlador SATA AHCI (ICH9). */
#ifndef MVM_AHCI_H
#define MVM_AHCI_H

#include "devices.h"

#define AHCI_PORTS 8
#define AHCI_ABAR_SIZE 0x1000

typedef struct ahci ahci;
extern const mvm_io_ops ahci_mmio_ops; /* BAR5 (ABAR) */

ahci *ahci_new(mvm_vm *vm, irq_line irq);
void ahci_free(ahci *h);
void ahci_reset(ahci *h);
/* blk NULL com cdrom = unidade sem midia */
bool ahci_attach(ahci *h, int port, mvm_blk *blk, bool cdrom);
/* 0 = vazio, 1 = disco ATA, 2 = CD ATAPI */
int ahci_port_kind(ahci *h, int port);
/* com a VM ligada: conecta/desconecta um disco (hot-plug) e troca a midia de um CD */
bool ahci_hotplug(ahci *h, int port, mvm_blk *blk);
bool ahci_unplug(ahci *h, int port);
bool ahci_change_media(ahci *h, int port, mvm_blk *blk);
int ahci_free_port(ahci *h);

#endif
