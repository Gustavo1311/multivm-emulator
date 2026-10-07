/*
 * mvm-img: ferramenta de imagens de disco do MultiVM.
 *   info ARQ                      formato e tamanhos
 *   create [-b BLOCO] ARQ.mvd TAM cria um MVD vazio (TAM: 20G, 512M, bytes)
 *   snapshot BASE NOVO.mvd        MVD vazio que le os blocos nao gravados de BASE
 *   convert [-O mvd|raw] [-c] [-b BLOCO] ORIGEM DESTINO
 *   compact [-c] ARQ.mvd          reescreve o MVD sem espaco desperdicado
 *   check ARQ                     le tudo e confere a estrutura
 */
#define _GNU_SOURCE
#include "mvm.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

static void usage(void)
{
    fprintf(stderr,
            "uso: mvm-img info ARQ\n"
            "     mvm-img create [-b BLOCO] ARQ.mvd TAMANHO\n"
            "     mvm-img snapshot BASE NOVO.mvd\n"
            "     mvm-img convert [-O mvd|raw] [-c] [-b BLOCO] ORIGEM DESTINO\n"
            "     mvm-img compact [-c] ARQ.mvd\n"
            "     mvm-img check ARQ\n"
            "formatos lidos: raw, qcow2, vdi, vmdk, vhd, vhdx, mvd\n");
    exit(2);
}

static uint64_t parse_size(const char *s)
{
    char *e;
    double v = strtod(s, &e);
    uint64_t m = 1;
    switch (*e) {
    case 'k': case 'K': m = 1ULL << 10; break;
    case 'm': case 'M': m = 1ULL << 20; break;
    case 'g': case 'G': m = 1ULL << 30; break;
    case 't': case 'T': m = 1ULL << 40; break;
    case 0: break;
    default: fprintf(stderr, "tamanho invalido: %s\n", s); exit(2);
    }
    return (uint64_t)(v * (double)m);
}

static double mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static bool progress(void *opaque, uint64_t done, uint64_t total)
{
    double *last = opaque;
    double t = mono();
    if (isatty(2) && (t - *last > 0.5 || done == total)) {
        fprintf(stderr, "\r  %5.1f%%", total ? 100.0 * done / total : 100.0);
        *last = t;
    }
    return true;
}

static int convert(const char *src, const char *dst, int fmt, unsigned flags, uint32_t bs)
{
    char err[512];
    double last = 0, t0 = mono();
    if (mvm_image_convert(src, -1, dst, fmt, flags, bs, progress, &last, err, sizeof(err)) < 0) {
        fprintf(stderr, "\nmvm-img: %s\n", err);
        return 1;
    }
    mvm_image_info in, out;
    if (mvm_image_info_get(dst, -1, &out, err, sizeof(err)) == 0 && mvm_image_info_get(src, -1, &in, err, sizeof(err)) == 0)
        fprintf(stderr, "\r%s (%s, %.1f MiB) -> %s (%s, %.1f MiB) em %.1f s\n", src, in.format, in.file_size / 1048576.0,
                dst, out.format, out.file_size / 1048576.0, mono() - t0);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3)
        usage();
    const char *cmd = argv[1];
    int fmt = MVM_IMAGE_MVD;
    unsigned flags = 0;
    uint32_t bs = 0;
    int opt;
    optind = 2;
    while ((opt = getopt(argc, argv, "O:cb:")) != -1) {
        switch (opt) {
        case 'O':
            if (!strcasecmp(optarg, "mvd"))
                fmt = MVM_IMAGE_MVD;
            else if (!strcasecmp(optarg, "raw"))
                fmt = MVM_IMAGE_RAW;
            else {
                fprintf(stderr, "formato de saida '%s' nao suportado (mvd ou raw)\n", optarg);
                return 2;
            }
            break;
        case 'c': flags |= MVM_IMAGE_COMPRESS; break;
        case 'b': bs = (uint32_t)parse_size(optarg); break;
        default: usage();
        }
    }
    char **a = argv + optind;
    int n = argc - optind;
    char err[512];

    if (!strcmp(cmd, "info") && n == 1) {
        mvm_image_info i;
        if (mvm_image_info_get(a[0], -1, &i, err, sizeof(err)) < 0) {
            fprintf(stderr, "mvm-img: %s\n", err);
            return 1;
        }
        printf("formato: %s\ntamanho virtual: %llu (%.2f GiB)\narquivo: %llu (%.2f GiB)\ngravavel: %s\n", i.format,
               (unsigned long long)i.virtual_size, i.virtual_size / 1073741824.0, (unsigned long long)i.file_size,
               i.file_size / 1073741824.0, i.writable ? "sim" : "nao");
        return 0;
    }
    if (!strcmp(cmd, "create") && n == 2) {
        if (mvm_image_create(a[0], parse_size(a[1]), bs, NULL, err, sizeof(err)) < 0) {
            fprintf(stderr, "mvm-img: %s\n", err);
            return 1;
        }
        return 0;
    }
    if (!strcmp(cmd, "snapshot") && n == 2) {
        /* caminho absoluto do base: a imagem nova funciona de qualquer diretorio */
        char base[4096];
        if (!realpath(a[0], base)) {
            perror(a[0]);
            return 1;
        }
        if (mvm_image_create(a[1], 0, bs, base, err, sizeof(err)) < 0) {
            fprintf(stderr, "mvm-img: %s\n", err);
            return 1;
        }
        return 0;
    }
    if (!strcmp(cmd, "convert") && n == 2)
        return convert(a[0], a[1], fmt, flags, bs);
    if (!strcmp(cmd, "compact") && n == 1) {
        mvm_image_info i;
        if (mvm_image_info_get(a[0], -1, &i, err, sizeof(err)) < 0 || strcmp(i.format, "mvd")) {
            fprintf(stderr, "mvm-img: compact so vale para imagens mvd\n");
            return 1;
        }
        char tmp[4096];
        snprintf(tmp, sizeof(tmp), "%s.compact", a[0]);
        if (convert(a[0], tmp, MVM_IMAGE_MVD, flags, bs))
            return 1;
        if (rename(tmp, a[0]) < 0) {
            perror("rename");
            return 1;
        }
        return 0;
    }
    if (!strcmp(cmd, "check") && n == 1) {
        static char report[8192];
        double last = 0;
        int r = mvm_image_check(a[0], report, sizeof(report), progress, &last);
        if (isatty(2))
            fprintf(stderr, "\r");
        fputs(report, stdout);
        return r < 0;
    }
    usage();
}
