/*
 * mvm-cli: executa uma VM no terminal (console serial em stdin/stdout).
 * Uso: mvm-cli -a <arch> -k <kernel> [opcoes]   (Ctrl-A X sai)
 */
#define _GNU_SOURCE
#include "mvm.h"

#include <getopt.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <ucontext.h>

static mvm_vm *g_vm;
static struct termios g_old_tio;
static int g_raw;
static volatile int g_quit;
static double g_timeout;
static const char *g_expect;
static char g_tail[4096];
static size_t g_tail_len;
static int g_expect_hit;
static int g_viewer; /* --viewer: copia o framebuffer a 30 Hz como o app Android */
static double g_viewer_cpu;
static unsigned g_viewer_frames;

static void serial_out(void *opaque, const uint8_t *data, size_t len)
{
    (void)opaque;
    fwrite(data, 1, len, stdout);
    fflush(stdout);
    if (g_expect && !g_expect_hit) {
        for (size_t i = 0; i < len; i++) {
            if (g_tail_len == sizeof(g_tail) - 1) {
                memmove(g_tail, g_tail + 2048, g_tail_len - 2048);
                g_tail_len -= 2048;
            }
            g_tail[g_tail_len++] = (char)data[i];
        }
        g_tail[g_tail_len] = 0;
        if (strstr(g_tail, g_expect)) {
            g_expect_hit = 1;
            mvm_request_stop(g_vm);
        }
    }
}

static void restore_tty(void)
{
    if (g_raw)
        tcsetattr(0, TCSANOW, &g_old_tio);
}

static void *stdin_thread(void *arg)
{
    (void)arg;
    int ctrl_a = 0;
    while (!g_quit) {
        struct pollfd p = {.fd = 0, .events = POLLIN};
        if (poll(&p, 1, 100) <= 0)
            continue;
        uint8_t buf[256];
        ssize_t n = read(0, buf, sizeof(buf));
        if (n <= 0)
            break;
        for (ssize_t i = 0; i < n; i++) {
            if (g_raw && ctrl_a) {
                ctrl_a = 0;
                if (buf[i] == 'x' || buf[i] == 'X') {
                    mvm_request_stop(g_vm);
                    return NULL;
                }
                if (buf[i] == 'r') {
                    mvm_request_reset(g_vm);
                    continue;
                }
                if (buf[i] == 'p' || buf[i] == 'P') {
                    mvm_power_button(g_vm);
                    continue;
                }
                if (buf[i] != 1) continue;
            } else if (g_raw && buf[i] == 1) {
                ctrl_a = 1;
                continue;
            }
            uint8_t ch = buf[i];
            while (!mvm_serial_input(g_vm, &ch, 1) && !g_quit)
                usleep(1000);
        }
    }
    return NULL;
}

/* --type SEG:TEXTO: digita TEXTO no teclado emulado apos SEG segundos (\n = Enter) */
static double g_type_delay;
static const char *g_type_text;
static const char *g_type_wait; /* --type-after: espera este texto na tela VGA */

static int ascii_to_evdev(char ch, bool *shift)
{
    static const char *row1 = "1234567890-=", *row1s = "!@#$%^&*()_+";
    static const char *q = "qwertyuiop[]", *qs = "QWERTYUIOP{}";
    static const char *a = "asdfghjkl;'`", *as = "ASDFGHJKL:\"~";
    static const char *z = "\\zxcvbnm,./", *zs = "|ZXCVBNM<>?";
    *shift = false;
    const char *p;
    if (ch == '\n') return 28;
    if (ch == ' ') return 57;
    if (ch == '\t') return 15;
    if ((p = strchr(row1, ch)) && ch) return 2 + (int)(p - row1);
    if ((p = strchr(row1s, ch)) && ch) { *shift = true; return 2 + (int)(p - row1s); }
    if ((p = strchr(q, ch)) && ch) return 16 + (int)(p - q);
    if ((p = strchr(qs, ch)) && ch) { *shift = true; return 16 + (int)(p - qs); }
    if ((p = strchr(a, ch)) && ch) return 30 + (int)(p - a);
    if ((p = strchr(as, ch)) && ch) { *shift = true; return 30 + (int)(p - as); }
    if ((p = strchr(z, ch)) && ch) return 43 + (int)(p - z);
    if ((p = strchr(zs, ch)) && ch) { *shift = true; return 43 + (int)(p - zs); }
    return 0;
}

