/* API publica de imagens: informacao, criacao (MVD), conversao e verificacao. */
#include "img.h"
#include "mvd.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* bytes a partir de off que o formato garante serem zero (sem ler) */
static uint64_t known_zero(mvm_blk *b, uint64_t off, uint64_t want)
{
    if (!b->drv || !b->drv->zero_span)
        return 0;
    uint64_t z = 0;
    while (z < want) {
        uint64_t s = b->drv->zero_span(b->st, off + z);
        if (!s)
            break;
        z += s;
    }
    return z;
}

static mvm_blk *open_src(const char *path, int fd, char *err, size_t errlen)
{
    if (path)
        return blk_open(path, true, err, errlen);
    int d = dup(fd);
    if (d < 0) {
        snprintf(err, errlen, "descritor invalido: %s", strerror(errno));
        return NULL;
    }
    return blk_open_fd(d, true, err, errlen);
}

int mvm_image_info_get(const char *path, int fd, mvm_image_info *info, char *err, size_t errlen)
{
    memset(info, 0, sizeof(*info));
    mvm_blk *b;
    if (path) { /* tenta com escrita para saber se o formato aceita */
        b = blk_open(path, false, err, errlen);
    } else {
        int d = dup(fd);
        b = d < 0 ? NULL : blk_open_fd(d, false, err, errlen);
        if (d < 0)
            snprintf(err, errlen, "descritor invalido");
    }
    if (!b)
        return -1;
    snprintf(info->format, sizeof(info->format), "%s", blk_format(b));
    info->virtual_size = b->size;
    info->file_size = img_file_size(b->fd);
    info->writable = !b->readonly;
    blk_close(b);
    return 0;
}

int mvm_image_create(const char *path, uint64_t size, uint32_t block_size, const char *backing, char *err,
                     size_t errlen)
{
    unsigned bits = 0;
    if (block_size) {
        while ((1u << bits) < block_size)
            bits++;
        if ((1u << bits) != block_size) {
            snprintf(err, errlen, "o tamanho do bloco tem de ser potencia de 2");
            return -1;
        }
    }
    if (backing && !size) { /* overlay: herda o tamanho do base */
        img_open_args a = {.fd = -1, .path = path};
        mvm_blk *bb = img_open_backing(&a, backing, err, errlen);
        if (!bb)
            return -1;
        size = (bb->size + 511) & ~511ULL;
        blk_close(bb);
    }
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        snprintf(err, errlen, "nao foi possivel criar '%s': %s", path, strerror(errno));
        return -1;
    }
    size = (size + 511) & ~511ULL;
    int r = mvd_create_fd(fd, size, bits, backing, err, errlen);
    close(fd);
    if (r < 0)
        unlink(path);
    return r;
}

