/* MVD (MultiVM Disk): criacao e acesso por bloco usados pela conversao. */
#ifndef MVM_MVD_H
#define MVM_MVD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MVD_DEFAULT_BLOCK_BITS 18 /* 256 KiB */

/* cria uma imagem MVD vazia no descritor (trunca o arquivo) */
int mvd_create_fd(int fd, uint64_t vsize, unsigned block_bits, const char *backing, char *err, size_t errlen);
/* grava um bloco inteiro (st = estado do driver), comprimindo com LZ4 se compensar */
int mvd_put_block(void *st, uint64_t bi, const uint8_t *data, bool compress);
uint64_t mvd_block_size(void *st);

typedef struct {
    uint64_t block_size, blocks;
    uint64_t data_blocks, zero_blocks, lz4_blocks;
    uint64_t used_bytes;  /* bytes do arquivo referenciados pelo mapa (+ cabecalho e mapa) */
    uint64_t free_slots;  /* espacos livres reaproveitaveis */
    char backing[1024];
} mvd_stats;
void mvd_get_stats(void *st, mvd_stats *s);

#endif