/* digita um texto com as sequencias de escape de --type */
static void type_seq(const char *text)
{
    for (const char *t = text; *t && !g_quit; t++) {
        char ch = *t;
        if (ch == '\\' && t[1] == 'w') { /* \\wN: espera N segundos */
            t += 2;
            int secs = 0;
            while (*t >= '0' && *t <= '9')
                secs = secs * 10 + (*t++ - '0');
            t--;
            for (int i = 0; i < secs * 10 && !g_quit; i++)
                usleep(100000);
            continue;
        }
        if (ch == '\\' && t[1] == 'S' && t[2]) { /* \\Sc: Alt+SysRq+c */
            bool sh;
            int code = ascii_to_evdev(t[2], &sh);
            t += 2;
            mvm_key_event(g_vm, 56, true);
            mvm_key_event(g_vm, 99, true);
            usleep(20000);
            if (code) { mvm_key_event(g_vm, (uint32_t)code, true); usleep(20000); mvm_key_event(g_vm, (uint32_t)code, false); }
            usleep(20000);
            mvm_key_event(g_vm, 99, false);
            mvm_key_event(g_vm, 56, false);
            usleep(30000);
            continue;
        }
        if (ch == '\\' && (t[1] == 'd' || t[1] == 'u') && t[2] >= '0' && t[2] <= '9') {
            /* \\dN / \\uN: segura / solta a tecla evdev N (ex.: \\d56\\k49\\u56 = Alt+N) */
            bool down = t[1] == 'd';
            t += 2;
            int code = 0;
            while (*t >= '0' && *t <= '9')
                code = code * 10 + (*t++ - '0');
            t--;
            mvm_key_event(g_vm, (uint32_t)code, down);
            usleep(30000);
            continue;
        }
        if (ch == '\\' && t[1] == 'k') { /* \\kN: tecla com codigo evdev N (ex.: 108 = seta p/ baixo) */
            t += 2;
            int code = 0;
            while (*t >= '0' && *t <= '9')
                code = code * 10 + (*t++ - '0');
            t--;
            mvm_key_event(g_vm, (uint32_t)code, true);
            usleep(20000);
            mvm_key_event(g_vm, (uint32_t)code, false);
            usleep(30000);
            continue;
        }
        if (ch == '\\' && t[1] == 'P') { /* \\P: botao de energia */
            t++;
            mvm_power_button(g_vm);
            continue;
        }
        if (ch == '\\' && t[1] == 'n') { ch = '\n'; t++; }
        bool shift;
        int code = ascii_to_evdev(ch, &shift);
        if (!code) continue;
        if (shift) mvm_key_event(g_vm, 42, true);
        mvm_key_event(g_vm, (uint32_t)code, true);
        usleep(20000);
        mvm_key_event(g_vm, (uint32_t)code, false);
        if (shift) mvm_key_event(g_vm, 42, false);
        usleep(30000);
    }
}

static bool write_ppm(const char *path)
{
    mvm_fb_info fi;
    if (!mvm_fb_info_get(g_vm, &fi))
        return false;
    /* sem espectador a VGA adia o desenho dos modos graficos: avisa e espera um quadro */
    mvm_fb_generation(g_vm);
    usleep(60000);
    size_t sz = (size_t)fi.width * fi.height * 4 + 16 * 1024 * 1024;
    uint8_t *px = malloc(sz);
    bool ok = false;
    if (px && mvm_fb_copy(g_vm, px, sz, &fi)) {
        FILE *f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%u %u\n255\n", fi.width, fi.height);
            for (size_t i = 0; i < (size_t)fi.width * fi.height; i++) {
                uint8_t rgb[3] = {px[i * 4 + 2], px[i * 4 + 1], px[i * 4]};
                fwrite(rgb, 1, 3, f);
            }
            fclose(f);
            ok = true;
        }
    }
    free(px);
    return ok;
}

/* --control FIFO: comandos durante a execucao, um por linha:
 *   type TEXTO | text ARQ | shot ARQ.ppm | power | reset | quit */
static const char *g_control;

static uint64_t *g_prof;
static volatile unsigned g_prof_n;
static double thread_cpu(void)
{
    struct rusage ru;
    getrusage(RUSAGE_THREAD, &ru);
    return (double)ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + (double)ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}

