/*
 * Teste da camada de disco do MultiVM (todos os formatos):
 *   imgtest read IMG SAIDA.raw              le o disco virtual inteiro via blk_read
 *   imgtest write IMG ESPELHO.raw SEED N [D] N escritas aleatorias, repetidas no espelho raw;
 *                                           com D, ~1/6 das operacoes sao discard (TRIM)
 *   imgtest open IMG                        so abre (mensagens de erro, formato, tamanho)
 *   imgtest crash IMG COMMIT.raw TOCADO.raw SEED N
 *                                           escritas+discards com um flush no meio; COMMIT.raw
 *                                           recebe o estado do flush e TOCADO.raw marca (0xff) o
 *                                           que mudou depois; termina sem fechar (queda)
 *   imgtest verify2 IMG COMMIT.raw TOCADO.raw  setores nao tocados depois do flush tem de estar
 *                                           exatamente como no flush
 *   imgtest guard IMG                       gravar assinatura de formato numa imagem raw falha
 *   imgtest bench IMG MB                    desempenho: sequencial, 4 KiB aleatorio (sobrescrita e
 *                                           alocacao), leitura sem cache
 */
#include "internal.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t rng;
static uint64_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static uint8_t buf[5 << 20], chk[5 << 20];

/* uma operacao aleatoria (escrita ou discard) no disco e nos espelhos */
static int one_op(mvm_blk *b, int *mirrors, int nm, bool discard, int i)
{
    size_t len = 512 * (1 + rnd() % 600); /* 512 B .. 300 KiB */
    if (rnd() % 16 == 0)
        len = 512 * (1 + rnd() % 8192); /* as vezes ate 4 MiB (blocos inteiros) */
    uint64_t off = (rnd() % (b->size / 512)) * 512;
    if (off + len > b->size)
        len = (size_t)(b->size - off);
    bool d = discard && rnd() % 6 == 0;
    if (d || rnd() % 4 == 0)
        memset(buf, 0, len);
    else
        for (size_t k = 0; k < len; k += 8) {
            uint64_t v = rnd();
            memcpy(buf + k, &v, 8);
        }
    int r = d ? blk_discard(b, off, len) : blk_write(b, off, buf, len);
    if (r < 0) {
        printf("erro de %s em %llu (%zu bytes)\n", d ? "discard" : "escrita", (unsigned long long)off, len);
        return -1;
    }
    for (int k = 0; k < nm; k++)
        if (pwrite(mirrors[k], buf, len, (off_t)off) != (ssize_t)len)
            return -1;
    if (i % 97 == 0) { /* le de volta e confere */
        if (blk_read(b, off, chk, len) < 0 || memcmp(chk, buf, len)) {
            printf("leitura apos %s divergiu em %llu\n", d ? "discard" : "escrita", (unsigned long long)off);
            return -1;
        }
    }
    return 0;
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void drop_cache(mvm_blk *b)
{
    blk_flush(b);
    posix_fadvise(b->fd, 0, 0, POSIX_FADV_DONTNEED);
}

static int bench(mvm_blk *b, uint64_t mb)
{
    uint64_t total = mb << 20;
    if (2 * total > b->size)
        return 1;
    for (size_t k = 0; k < (1 << 20); k += 8) {
        uint64_t v = rnd();
        memcpy(buf + k, &v, 8);
    }
    double t = now();
    for (uint64_t off = 0; off < total; off += 1 << 20) {
        buf[0]++;
        if (blk_write(b, off, buf, 1 << 20) < 0)
            return 1;
    }
    blk_flush(b);
    double seqw = mb / (now() - t);
    const int N = 4000;
    t = now();
    for (int i = 0; i < N; i++) { /* sobrescrita de 4 KiB, flush a cada 64 (como um FS com journal) */
        buf[0]++;
        if (blk_write(b, (rnd() % (total / 4096)) * 4096, buf, 4096) < 0)
            return 1;
        if (i % 64 == 63)
            blk_flush(b);
    }
    blk_flush(b);
    double rw = N / (now() - t);
    t = now();
    for (int i = 0; i < N / 4; i++) { /* 4 KiB em area nunca escrita: aloca */
        buf[0]++;
        if (blk_write(b, total + (rnd() % (total / 4096)) * 4096, buf, 4096) < 0)
            return 1;
        if (i % 64 == 63)
            blk_flush(b);
    }
    blk_flush(b);
    double aw = (N / 4) / (now() - t);
    drop_cache(b);
    t = now();
    for (int i = 0; i < N; i++)
        if (blk_read(b, (rnd() % (total / 4096)) * 4096, chk, 4096) < 0)
            return 1;
    double rr = N / (now() - t);
    drop_cache(b);
    t = now();
    for (uint64_t off = 0; off < total; off += 1 << 20)
        if (blk_read(b, off, chk, 1 << 20) < 0)
            return 1;
    double seqr = mb / (now() - t);
    printf("%-6s seq-escrita %6.1f MB/s | 4K sobrescrita %6.0f IOPS | 4K alocando %6.0f IOPS | 4K leitura %6.0f IOPS | seq-leitura %6.1f MB/s\n",
           blk_format(b), seqw, rw, aw, rr, seqr);
    return 0;
}

int main(int argc, char **argv)
{
    char err[512];
    if (argc < 3)
        return 2;
    const char *mode = argv[1];
    bool ro = !strcmp(mode, "read") || !strcmp(mode, "open") || !strcmp(mode, "verify2");
    mvm_blk *b = blk_open(argv[2], ro, err, sizeof(err));
    if (!b) {
        printf("erro: %s\n", err);
        return 1;
    }
    if (!strcmp(mode, "open")) {
        printf("aberto: %llu bytes, %s%s\n", (unsigned long long)b->size, blk_format(b), b->readonly ? " (somente leitura)" : "");
        blk_close(b);
        return 0;
    }
    if (!strcmp(mode, "read")) {
        FILE *o = fopen(argv[3], "wb");
        for (uint64_t off = 0; off < b->size; off += 1 << 20) {
            size_t n = b->size - off < (1 << 20) ? (size_t)(b->size - off) : (1 << 20);
            if (blk_read(b, off, buf, n) < 0) {
                printf("erro de leitura em %llu\n", (unsigned long long)off);
                return 1;
            }
            fwrite(buf, 1, n, o);
        }
        fclose(o);
        blk_close(b);
        return 0;
    }
    if (!strcmp(mode, "verify2")) {
        int fa = open(argv[3], O_RDONLY), fb = open(argv[4], O_RDONLY);
        static uint8_t ba[1 << 20], bb[1 << 20];
        uint64_t mixed = 0;
        for (uint64_t off = 0; off < b->size; off += 1 << 20) {
            size_t n = b->size - off < (1 << 20) ? (size_t)(b->size - off) : (1 << 20);
            if (blk_read(b, off, buf, n) < 0 || pread(fa, ba, n, (off_t)off) != (ssize_t)n ||
                pread(fb, bb, n, (off_t)off) != (ssize_t)n) {
                printf("erro de leitura em %llu\n", (unsigned long long)off);
                return 1;
            }
            for (size_t s = 0; s < n; s += 512) {
                if (bb[s]) { /* tocado depois do flush: qualquer estado gravado vale */
                    mixed++;
                    continue;
                }
                if (memcmp(buf + s, ba + s, 512)) {
                    printf("setor %llu (nao tocado depois do flush) mudou\n", (unsigned long long)((off + s) / 512));
                    return 1;
                }
            }
        }
        printf("ok (%llu setores tocados depois do flush)\n", (unsigned long long)mixed);
        return 0;
    }
    if (!strcmp(mode, "bench")) {
        rng = 12345;
        int r = bench(b, strtoull(argv[3], NULL, 0));
        blk_close(b);
        return r;
    }
    if (!strcmp(mode, "guard")) {
        memset(buf, 0, 512);
        memcpy(buf, "QFI\xfb\0\0\0\3", 8);
        int r1 = blk_write(b, 0, buf, 512);
        memcpy(buf, "conectix", 8);
        int r2 = blk_write(b, b->size - 512, buf, 512);
        memcpy(buf, "texto comum", 11);
        int r3 = blk_write(b, 0, buf, 512);
        printf("%s\n", r1 < 0 && r2 < 0 && r3 == 0 ? "ok" : "FALHOU");
        blk_close(b);
        return !(r1 < 0 && r2 < 0 && r3 == 0);
    }
    if (!strcmp(mode, "crash")) {
        int commit = open(argv[3], O_RDWR), touched = open(argv[4], O_RDWR);
        rng = strtoull(argv[5], NULL, 0) | 1;
        int n = atoi(argv[6]);
        static uint8_t ff[5 << 20];
        memset(ff, 0xff, sizeof(ff));
        for (int i = 0; i < n; i++) {
            if (i < n / 2) {
                if (one_op(b, &commit, 1, true, i) < 0)
                    return 1;
            } else {
                uint64_t save = rng;
                if (one_op(b, NULL, 0, true, i) < 0)
                    return 1;
                /* repete o sorteio de offset/tamanho para marcar o trecho tocado */
                uint64_t after = rng;
                rng = save;
                size_t len = 512 * (1 + rnd() % 600);
                if (rnd() % 16 == 0)
                    len = 512 * (1 + rnd() % 8192);
                uint64_t off = (rnd() % (b->size / 512)) * 512;
                if (off + len > b->size)
                    len = (size_t)(b->size - off);
                if (pwrite(touched, ff, len, (off_t)off) != (ssize_t)len)
                    return 1;
                rng = after;
            }
            if (i == n / 2 - 1 && blk_flush(b) < 0)
                return 1;
        }
        _exit(0); /* sem flush nem close: o que estiver so na memoria se perde */
    }
    /* write */
    int m = open(argv[3], O_RDWR);
    rng = strtoull(argv[4], NULL, 0) | 1;
    int n = atoi(argv[5]);
    bool discard = argc > 6;
    for (int i = 0; i < n; i++)
        if (one_op(b, &m, 1, discard, i) < 0)
            return 1;
    blk_flush(b);
    blk_close(b);
    close(m);
    return 0;
}