int mvm_image_convert(const char *src, int src_fd, const char *dst, int fmt, unsigned flags, uint32_t block_size,
                      mvm_progress_cb cb, void *opaque, char *err, size_t errlen)
{
    mvm_blk *in = open_src(src, src_fd, err, errlen);
    if (!in)
        return -1;
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s.part", dst);
    unlink(tmp);
    uint64_t total = (in->size + 511) & ~511ULL;
    mvm_blk *out = NULL;
    int ofd = -1, r = -1;
    uint8_t *buf = NULL;
    size_t chunk = 1u << 20;
    if (fmt == MVM_IMAGE_MVD) {
        unsigned bits = 0;
        if (block_size)
            while ((1u << bits) < block_size)
                bits++;
        int fd = open(tmp, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            snprintf(err, errlen, "nao foi possivel criar '%s': %s", tmp, strerror(errno));
            goto out;
        }
        int c = mvd_create_fd(fd, total, bits, NULL, err, errlen);
        close(fd);
        if (c < 0)
            goto out;
        if (!(out = blk_open(tmp, false, err, errlen)))
            goto out;
        chunk = (size_t)mvd_block_size(out->st);
    } else if (fmt == MVM_IMAGE_RAW) {
        ofd = open(tmp, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (ofd < 0 || ftruncate(ofd, (off_t)total) < 0) {
            snprintf(err, errlen, "nao foi possivel criar '%s': %s", tmp, strerror(errno));
            goto out;
        }
    } else {
        snprintf(err, errlen, "formato de saida desconhecido");
        goto out;
    }
    buf = malloc(chunk);
    if (!buf) {
        snprintf(err, errlen, "sem memoria");
        goto out;
    }
    for (uint64_t off = 0; off < total; off += chunk) {
        size_t n = total - off < chunk ? (size_t)(total - off) : chunk;
        if (known_zero(in, off, n) >= n) { /* nao alocado na origem: nada a copiar */
            if (cb && !cb(opaque, off + n, total)) {
                snprintf(err, errlen, "conversao cancelada");
                goto out;
            }
            continue;
        }
        if (blk_read(in, off, buf, n) < 0) {
            snprintf(err, errlen, "erro de leitura em %llu: %s", (unsigned long long)off, strerror(errno));
            goto out;
        }
        if (n < chunk)
            memset(buf + n, 0, chunk - n);
        if (!img_all_zero(buf, n)) {
            int w = out ? mvd_put_block(out->st, off / chunk, buf, flags & MVM_IMAGE_COMPRESS)
                        : img_pwrite(ofd, buf, n, off);
            if (w < 0) {
                snprintf(err, errlen, "erro de escrita em %llu: %s", (unsigned long long)off, strerror(errno));
                goto out;
            }
        }
        if (cb && !cb(opaque, off + n, total)) {
            snprintf(err, errlen, "conversao cancelada");
            goto out;
        }
    }
    if ((out && blk_flush(out) < 0) || (ofd >= 0 && fdatasync(ofd) < 0)) {
        snprintf(err, errlen, "erro ao gravar: %s", strerror(errno));
        goto out;
    }
    r = 0;
out:
    free(buf);
    blk_close(out);
    if (ofd >= 0)
        close(ofd);
    blk_close(in);
    if (r == 0 && rename(tmp, dst) < 0) {
        snprintf(err, errlen, "nao foi possivel renomear para '%s': %s", dst, strerror(errno));
        r = -1;
    }
    if (r < 0)
        unlink(tmp);
    return r;
}

int mvm_image_check(const char *path, char *report, size_t report_len, mvm_progress_cb cb, void *opaque)
{
    char err[512];
    mvm_blk *b = blk_open(path, true, err, sizeof(err));
    if (!b) {
        snprintf(report, report_len, "ERRO: %s\n", err);
        return -1;
    }
    size_t o = 0;
#define OUT(...) (o += (size_t)snprintf(report + o, o < report_len ? report_len - o : 0, __VA_ARGS__))
    uint64_t fsize = img_file_size(b->fd);
    OUT("formato: %s\ntamanho virtual: %llu bytes (%.2f GiB)\narquivo: %llu bytes (%.2f GiB)\n", blk_format(b),
        (unsigned long long)b->size, b->size / 1073741824.0, (unsigned long long)fsize, fsize / 1073741824.0);
    if (b->drv == &img_mvd) {
        mvd_stats s;
        mvd_get_stats(b->st, &s);
        OUT("blocos: %llu de %llu KiB; com dados: %llu, comprimidos: %llu, zerados: %llu\n",
            (unsigned long long)s.blocks, (unsigned long long)(s.block_size >> 10), (unsigned long long)s.data_blocks,
            (unsigned long long)s.lz4_blocks, (unsigned long long)s.zero_blocks);
        OUT("espaco em uso: %llu bytes; reaproveitavel/desperdicado: %llu bytes\n", (unsigned long long)s.used_bytes,
            (unsigned long long)(fsize > s.used_bytes ? fsize - s.used_bytes : 0));
        if (s.backing[0])
            OUT("arquivo base: %s\n", s.backing);
    }
    int r = 0;
    size_t chunk = 4u << 20;
    uint8_t *buf = malloc(chunk);
    for (uint64_t off = 0; buf && off < b->size; off += chunk) {
        size_t n = b->size - off < chunk ? (size_t)(b->size - off) : chunk;
        if (blk_read(b, off, buf, n) < 0) {
            OUT("ERRO de leitura em %llu: %s\n", (unsigned long long)off, strerror(errno));
            r = -1;
            break;
        }
        if (cb && !cb(opaque, off + n, b->size)) {
            OUT("verificacao interrompida\n");
            r = -1;
            break;
        }
    }
    if (r == 0)
        OUT("nenhum erro encontrado\n");
#undef OUT
    free(buf);
    blk_close(b);
    return r;
}