/* Simula a tela do app: a cada 33 ms, se o quadro mudou, copia para um "Bitmap" RGBA. */
static void *viewer_thread(void *arg)
{
    (void)arg;
    uint32_t last = 0;
    uint8_t *buf = NULL, *bmp = NULL;
    size_t cap = 0;
    while (!g_quit) {
        usleep(33000);
        uint32_t gen = mvm_fb_generation(g_vm);
        if (gen == last && buf)
            continue;
        mvm_fb_info fi;
        if (!mvm_fb_info_get(g_vm, &fi) || !fi.width || !fi.height)
            continue;
        size_t need = (size_t)fi.width * fi.height * 4;
        if (need > cap) {
            free(buf);
            free(bmp);
            buf = malloc(need);
            bmp = malloc(need);
            cap = need;
        }
        if (!getenv("MVM_VIEWER_OLD")) { /* caminho do app: so as linhas alteradas, direto no Bitmap */
            static uint32_t since, bw, bh;
            if (bw != fi.width || bh != fi.height) {
                bw = fi.width;
                bh = fi.height;
                since = 0;
            }
            if (mvm_fb_copy_rows(g_vm, bmp, bw, bh, (size_t)bw * 4, MVM_FB_COPY_RGBA, &since, NULL, NULL, NULL) > 0)
                g_viewer_frames++;
            if (getenv("MVM_VIEWER_CHECK")) { /* a copia parcial tem de bater com a completa */
                mvm_fb_info f2;
                if (mvm_fb_copy(g_vm, buf, cap, &f2) && f2.width == bw && f2.height == bh) {
                    unsigned bad = 0;
                    const uint32_t *full = (const uint32_t *)buf, *part = (const uint32_t *)bmp;
                    for (size_t i = 0; i < (size_t)bw * bh; i++) {
                        uint32_t p = full[i];
                        if (part[i] != (0xff000000u | (p & 0xff00u) | ((p >> 16) & 0xffu) | ((p & 0xffu) << 16)))
                            bad++;
                    }
                    static unsigned checks, fails;
                    checks++;
                    if (bad && mvm_fb_generation(g_vm) == since) /* sem quadro novo no meio */
                        fprintf(stderr, "[viewer] DIFERENCA: %u pixels (verificacao %u, falhas %u)\n", bad, checks, ++fails);
                }
            }
            last = gen;
            continue;
        }
        /* caminho antigo do app: copia + troca R/B + copyPixelsFromBuffer */
        if (!mvm_fb_copy(g_vm, buf, cap, &fi))
            continue;
        for (uint32_t i = 0; i < fi.width * fi.height; i++) {
            uint8_t b = buf[4 * i], r = buf[4 * i + 2];
            buf[4 * i] = r;
            buf[4 * i + 2] = b;
            buf[4 * i + 3] = 0xff;
        }
        memcpy(bmp, buf, (size_t)fi.width * fi.height * 4);
        last = gen;
        g_viewer_frames++;
    }
    g_viewer_cpu = thread_cpu();
    free(buf);
    free(bmp);
    return NULL;
}

static void *control_thread(void *arg)
{
    (void)arg;
    char line[4096];
    while (!g_quit) {
        FILE *f = fopen(g_control, "r");
        if (!f)
            break;
        while (!g_quit && fgets(line, sizeof(line), f)) {
            size_t n = strlen(line);
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
                line[--n] = 0;
            if (!strncmp(line, "type ", 5)) {
                type_seq(line + 5);
            } else if (!strncmp(line, "text ", 5)) {
                static char scr[16384];
                size_t k = mvm_text_screen(g_vm, scr, sizeof(scr) - 1);
                FILE *o = fopen(line + 5, "w");
                if (o) {
                    if (k)
                        fwrite(scr, 1, k, o);
                    else
                        fputs("(tela fora do modo texto)\n", o);
                    fclose(o);
                }
            } else if (!strcmp(line, "profreset")) { /* MVM_PROF: descarta as amostras ate aqui */
                g_prof_n = 0;
            } else if (!strncmp(line, "shot ", 5)) {
                write_ppm(line + 5);
            } else if (!strncmp(line, "mouse ", 6)) { /* mouse DX DY [BOTOES] (relativo, PS/2) */
                int dx = 0, dy = 0, bt = 0;
                sscanf(line + 6, "%d %d %d", &dx, &dy, &bt);
                /* passos pequenos: o PS/2 limita cada pacote a +-255 */
                while (dx || dy) {
                    int sx = dx > 100 ? 100 : dx < -100 ? -100 : dx;
                    int sy = dy > 100 ? 100 : dy < -100 ? -100 : dy;
                    mvm_pointer_event(g_vm, sx, sy, 0, (uint32_t)bt);
                    dx -= sx;
                    dy -= sy;
                    usleep(20000);
                }
                mvm_pointer_event(g_vm, 0, 0, 0, (uint32_t)bt);
            } else if (!strncmp(line, "click", 5)) { /* clique com o botao esquerdo */
                mvm_pointer_event(g_vm, 0, 0, 0, 1);
                usleep(80000);
                mvm_pointer_event(g_vm, 0, 0, 0, 0);
            } else if (!strncmp(line, "media", 5)) { /* media list|insert N ARQ|eject N|add ARQ */
                char err[256] = {0}, arg[3000] = {0};
                int slot = -1, r = 0;
                if (!strcmp(line, "media list")) {
                    mvm_media_info mi[MVM_MAX_MEDIA];
                    int n = mvm_media_list(g_vm, mi, MVM_MAX_MEDIA);
                    static const char *kinds[] = {"disco", "cd", "disquete"}, *buses[] = {"ide", "sata", "fdc", "virtio"};
                    for (int i = 0; i < n; i++)
                        fprintf(stderr, "[media] %d: %s %s:%d %s%s%s\n", i, kinds[mi[i].kind], buses[mi[i].bus], mi[i].unit,
                                mi[i].present ? mi[i].name : "(vazio)", mi[i].readonly ? " ro" : "",
                                mi[i].changeable ? "" : " (fixo)");
                } else if (sscanf(line, "media insert %d %2999s", &slot, arg) == 2) {
                    r = mvm_media_insert(g_vm, slot, arg, -1, false, NULL, err, sizeof(err));
                } else if (sscanf(line, "media eject %d", &slot) == 1) {
                    r = mvm_media_eject(g_vm, slot, err, sizeof(err));
                } else if (sscanf(line, "media add %2999s", arg) == 1) {
                    r = mvm_media_add_disk(g_vm, arg, -1, false, NULL, err, sizeof(err));
                    if (r >= 0)
                        fprintf(stderr, "[media] disco conectado no drive %d\n", r);
                }
                fprintf(stderr, "[media] %s%s\n", r < 0 ? "erro: " : "ok", r < 0 ? err : "");
            } else if (!strcmp(line, "power")) {
                mvm_power_button(g_vm);
            } else if (!strcmp(line, "reset")) {
                mvm_request_reset(g_vm);
            } else if (!strcmp(line, "quit")) {
                mvm_request_stop(g_vm);
            }
        }
        fclose(f);
    }
    return NULL;
}

