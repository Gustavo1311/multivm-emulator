/* Utilitarios comuns aos drivers de formato de imagem. */
#include "img.h"
#include "../inflate.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int img_pread(int fd, void *buf, size_t len, uint64_t off)
{
    uint8_t *p = buf;
    while (len) {
        ssize_t r = pread(fd, p, len, (off_t)off);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0) { /* alem do fim do arquivo: zeros */
            memset(p, 0, len);
            return 0;
        }
        p += r;
        off += (uint64_t)r;
        len -= (size_t)r;
    }
    return 0;
}

int img_pwrite(int fd, const void *buf, size_t len, uint64_t off)
{
    const uint8_t *p = buf;
    while (len) {
        ssize_t r = pwrite(fd, p, len, (off_t)off);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += r;
        off += (uint64_t)r;
        len -= (size_t)r;
    }
    return 0;
}

uint64_t img_file_size(int fd)
{
    struct stat st;
    return fstat(fd, &st) == 0 ? (uint64_t)st.st_size : 0;
}

uint32_t img_crc32c(uint32_t crc, const void *buf, size_t len)
{
    static uint32_t table[256];
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c >> 1) ^ (0x82f63b78u & (0u - (c & 1)));
            table[i] = c;
        }
    }
    const uint8_t *p = buf;
    crc = ~crc;
    while (len--)
        crc = table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

bool img_all_zero(const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len && ((uintptr_t)p & 7)) {
        if (*p++)
            return false;
        len--;
    }
    const uint64_t *q = (const uint64_t *)(const void *)p;
    for (; len >= 32; len -= 32, q += 4)
        if (q[0] | q[1] | q[2] | q[3])
            return false;
    p = (const uint8_t *)q;
    while (len--)
        if (*p++)
            return false;
    return true;
}

void img_rel_path(const char *base, const char *rel, char *out, size_t outlen)
{
    const char *slash = base ? strrchr(base, '/') : NULL;
    if (rel[0] == '/' || !slash)
        snprintf(out, outlen, "%s", rel);
    else
        snprintf(out, outlen, "%.*s/%s", (int)(slash - base), base, rel);
}

mvm_blk *img_open_backing(const img_open_args *a, const char *rel, char *err, size_t errlen)
{
    if (!a->path) {
        snprintf(err, errlen, "a imagem depende do arquivo base '%s', que nao pode ser aberto pelo "
                              "seletor de arquivos; converta-a antes para uma imagem independente", rel);
        return NULL;
    }
    char path[4096];
    img_rel_path(a->path, rel, path, sizeof(path));
    char e2[256];
    mvm_blk *b = blk_open(path, true, e2, sizeof(e2));
    if (!b)
        snprintf(err, errlen, "arquivo base '%s': %s", path, e2);
    return b;
}

long img_zlib_inflate(const void *in, size_t inlen, void *out, size_t outlen)
{
    const uint8_t *p = in;
    if (inlen < 2 || (p[0] & 0x0f) != 8 || ((p[0] << 8) | p[1]) % 31 || (p[1] & 0x20))
        return -1;
    return mvm_inflate(p + 2, inlen - 2, out, outlen);
}
