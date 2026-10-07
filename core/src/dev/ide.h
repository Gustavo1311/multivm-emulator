/* Controlador IDE (PIIX3) com discos ATA e CD-ROM ATAPI. */
#ifndef MVM_IDE_H
#define MVM_IDE_H

#include "devices.h"

typedef struct ide_ctrl ide_ctrl;

ide_ctrl *ide_new(mvm_vm *vm, irq_line irq14, irq_line irq15);
void ide_debug_dump(ide_ctrl *c);
void ide_free(ide_ctrl *c);
void ide_reset(ide_ctrl *c);
/* bus 0/1 (primario/secundario), unit 0/1 (mestre/escravo). blk NULL = CD sem midia */
bool ide_attach(ide_ctrl *c, int bus, int unit, mvm_blk *blk, bool cdrom);
void *ide_bus_opaque(ide_ctrl *c, int bus);
/* 0 = vazio, 1 = disco ATA, 2 = CD-ROM ATAPI */
int ide_drive_kind(ide_ctrl *c, int bus, int unit);
/* troca a midia de um CD (blk NULL = ejetado); o convidado e avisado por UNIT ATTENTION */
bool ide_change_media(ide_ctrl *c, int bus, int unit, mvm_blk *blk);
void ide_bmdma_run(ide_ctrl *c, int bus);

/* callback de scatter-gather: copia ate len bytes de/para o convidado, retorna o copiado */
typedef uint32_t (*ide_xfer_fn)(void *opaque, uint8_t *data, uint32_t len, bool to_host);

/* Execucao de comandos ATA/ATAPI para outros transportes (AHCI): um disco por porta. */
typedef struct ide_port ide_port;
typedef struct {
    uint8_t status, error, device;
    uint8_t nsector, sector, lcyl, hcyl, hob_nsector, hob_sector, hob_lcyl, hob_hcyl;
    uint32_t bytes; /* bytes transferidos */
    bool pio;       /* o comando usou PIO (o AHCI manda um FIS PIO Setup) */
} ide_port_result;
ide_port *ide_port_new(mvm_vm *vm, mvm_blk *blk, bool cdrom);
void ide_port_free(ide_port *p);
void ide_port_reset(ide_port *p);
int ide_port_kind(ide_port *p);
bool ide_port_change_media(ide_port *p, mvm_blk *blk);
uint32_t ide_port_signature(ide_port *p);
/* fis: FIS H2D de registradores (20 bytes); cdb: pacote ATAPI (12 bytes) */
void ide_port_exec(ide_port *p, const uint8_t *fis, const uint8_t *cdb, ide_xfer_fn fn, void *opaque, ide_port_result *r);

extern const mvm_io_ops ide_cmd_ops;   /* 8 portas (0x1F0 / 0x170): opaque = ide_bus_opaque */
extern const mvm_io_ops ide_ctl_ops;   /* 1 porta (0x3F6 / 0x376) */
extern const mvm_io_ops ide_bmdma_ops; /* BAR4: 16 portas, opaque = ide_ctrl */

#endif