static void *type_thread(void *arg)
{
    (void)arg;
    if (g_type_wait) {
        static char scr[16384];
        while (!g_quit) {
            size_t n = mvm_text_screen(g_vm, scr, sizeof(scr) - 1);
            scr[n] = 0;
            if (strstr(scr, g_type_wait))
                break;
            usleep(5000);
        }
    }
    usleep((useconds_t)(g_type_delay * 1e6));
    type_seq(g_type_text);
    return NULL;
}

/* MVM_PROF=arquivo: perfil por amostragem do host (PC a cada ~1 ms de CPU da thread
 * da vCPU); grava os PCs e /proc/self/maps ao final, para simbolizar com nm. */
#define PROF_MAX (1u << 22)
static void prof_handler(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)si;
#if defined(__aarch64__)
    uint64_t pc = ((ucontext_t *)uc)->uc_mcontext.pc;
#elif defined(__x86_64__)
    uint64_t pc = (uint64_t)((ucontext_t *)uc)->uc_mcontext.gregs[REG_RIP];
#else
    uint64_t pc = 0; (void)uc;
#endif
    if (g_prof_n < PROF_MAX)
        g_prof[g_prof_n++] = pc;
}

static void prof_write(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    for (unsigned i = 0; i < g_prof_n; i++)
        fprintf(f, "%llx\n", (unsigned long long)g_prof[i]);
    fprintf(f, "# maps\n");
    FILE *m = fopen("/proc/self/maps", "r");
    char line[512];
    while (m && fgets(line, sizeof(line), m))
        fputs(line, f);
    if (m)
        fclose(m);
    fclose(f);
    fprintf(stderr, "[mvm-cli] perfil: %u amostras em %s\n", g_prof_n, path);
}

/* sinais fatais: registra onde o host falhou e o estado do convidado antes de sair */
static void crash_handler(int sig, siginfo_t *si, void *uc)
{
#if defined(__aarch64__)
    uint64_t pc = ((ucontext_t *)uc)->uc_mcontext.pc;
#elif defined(__x86_64__)
    uint64_t pc = (uint64_t)((ucontext_t *)uc)->uc_mcontext.gregs[REG_RIP];
#else
    uint64_t pc = 0; (void)uc;
#endif
    fprintf(stderr, "\n[mvm-cli] sinal %d (%s) no PC do host 0x%llx, endereco 0x%llx\n", sig, strsignal(sig),
            (unsigned long long)pc, (unsigned long long)(uintptr_t)si->si_addr);
    FILE *m = fopen("/proc/self/maps", "r");
    char line[512];
    while (m && fgets(line, sizeof(line), m)) {
        unsigned long long a, b;
        if (sscanf(line, "%llx-%llx", &a, &b) == 2 && pc >= a && pc < b)
            fprintf(stderr, "[mvm-cli] regiao: %s", line);
    }
    if (m)
        fclose(m);
    if (g_vm)
        mvm_debug_dump(g_vm);
    restore_tty();
    _exit(128 + sig);
}

static void install_crash_handler(void)
{
    static uint8_t altstack[65536];
    stack_t ss = {.ss_sp = altstack, .ss_size = sizeof(altstack), .ss_flags = 0};
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
}

static void *timeout_thread(void *arg)
{
    (void)arg;
    struct timespec ts = {.tv_sec = (time_t)g_timeout, .tv_nsec = (long)((g_timeout - (time_t)g_timeout) * 1e9)};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) /* sinais (ex.: SIGPROF) interrompem */
        ;
    if (!g_quit) {
        fprintf(stderr, "\n[mvm-cli] tempo limite atingido\n");
        mvm_request_stop(g_vm);
    }
    return NULL;
}

/* ---- audio: gravador WAV (saida) e WAV como microfone ---- */
static FILE *g_wav;
static uint64_t g_wav_frames;
static const char *g_audio_in;

static void wav_header(FILE *f, uint64_t frames)
{
    uint32_t data = (uint32_t)(frames * 4);
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    uint32_t v = 36 + data;
    memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v = 16; memcpy(h + 16, &v, 4);
    uint16_t w = 1; memcpy(h + 20, &w, 2);      /* PCM */
    w = 2; memcpy(h + 22, &w, 2);               /* estereo */
    v = MVM_AUDIO_RATE; memcpy(h + 24, &v, 4);
    v = MVM_AUDIO_RATE * 4; memcpy(h + 28, &v, 4);
    w = 4; memcpy(h + 32, &w, 2);
    w = 16; memcpy(h + 34, &w, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data, 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), f);
    fseek(f, 0, SEEK_END);
}

