/*
 * Formatos de imagem de disco: cada formato e um driver atras de blk_* (util.c).
 * Os dispositivos (IDE, AHCI, virtio-blk) so enxergam o disco virtual.
 */
#ifndef MVM_IMG_H
#define MVM_IMG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../internal.h"

/* o que blk_setup le do arquivo para reconhecer o formato */
typedef struct {
    const uint8_t *head; /* primeiros bytes (ate 4 KiB; zeros alem do fim) */
    size_t head_len;
    const uint8_t *tail; /* ultimos 512 bytes (rodape VHD) */
    uint64_t fsize;      /* tamanho do arquivo */
} img_probe_info;

/* parametros de abertura */
typedef struct {
    int fd;
    const char *path; /* NULL se aberto so pelo descritor (Android/SAF) */
    bool readonly;    /* entrada e saida (o driver pode forcar somente leitura) */
    uint64_t fsize;
    uint64_t vsize;   /* saida: tamanho do disco virtual */
    bool can_discard; /* saida: discard libera espaco de verdade */
} img_open_args;

typedef struct blk_driver {
    const char *name;
    bool (*probe)(const img_probe_info *p);
    void *(*open)(img_open_args *a, char *err, size_t errlen);
    int (*read)(void *st, uint64_t off, void *buf, size_t len);
    int (*write)(void *st, uint64_t off, const void *buf, size_t len);
    int (*flush)(void *st);
    int (*discard)(void *st, uint64_t off, uint64_t len); /* NULL: ignorado */
    /* bytes a partir de off que com certeza sao zero sem ler (nao alocados); 0 = desconhecido */
    uint64_t (*zero_span)(void *st, uint64_t off);
    void (*close)(void *st);
} blk_driver;

extern const blk_driver img_qcow2, img_vdi, img_vmdk, img_vhd, img_vhdx, img_mvd;

/* ---- utilitarios (img_util.c) ---- */
int img_pread(int fd, void *buf, size_t len, uint64_t off);  /* alem do fim: zeros */
int img_pwrite(int fd, const void *buf, size_t len, uint64_t off);
uint64_t img_file_size(int fd);

static inline uint16_t rd16le(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32le(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline uint64_t rd64le(const uint8_t *p) { return rd32le(p) | ((uint64_t)rd32le(p + 4) << 32); }
static inline uint32_t rd32be(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static inline uint64_t rd64be(const uint8_t *p) { return ((uint64_t)rd32be(p) << 32) | rd32be(p + 4); }
static inline void wr16le(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void wr32le(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static inline void wr64le(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static inline void wr32be(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (24 - 8 * i)); }
static inline void wr64be(uint8_t *p, uint64_t v) { wr32be(p, (uint32_t)(v >> 32)); wr32be(p + 4, (uint32_t)v); }

uint32_t img_crc32c(uint32_t crc, const void *buf, size_t len); /* crc inicial 0 */
bool img_all_zero(const void *buf, size_t len);

/* caminho de 'rel' relativo ao diretorio de 'base' (absoluto: copiado) */
void img_rel_path(const char *base, const char *rel, char *out, size_t outlen);
/* abre um arquivo base (somente leitura, qualquer formato) referenciado por 'rel' */
mvm_blk *img_open_backing(const img_open_args *a, const char *rel, char *err, size_t errlen);

/* descompressao zlib (cabecalho de 2 bytes + deflate) */
long img_zlib_inflate(const void *in, size_t inlen, void *out, size_t outlen);

/* LZ4 (formato de bloco), usado pelo MVD */
size_t lz4_compress(const uint8_t *src, size_t n, uint8_t *dst, size_t cap); /* 0 = nao coube */
long lz4_decompress(const uint8_t *src, size_t n, uint8_t *dst, size_t cap); /* -1 = invalido */

#endif
