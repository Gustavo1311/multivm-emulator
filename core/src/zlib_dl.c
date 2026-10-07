/*
 * Compressao deflate pela zlib do sistema, carregada com dlopen (o build do
 * Android sem NDK nao tem zlib.h; libz.so faz parte da API estavel do Android).
 * A struct z_stream abaixo segue o layout da zlib 1.2/1.3, que e estavel.
 * Se a biblioteca nao existir, zdl_available() devolve false e quem usa cai num
 * caminho sem compressao.
 */
#include "internal.h"

#include <dlfcn.h>
#include <stdlib.h>

typedef struct {
    const uint8_t *next_in;
    unsigned avail_in;
    unsigned long total_in;
    uint8_t *next_out;
    unsigned avail_out;
    unsigned long total_out;
    const char *msg;
    void *state;
    void *zalloc, *zfree, *opaque;
    int data_type;
    unsigned long adler, reserved;
} z_stream_;

#define Z_SYNC_FLUSH_ 2
#define Z_OK_ 0
#define Z_BUF_ERROR_ (-5)

static struct {
    pthread_once_t once;
    bool ok;
    int (*init)(z_stream_ *, int level, const char *version, int size);
    int (*deflate)(z_stream_ *, int flush);
    int (*end)(z_stream_ *);
} zl = {PTHREAD_ONCE_INIT, false, NULL, NULL, NULL};

static void load(void)
{
    static const char *names[] = {"libz.so", "libz.so.1"};
    for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
        void *h = dlopen(names[i], RTLD_NOW);
        if (!h)
            continue;
        *(void **)&zl.init = dlsym(h, "deflateInit_");
        *(void **)&zl.deflate = dlsym(h, "deflate");
        *(void **)&zl.end = dlsym(h, "deflateEnd");
        if (zl.init && zl.deflate && zl.end) {
            zl.ok = true;
            return;
        }
    }
    LOGW("zlib nao encontrada: VNC sem compressao deflate");
}

bool zdl_available(void)
{
    pthread_once(&zl.once, load);
    return zl.ok;
}

struct zdl_stream {
    z_stream_ s;
};

zdl_stream *zdl_new(int level)
{
    if (!zdl_available())
        return NULL;
    zdl_stream *z = calloc(1, sizeof(*z));
    if (zl.init(&z->s, level, "1.2.13", (int)sizeof(z_stream_)) != Z_OK_) {
        free(z);
        return NULL;
    }
    return z;
}

void zdl_free(zdl_stream *z)
{
    if (!z)
        return;
    zl.end(&z->s);
    free(z);
}

/* comprime in com Z_SYNC_FLUSH (o fluxo continua); devolve os bytes escritos ou -1 */
long zdl_compress(zdl_stream *z, const void *in, size_t inlen, void *out, size_t outcap)
{
    z->s.next_in = in;
    z->s.avail_in = (unsigned)inlen;
    z->s.next_out = out;
    z->s.avail_out = (unsigned)outcap;
    int r = zl.deflate(&z->s, Z_SYNC_FLUSH_);
    if ((r != Z_OK_ && r != Z_BUF_ERROR_) || z->s.avail_in)
        return -1;
    return (long)(outcap - z->s.avail_out);
}

/* tamanho maximo da saida para inlen bytes (margem da zlib + sync flush) */
size_t zdl_bound(size_t inlen) { return inlen + inlen / 1000 + 64; }