static void wav_push(void *opaque, const int16_t *frames, size_t n)
{
    (void)opaque;
    fwrite(frames, 4, n, g_wav);
    g_wav_frames += n;
}

static void audio_nop(void *opaque, bool input) { (void)opaque; (void)input; }
static const mvm_audio_backend wav_backend = {audio_nop, audio_nop, wav_push};

/* entrega o WAV (48 kHz, estereo, 16 bits) como microfone em tempo real, em laco */
static void *audio_in_thread(void *arg)
{
    (void)arg;
    FILE *f = fopen(g_audio_in, "rb");
    if (!f) {
        fprintf(stderr, "--audio-in: nao consegui abrir %s\n", g_audio_in);
        return NULL;
    }
    int16_t buf[480 * 2];
    while (!g_quit) {
        size_t n = fread(buf, 4, 480, f);
        if (n < 480) {
            fseek(f, 44, SEEK_SET);
            if (!n)
                continue;
        }
        mvm_audio_write_input(g_vm, buf, n);
        struct timespec ts = {0, 10000000}; /* 10 ms */
        nanosleep(&ts, NULL);
    }
    fclose(f);
    return NULL;
}

static int g_vnc_port;
static char g_vnc_addr[64] = "127.0.0.1";
static const char *g_vnc_password;

static void usage(void)
{
    fprintf(stderr,
            "MultiVM %s\n"
            "uso: mvm-cli -a ARCH [opcoes]\n"
            "  -a, --arch ARCH       i386 | x86_64 | arm | arm64\n"
            "  -m, --memory MB       memoria (padrao 256)\n"
            "  -k, --kernel ARQ      kernel Linux / ELF / binario bruto\n"
            "  -i, --initrd ARQ      initrd\n"
            "  -c, --cmdline TXT     linha de comando do kernel\n"
            "  -d, --disk ARQ        disco (use varias vezes; ate 10 midias no total)\n"
            "  -r, --readonly ARQ    disco somente leitura\n"
            "      --cdrom ARQ       imagem ISO em CD-ROM IDE (x86; use varias vezes; 'none' = drive vazio)\n"
            "      --fda ARQ         imagem de disquete (x86; a segunda vez vira B:)\n"
            "      --fda-ro ARQ      disquete protegido contra escrita\n"
            "      --net MODELO      rede NAT (10.0.2.15): auto | rtl8139 | e1000 | virtio | none\n"
            "      --hostfwd REGRA   redireciona porta do host: [tcp|udp]:[lan:]PORTA_HOST-:PORTA_CONVIDADO\n"
            "      --dns IP          servidor DNS do host repassado em 10.0.2.3\n"
            "      --audio MODELO    placa de som: auto | ac97 | hda | virtio | none\n"
            "      --audio-wav ARQ   grava a saida de som num WAV (48 kHz, estereo)\n"
            "      --audio-in ARQ    WAV (48 kHz, estereo, 16 bits) como microfone, em laco\n"
            "      --vnc [ADDR:]N    servidor VNC na porta 5900+N (padrao so 127.0.0.1; ex.: 0.0.0.0:1)\n"
            "      --vnc-password S  senha do VNC (ate 8 caracteres)\n"
            "      --ide             discos -d/-r no controlador IDE (padrao com --bios)\n"
            "      --boot ORDEM      ordem de boot com BIOS: c=disco d=CD a=disquete (ex.: dca)\n"
            "      --vgabios ARQ     ROM de video (padrao: vgabios-stdvga.bin ao lado da BIOS)\n"
            "      --text            imprime a tela em modo texto ao sair\n"
            "      --type SEG:TEXTO  digita TEXTO no teclado apos SEG segundos (\\n = Enter, \\wN = espera N s, \\Sc = Alt+SysRq+c, \\P = botao de energia, \\kN = tecla evdev N)\n"
            "      --sata            discos (-d/-r) e CD-ROMs (--cdrom) no controlador SATA AHCI\n      --no-reboot       encerra quando o convidado reinicia (codigo de saida 3)\n      --rtc DATA        data inicial do relogio (AAAA-MM-DD[THH:MM:SS], UTC); padrao: a do host\n"
            "      --control FIFO    comandos em tempo de execucao: type TXT, text ARQ, shot ARQ, mouse DX DY [B], click, power, reset, quit,\n"
            "                        media list | media insert N ARQ | media eject N | media add ARQ\n"
            "      --type-after TXT  so comeca a contar SEG depois que TXT aparecer na tela de texto\n"
            "      --dtb ARQ         device tree externo (ARM)\n"
            "      --bios ARQ        firmware (x86)\n"
            "      --load-addr HEX   endereco de carga para binario bruto\n"
            "      --fb WxH          framebuffer\n"
            "      --fb-dump ARQ     grava o framebuffer (PPM) ao sair\n"
            "      --viewer          copia o framebuffer a 30 Hz como a tela do app (medicao de CPU)\n"
            "  -t, --timeout SEG     encerra apos SEG segundos\n"
            "  -e, --expect TXT      encerra com sucesso quando TXT aparecer no console\n"
            "  -v, --verbose         log detalhado\n"
            "Ctrl-A X sai, Ctrl-A R reinicia, Ctrl-A P aperta o botao de energia (ACPI).\n",
            MVM_VERSION);
}

