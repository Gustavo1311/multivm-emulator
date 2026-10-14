/* Flash CFI para as variaveis UEFI (OVMF_VARS.fd). */
#ifndef MVM_PFLASH_H
#define MVM_PFLASH_H
#include "devices.h"

typedef struct pflash pflash;
extern const mvm_io_ops pflash_ops;

/* abre o arquivo (gravavel se possivel); tamanho multiplo de 4 KiB e no maximo size_limit */
pflash *pflash_open(const char *path, uint32_t size_limit, char *err, size_t errlen);
uint32_t pflash_size(pflash *p);
void pflash_reset(pflash *p);
void pflash_flush(pflash *p);
void pflash_free(pflash *p);

#endif
