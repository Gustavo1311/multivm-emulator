/* Descompressor DEFLATE bruto (RFC 1951). */
#ifndef MVM_INFLATE_H
#define MVM_INFLATE_H

#include <stddef.h>
#include <stdint.h>

/* Descomprime ate outlen bytes; devolve o numero de bytes gerados ou -1 (dados invalidos). */
long mvm_inflate(const void *in, size_t inlen, void *out, size_t outlen);

#endif