int main(int argc, char **argv)
{
    mvm_config cfg;
    mvm_config_init(&cfg, MVM_ARCH_ARM64);
    int arch = -1, ndisk = 0, use_ide = 0, use_sata = 0, dump_text = 0;
    const char *fb_dump = NULL;
    static struct option opts[] = {
        {"arch", required_argument, 0, 'a'}, {"memory", required_argument, 0, 'm'},
        {"kernel", required_argument, 0, 'k'}, {"initrd", required_argument, 0, 'i'},
        {"cmdline", required_argument, 0, 'c'}, {"disk", required_argument, 0, 'd'},
        {"readonly", required_argument, 0, 'r'}, {"dtb", required_argument, 0, 1},
        {"cdrom", required_argument, 0, 6}, {"ide", no_argument, 0, 7}, {"sata", no_argument, 0, 14}, {"no-reboot", no_argument, 0, 15}, {"rtc", required_argument, 0, 16}, {"boot", required_argument, 0, 8}, {"fda", required_argument, 0, 17}, {"fda-ro", required_argument, 0, 18},
        {"net", required_argument, 0, 19}, {"hostfwd", required_argument, 0, 20}, {"dns", required_argument, 0, 21},
        {"audio", required_argument, 0, 22}, {"audio-wav", required_argument, 0, 23}, {"audio-in", required_argument, 0, 24},
        {"vnc", required_argument, 0, 25}, {"vnc-password", required_argument, 0, 26},
        {"vgabios", required_argument, 0, 9}, {"text", no_argument, 0, 10}, {"type", required_argument, 0, 11}, {"type-after", required_argument, 0, 12}, {"control", required_argument, 0, 13},
        {"bios", required_argument, 0, 2}, {"load-addr", required_argument, 0, 3},
        {"fb", required_argument, 0, 4}, {"fb-dump", required_argument, 0, 5}, {"viewer", no_argument, 0, 27},
        {"timeout", required_argument, 0, 't'}, {"expect", required_argument, 0, 'e'},
        {"verbose", no_argument, 0, 'v'}, {"help", no_argument, 0, 'h'}, {0, 0, 0, 0}};
    int o, no_reboot = 0;
    while ((o = getopt_long(argc, argv, "a:m:k:i:c:d:r:t:e:vh", opts, NULL)) != -1) {
        switch (o) {
        case 'a':
            arch = mvm_arch_from_name(optarg);
            if (arch < 0) {
                fprintf(stderr, "arquitetura desconhecida: %s\n", optarg);
                return 2;
            }
            break;
        case 'm': cfg.ram_mb = (uint32_t)atoi(optarg); break;
        case 'k': cfg.kernel = optarg; break;
        case 'i': cfg.initrd = optarg; break;
        case 'c': cfg.cmdline = optarg; break;
        case 'd':
        case 'r':
            if (ndisk < MVM_MAX_DISKS) {
                cfg.disks[ndisk].path = optarg;
                cfg.disks[ndisk].readonly = o == 'r';
                ndisk++;
            }
            break;
        case 1: cfg.dtb = optarg; break;
        case 2: cfg.firmware = optarg; break;
        case 3: cfg.raw_load_addr = strtoull(optarg, NULL, 16); break;
        case 4:
            if (sscanf(optarg, "%ux%u", &cfg.fb_width, &cfg.fb_height) != 2) {
                fprintf(stderr, "--fb espera LxA\n");
                return 2;
            }
            break;
        case 5: fb_dump = optarg; break;
        case 6:
            if (ndisk < MVM_MAX_DISKS) {
                cfg.disks[ndisk].path = strcmp(optarg, "none") ? optarg : NULL;
                cfg.disks[ndisk].empty = !strcmp(optarg, "none"); /* drive sem midia */
                cfg.disks[ndisk].readonly = true;
                cfg.disks[ndisk].type = MVM_DISK_CDROM;
                ndisk++;
            }
            break;
        case 17:
        case 18:
            if (ndisk < MVM_MAX_DISKS) {
                cfg.disks[ndisk].path = strcmp(optarg, "none") ? optarg : NULL;
                cfg.disks[ndisk].empty = !strcmp(optarg, "none");
                cfg.disks[ndisk].readonly = o == 18;
                cfg.disks[ndisk].type = MVM_DISK_FLOPPY;
                ndisk++;
            }
            break;
        case 19: {
            static const char *names[] = {"none", "auto", "rtl8139", "e1000", "virtio"};
            int k = -1;
            for (int i = 0; i < 5; i++)
                if (!strcmp(optarg, names[i]))
                    k = i;
            if (k < 0) {
                fprintf(stderr, "--net: modelo desconhecido: %s\n", optarg);
                return 2;
            }
            cfg.net.model = (mvm_nic_model)k;
            break;
        }
        case 20: {
            if (cfg.net.nforwards >= MVM_MAX_FORWARDS)
                break;
            mvm_port_forward *fw = &cfg.net.forwards[cfg.net.nforwards];
            const char *p = optarg;
            if (!strncmp(p, "udp:", 4)) { fw->udp = true; p += 4; }
            else if (!strncmp(p, "tcp:", 4)) p += 4;
            if (!strncmp(p, "lan:", 4)) { fw->lan = true; p += 4; }
            unsigned hp, gp;
            if (sscanf(p, "%u-:%u", &hp, &gp) != 2 || !hp || hp > 65535 || !gp || gp > 65535) {
                fprintf(stderr, "--hostfwd espera [tcp|udp]:[lan:]PORTA_HOST-:PORTA_CONVIDADO\n");
                return 2;
            }
            fw->host_port = (uint16_t)hp;
            fw->guest_port = (uint16_t)gp;
            cfg.net.nforwards++;
            if (cfg.net.model == MVM_NIC_NONE)
                cfg.net.model = MVM_NIC_AUTO;
            break;
        }
        case 21: cfg.net.dns = optarg; break;
        case 22: {
            static const char *names[] = {"none", "auto", "ac97", "hda", "virtio"};
            int k = -1;
            for (int i = 0; i < 5; i++)
                if (!strcmp(optarg, names[i]))
                    k = i;
            if (k < 0) {
                fprintf(stderr, "--audio: modelo desconhecido: %s\n", optarg);
                return 2;
            }
            cfg.audio.model = (mvm_snd_model)k;
            break;
        }
        case 23:
            g_wav = fopen(optarg, "wb");
            if (!g_wav) {
                fprintf(stderr, "--audio-wav: nao consegui criar %s\n", optarg);
                return 2;
            }
            wav_header(g_wav, 0);
            if (cfg.audio.model == MVM_SND_NONE)
                cfg.audio.model = MVM_SND_AUTO;
            break;
        case 25: {
            const char *colon = strrchr(optarg, ':');
            g_vnc_port = 5900 + atoi(colon ? colon + 1 : optarg);
            if (colon) {
                snprintf(g_vnc_addr, sizeof(g_vnc_addr), "%.*s", (int)(colon - optarg), optarg);
            }
            break;
        }
        case 26: g_vnc_password = optarg; break;
        case 27: g_viewer = 1; break;
        case 24:
            g_audio_in = optarg;
            cfg.audio.mic = true;
            if (cfg.audio.model == MVM_SND_NONE)
                cfg.audio.model = MVM_SND_AUTO;
            break;
        case 7: use_ide = 1; break;
        case 15: no_reboot = 1; break;
        case 16: {
            struct tm tm;
            memset(&tm, 0, sizeof(tm));
            if (!strptime(optarg, "%Y-%m-%dT%H:%M:%S", &tm) && !strptime(optarg, "%Y-%m-%d", &tm)) {
                fprintf(stderr, "--rtc espera AAAA-MM-DD[THH:MM:SS]\n");
                return 2;
            }
            cfg.rtc_base = (int64_t)timegm(&tm);
            break;
        }
        case 8: cfg.boot_order = optarg; break;
        case 9: cfg.vga_bios = optarg; break;
        case 10: dump_text = 1; break;
        case 11: {
            char *c = strchr(optarg, ':');
            if (!c) { fprintf(stderr, "--type espera SEG:TEXTO\n"); return 2; }
            *c = 0;
            g_type_delay = atof(optarg);
            g_type_text = c + 1;
            break;
        }
        case 12: g_type_wait = optarg; break;
        case 13: g_control = optarg; break;
        case 14: use_sata = 1; break;
        case 't': g_timeout = atof(optarg); break;
        case 'e': g_expect = optarg; break;
        case 'v': mvm_set_log_level(MVM_LOG_DEBUG); break;
        default: usage(); return o == 'h' ? 0 : 2;
        }
    }
    if (arch < 0) {
        usage();
        return 2;
    }
    cfg.arch = (mvm_arch)arch;
    if (use_ide)
        for (int i = 0; i < ndisk; i++)
            if (cfg.disks[i].type == MVM_DISK_AUTO)
                cfg.disks[i].type = MVM_DISK_IDE;
    if (use_sata) /* discos e CD-ROMs no controlador AHCI */
        for (int i = 0; i < ndisk; i++) {
            if (cfg.disks[i].type == MVM_DISK_AUTO || cfg.disks[i].type == MVM_DISK_IDE)
                cfg.disks[i].type = MVM_DISK_SATA;
            else if (cfg.disks[i].type == MVM_DISK_CDROM)
                cfg.disks[i].type = MVM_DISK_SATA_CDROM;
        }

    char err[512];
    g_vm = mvm_create(&cfg, err, sizeof(err));
    if (g_vm && no_reboot)
        mvm_set_no_reboot(g_vm, true);
    if (g_vm)
        install_crash_handler();
    if (!g_vm) {
        fprintf(stderr, "erro: %s\n", err);
        return 1;
    }
    mvm_set_serial_output(g_vm, serial_out, NULL);
    if (g_wav)
        mvm_audio_set_backend(g_vm, &wav_backend, NULL);
    mvm_vnc *vnc = NULL;
    if (g_vnc_port) {
        vnc = mvm_vnc_start(g_vm, g_vnc_addr[0] ? g_vnc_addr : "127.0.0.1", g_vnc_port, g_vnc_password, err, sizeof(err));
        if (!vnc)
            fprintf(stderr, "aviso: %s\n", err);
    }
    pthread_t ain_th;
    if (g_audio_in)
        pthread_create(&ain_th, NULL, audio_in_thread, NULL);

    if (isatty(0) && !g_expect) {
        tcgetattr(0, &g_old_tio);
        struct termios t = g_old_tio;
        cfmakeraw(&t);
        t.c_oflag |= OPOST | ONLCR;
        tcsetattr(0, TCSANOW, &t);
        g_raw = 1;
        atexit(restore_tty);
    }
    const char *prof = getenv("MVM_PROF");
    sigset_t profset;
    sigemptyset(&profset);
    sigaddset(&profset, SIGPROF);
    if (prof) { /* so a thread principal (vCPU) recebe SIGPROF */
        g_prof = calloc(PROF_MAX, sizeof(*g_prof));
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = prof_handler;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigaction(SIGPROF, &sa, NULL);
        pthread_sigmask(SIG_BLOCK, &profset, NULL);
    }
    pthread_t in_th, to_th;
    pthread_create(&in_th, NULL, stdin_thread, NULL);
    if (g_timeout > 0)
        pthread_create(&to_th, NULL, timeout_thread, NULL);
    pthread_t ty_th;
    if (g_type_text)
        pthread_create(&ty_th, NULL, type_thread, NULL);
    pthread_t ctl_th;
    if (g_control)
        pthread_create(&ctl_th, NULL, control_thread, NULL);

    pthread_t vw_th;
    if (g_viewer)
        pthread_create(&vw_th, NULL, viewer_thread, NULL);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (prof) {
        pthread_sigmask(SIG_UNBLOCK, &profset, NULL);
        struct itimerval it = {{0, 1000}, {0, 1000}};
        setitimer(ITIMER_PROF, &it, NULL);
    }
    int r = mvm_run(g_vm);
    if (prof) {
        struct itimerval it0 = {{0, 0}, {0, 0}};
        setitimer(ITIMER_PROF, &it0, NULL);
        prof_write(prof);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double vm_cpu = thread_cpu();
    g_quit = 1;
    if (g_viewer)
        pthread_join(vw_th, NULL);
    restore_tty();
    g_raw = 0;
    double secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    uint64_t insns = mvm_instruction_count(g_vm);
    fprintf(stderr, "\n[mvm-cli] fim (%s): %llu instrucoes em %.2fs (%.1f MIPS)\n",
            r == MVM_EXIT_SHUTDOWN ? "desligado" : r == MVM_EXIT_ERROR ? "erro" : r == MVM_EXIT_REBOOT ? "reinicio" : "parado",
            (unsigned long long)insns, secs, secs > 0 ? (double)insns / secs / 1e6 : 0.0);
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    double all_cpu = (double)ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + (double)ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
    fprintf(stderr, "[mvm-cli] CPU: total %.2fs (%.0f%%), thread da VM %.2fs (%.0f%%)", all_cpu,
            secs > 0 ? 100.0 * all_cpu / secs : 0.0, vm_cpu, secs > 0 ? 100.0 * vm_cpu / secs : 0.0);
    if (g_viewer)
        fprintf(stderr, ", visualizador %.2fs (%u quadros)", g_viewer_cpu, g_viewer_frames);
    fputc('\n', stderr);
    double lag = (double)mvm_clock_lag_ns(g_vm) / 1e9;
    if (lag > 0.001)
        fprintf(stderr, "[mvm-cli] relogio do convidado freado em %.2fs (%.0f%% do tempo)\n", lag,
                secs > 0 ? 100.0 * lag / secs : 0.0);

    if (dump_text) {
        char txt[8192];
        if (mvm_text_screen(g_vm, txt, sizeof(txt)))
            fprintf(stderr, "[mvm-cli] tela:\n%s", txt);
        else
            fprintf(stderr, "[mvm-cli] tela fora do modo texto\n");
    }
    if (fb_dump && write_ppm(fb_dump))
        fprintf(stderr, "[mvm-cli] framebuffer gravado em %s\n", fb_dump);
    if (g_audio_in)
        pthread_join(ain_th, NULL);
    mvm_vnc_stop(vnc);
    mvm_destroy(g_vm);
    if (g_wav) {
        wav_header(g_wav, g_wav_frames);
        fclose(g_wav);
        fprintf(stderr, "[mvm-cli] audio: %.2f s gravados\n", (double)g_wav_frames / MVM_AUDIO_RATE);
    }
    pthread_join(in_th, NULL);
    if (g_expect)
        return g_expect_hit ? 0 : 1;
    return r == MVM_EXIT_ERROR ? 1 : 0;
}
