/* Flash CFI para as variaveis UEFI (OVMF_VARS.fd no x86, QEMU_VARS.fd no ARM). */
#ifndef MVM_PFLASH_H
#define MVM_PFLASH_H
#include "devices.h"

typedef struct pflash pflash;
extern const mvm_io_ops pflash_ops;

/* abre o arquivo (gravavel se possivel); tamanho multiplo de 4 KiB e no maximo size_limit */
pflash *pflash_open(const char *path, uint32_t size_limit, char *err, size_t errlen);
/* largura do barramento (1 = x86/OVMF, 4 = ARM/ArmVirtQemu) e bloco apagavel em bytes */
pflash *pflash_open_ex(const char *path, uint32_t size_limit, uint32_t width, uint32_t sector, char *err,
                       size_t errlen);
/* flash apagada (0xFF) so na memoria: as variaveis se perdem ao fechar a VM */
pflash *pflash_new_blank(uint32_t size, uint32_t width, uint32_t sector);
uint32_t pflash_size(pflash *p);
void pflash_reset(pflash *p);
void pflash_flush(pflash *p);
void pflash_free(pflash *p);

#endif
