/* Log, console serial, discos e temporizadores. */
#define _GNU_SOURCE
#include <stdatomic.h>
#include "internal.h"
#include "img/img.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <android/log.h>
#endif

/* ------------------------------------------------------------------ log */

mvm_log_level mvm_log_threshold = MVM_LOG_INFO;
static mvm_log_cb log_cb;
static void *log_opaque;

void mvm_set_log_callback(mvm_log_cb cb, void *opaque)
{
    log_cb = cb;
    log_opaque = opaque;
}

void mvm_set_log_level(mvm_log_level level) { mvm_log_threshold = level; }

void mvm_log(mvm_log_level level, const char *fmt, ...)
{
    if (level > mvm_log_threshold)
        return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (log_cb) {
        log_cb(log_opaque, level, buf);
        return;
    }
#ifdef __ANDROID__
    static const int prio[] = {ANDROID_LOG_ERROR, ANDROID_LOG_WARN, ANDROID_LOG_INFO, ANDROID_LOG_DEBUG};
    __android_log_write(prio[level], "MultiVM", buf);
#else
    static const char *tag[] = {"E", "W", "I", "D"};
    fprintf(stderr, "[mvm %s] %s\n", tag[level], buf);
#endif
}

/* ------------------------------------------------------------ chardev */

void chr_init(mvm_chardev *c)
{
    memset(c, 0, sizeof(*c));
    pthread_mutex_init(&c->lock, NULL);
}

void chr_destroy(mvm_chardev *c) { pthread_mutex_destroy(&c->lock); }

void chr_write(mvm_chardev *c, const uint8_t *buf, size_t len)
{
    if (c->out_cb)
        c->out_cb(c->out_opaque, buf, len);
}

size_t chr_push_input(mvm_chardev *c, const uint8_t *buf, size_t len)
{
    size_t n = 0;
    pthread_mutex_lock(&c->lock);
    while (n < len) {
        unsigned next = (c->in_head + 1) % CHR_IN_SIZE;
        if (next == c->in_tail)
            break;
        c->in[c->in_head] = buf[n++];
        c->in_head = next;
    }
    pthread_mutex_unlock(&c->lock);
    return n;
}

int chr_read_byte(mvm_chardev *c)
{
    int v = -1;
    pthread_mutex_lock(&c->lock);
    if (c->in_tail != c->in_head) {
        v = c->in[c->in_tail];
        c->in_tail = (c->in_tail + 1) % CHR_IN_SIZE;
    }
    pthread_mutex_unlock(&c->lock);
    return v;
}

bool chr_has_input(mvm_chardev *c)
{
    pthread_mutex_lock(&c->lock);
    bool r = c->in_tail != c->in_head;
    pthread_mutex_unlock(&c->lock);
    return r;
}

/* --------------------------------------------------------------- disco */

static const blk_driver *const blk_drivers[] = {&img_mvd, &img_qcow2, &img_vdi, &img_vmdk, &img_vhdx, &img_vhd};

/* profundidade de arquivos base abertos (evita cadeias circulares) */
static __thread int blk_depth;

/* detecta o formato e completa o mvm_blk; libera tudo em erro */
static mvm_blk *blk_setup(mvm_blk *b, const char *path, char *err, size_t errlen)
{
    static __thread uint8_t head[4096];
    uint8_t tail[512];
    img_probe_info pi = {head, sizeof(head), tail, b->size};
    if (img_pread(b->fd, head, sizeof(head), 0) < 0)
        memset(head, 0, sizeof(head));
    memset(tail, 0, sizeof(tail));
    if (b->size >= 1024)
        img_pread(b->fd, tail, sizeof(tail), b->size - 512);
    b->can_discard = false;
#if defined(FALLOC_FL_PUNCH_HOLE) && defined(FALLOC_FL_KEEP_SIZE)
    /* raw: TRIM so se o sistema de arquivos abre buracos (no FUSE do /sdcard costuma nao abrir) */
    if (!b->readonly)
        b->can_discard = fallocate(b->fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, (off_t)b->size, 4096) == 0;
#endif
    for (size_t i = 0; i < sizeof(blk_drivers) / sizeof(blk_drivers[0]); i++) {
        const blk_driver *d = blk_drivers[i];
        if (!d->probe(&pi))
            continue;
        if (blk_depth >= 8) {
            snprintf(err, errlen, "cadeia de arquivos base longa demais (ou circular)");
            goto fail;
        }
        img_open_args a = {.fd = b->fd, .path = path, .readonly = b->readonly, .fsize = b->size};
        blk_depth++;
        b->st = d->open(&a, err, errlen);
        blk_depth--;
        if (!b->st)
            goto fail;
        b->drv = d;
        b->readonly = a.readonly;
        b->size = a.vsize;
        b->can_discard = a.can_discard && !a.readonly && d->discard;
        break;
    }
    return b;
fail:
    close(b->fd);
    free(b);
    return NULL;
}

mvm_blk *blk_open(const char *path, bool readonly, char *err, size_t errlen)
{
    int fd = open(path, readonly ? O_RDONLY : O_RDWR);
    if (fd < 0 && !readonly && (errno == EACCES || errno == EROFS)) {
        fd = open(path, O_RDONLY);
        readonly = true;
    }
    if (fd < 0) {
        snprintf(err, errlen, "nao foi possivel abrir disco '%s': %s", path, strerror(errno));
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) < 0) {
        snprintf(err, errlen, "fstat '%s': %s", path, strerror(errno));
        close(fd);
        return NULL;
    }
    mvm_blk *b = calloc(1, sizeof(*b));
    b->fd = fd;
    b->size = (uint64_t)st.st_size;
    if (S_ISBLK(st.st_mode)) {
        off_t end = lseek(fd, 0, SEEK_END);
        if (end > 0)
            b->size = (uint64_t)end;
    }
    b->readonly = readonly;
    return blk_setup(b, path, err, errlen);
}

mvm_blk *blk_open_fd(int fd, bool readonly, char *err, size_t errlen)
{
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0) {
        snprintf(err, errlen, "descritor de disco invalido (%d)", fd);
        return NULL;
    }
    if (!readonly) {
        int fl = fcntl(fd, F_GETFL);
        if (fl >= 0 && (fl & O_ACCMODE) == O_RDONLY)
            readonly = true;
    }
    mvm_blk *b = calloc(1, sizeof(*b));
    b->fd = fd;
    b->size = (uint64_t)st.st_size;
    if (S_ISBLK(st.st_mode)) {
        off_t end = lseek(fd, 0, SEEK_END);
        if (end > 0)
            b->size = (uint64_t)end;
    }
    b->readonly = readonly;
    return blk_setup(b, NULL, err, errlen);
}

void blk_close(mvm_blk *b)
{
    if (!b)
        return;
    if (b->drv)
        b->drv->close(b->st);
    close(b->fd);
    free(b);
}

const char *blk_format(const mvm_blk *b) { return b->drv ? b->drv->name : "raw"; }

int blk_read(mvm_blk *b, uint64_t off, void *buf, size_t len)
{
    if (b->drv)
        return b->drv->read(b->st, off, buf, len);
    return img_pread(b->fd, buf, len, off);
}

/*
 * Imagem raw: o convidado nao pode gravar no inicio (ou no ultimo setor) algo que
 * faria a imagem ser reconhecida como outro formato na proxima abertura -- uma
 * imagem qcow2/VMDK/VHD forjada poderia apontar um "arquivo base" para qualquer
 * arquivo do host. (Mesma protecao do QEMU para imagens raw detectadas.)
 */
static bool raw_write_forbidden(mvm_blk *b, uint64_t off, const uint8_t *p, size_t len)
{
    bool hit = false;
    if (off < 4096) {
        uint8_t head[4096];
        if (img_pread(b->fd, head, sizeof(head), 0) < 0)
            return true;
        size_t n = len < 4096 - off ? len : (size_t)(4096 - off);
        memcpy(head + off, p, n);
        img_probe_info pi = {head, sizeof(head), (const uint8_t *)"", 0};
        for (size_t i = 0; i < sizeof(blk_drivers) / sizeof(blk_drivers[0]) && !hit; i++)
            if (blk_drivers[i] != &img_vhd)
                hit = blk_drivers[i]->probe(&pi);
    }
    if (!hit && b->size >= 1024 && off + len > b->size - 512) {
        uint8_t tail[512];
        uint64_t t0 = b->size - 512;
        if (img_pread(b->fd, tail, 512, t0) < 0)
            return true;
        for (uint64_t i = off > t0 ? off : t0; i < off + len && i < b->size; i++)
            tail[i - t0] = p[i - off];
        static const uint8_t z[4096];
        img_probe_info pi = {z, sizeof(z), tail, b->size};
        hit = img_vhd.probe(&pi);
    }
    if (hit)
        LOGW("disco raw: gravacao de assinatura de formato de imagem bloqueada (offset %llu)",
             (unsigned long long)off);
    return hit;
}

int blk_write(mvm_blk *b, uint64_t off, const void *buf, size_t len)
{
    if (b->readonly)
        return -1;
    if (b->drv)
        return b->drv->write(b->st, off, buf, len);
    if ((off < 4096 || (b->size >= 1024 && off + len > b->size - 512)) && raw_write_forbidden(b, off, buf, len)) {
        errno = EPERM;
        return -1;
    }
    return img_pwrite(b->fd, buf, len, off);
}

int blk_flush(mvm_blk *b)
{
    if (b->drv)
        return b->drv->flush(b->st);
    return b->readonly ? 0 : fdatasync(b->fd);
}

int blk_discard(mvm_blk *b, uint64_t off, uint64_t len)
{
    if (b->readonly)
        return -1;
    if (off >= b->size)
        return 0;
    if (len > b->size - off)
        len = b->size - off;
    if (b->drv && b->drv->discard)
        return b->drv->discard(b->st, off, len);
    static const uint8_t zero[65536];
    if (!b->drv) {
#if defined(FALLOC_FL_PUNCH_HOLE) && defined(FALLOC_FL_KEEP_SIZE)
        if (fallocate(b->fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, (off_t)off, (off_t)len) == 0)
            return 0;
#endif
    }
    /* sem suporte: grava zeros (o conteudo lido depois tem de ser zero) */
    while (len) {
        size_t n = len < sizeof(zero) ? (size_t)len : sizeof(zero);
        if (blk_write(b, off, zero, n) < 0)
            return -1;
        off += n;
        len -= n;
    }
    return 0;
}

/* ------------------------------------------------------ temporizadores */

static int64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

#if defined(__aarch64__)
/* No AArch64 o contador generico (CNTVCT_EL0) e lido em modo usuario por ~20 ns;
 * clock_gettime chega a 300 ns em alguns aparelhos. Mas o contador continua contando
 * com o aparelho suspenso e o CLOCK_MONOTONIC nao: entre duas leituras proximas usa-se
 * o contador; se elas estiverem a mais de 50 ms (ociosidade ou suspensao), o deslocamento
 * e recalculado pelo CLOCK_MONOTONIC. Assim o relogio segue o MONOTONIC (sem saltar
 * apos uma suspensao do host, o que confundia o convidado) e nunca anda para tras.
 * MVM_HOST_CLOCK=gettime forca clock_gettime. */
static int clk_mode; /* 0 = nao iniciado, 1 = contador, 2 = clock_gettime */
static uint64_t clk_mult; /* ns = ticks * clk_mult >> 32 */
static _Atomic int64_t clk_off, clk_last;

static inline int64_t cnt_ns(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
    return (int64_t)(((unsigned __int128)v * clk_mult) >> 32);
}

static int64_t host_ns(void)
{
    if (__builtin_expect(clk_mode == 1, 1)) {
        int64_t raw = cnt_ns();
        int64_t last = atomic_load_explicit(&clk_last, memory_order_relaxed);
        atomic_store_explicit(&clk_last, raw, memory_order_relaxed);
        if (raw - last > 50000000LL || raw < last) {
            int64_t off = mono_ns() - raw;
            int64_t prev = atomic_load_explicit(&clk_off, memory_order_relaxed);
            if (raw + off < last + prev) /* nao volta no tempo */
                off = last + prev - raw;
            atomic_store_explicit(&clk_off, off, memory_order_relaxed);
            return raw + off;
        }
        return raw + atomic_load_explicit(&clk_off, memory_order_relaxed);
    }
    if (clk_mode == 0) {
        uint64_t frq;
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
        const char *e = getenv("MVM_HOST_CLOCK");
        if (frq < 1000000 || (e && !strcmp(e, "gettime"))) {
            clk_mode = 2;
        } else {
            clk_mult = (uint64_t)(((unsigned __int128)1000000000ULL << 32) / frq);
            int64_t raw = cnt_ns();
            atomic_store(&clk_last, raw);
            atomic_store(&clk_off, mono_ns() - raw);
            clk_mode = 1;
            return raw + atomic_load(&clk_off);
        }
    }
    return mono_ns();
}
#else
static int64_t host_ns(void) { return mono_ns(); }
#endif

/* Relogio do convidado. E monotonico: o freio de timers_run() aumenta clock_offset
 * (o relogio "recua"), mas o convidado pode ja ter lido um valor maior (TSC/QPC) e o
 * Windows derruba o DWM se o QPC voltar. Por isso o tempo fica parado ate o valor ja
 * observado ser alcancado, em vez de andar para tras. */
/* relogio sem a trava monotonica (so para o freio e a fila de timers) */
static int64_t now_raw(mvm_vm *vm) { return (vm->paused_at ? vm->paused_at : host_ns()) - vm->clock_offset; }

int64_t mvm_now(mvm_vm *vm)
{
    int64_t t = now_raw(vm);
    int64_t last = atomic_load_explicit(&vm->clock_last, memory_order_relaxed);
    while (t > last) {
        if (atomic_compare_exchange_weak_explicit(&vm->clock_last, &last, t, memory_order_relaxed,
                                                  memory_order_relaxed))
            return t;
    }
    return last;
}

void timer_init(mvm_timer *t, void (*cb)(void *), void *opaque)
{
    memset(t, 0, sizeof(*t));
    t->cb = cb;
    t->opaque = opaque;
}

void timer_del(mvm_vm *vm, mvm_timer *t)
{
    if (!t->active)
        return;
    for (mvm_timer **pp = &vm->timers; *pp; pp = &(*pp)->next) {
        if (*pp == t) {
            *pp = t->next;
            break;
        }
    }
    t->active = false;
    t->next = NULL;
}

void timer_mod(mvm_vm *vm, mvm_timer *t, int64_t expire)
{
    timer_del(vm, t);
    t->expire = expire;
    t->active = true;
    mvm_timer **pp = &vm->timers;
    while (*pp && (*pp)->expire <= expire)
        pp = &(*pp)->next;
    t->next = *pp;
    *pp = t;
}

/* Executa os temporizadores vencidos. Retorna o proximo prazo (ou INT64_MAX). */
/* Atraso maximo tolerado de um timer (menor que o periodo do tick de 1 ms do Linux). */
#define TIMER_MAX_LATE_NS 500000LL

int64_t timers_run(mvm_vm *vm)
{
    int64_t now = now_raw(vm);
    /* Se o emulador nao acompanha o tempo real (CPU lenta, host ocupado ou suspenso),
     * o relogio do convidado e freado: em vez de perder periodos do PIT/APIC (e o
     * Linux ver os jiffies atrasados em relacao ao TSC), todas as fontes de tempo
     * andam juntas mais devagar. E o mesmo mecanismo usado na pausa. */
    if (vm->timers && now - vm->timers->expire > TIMER_MAX_LATE_NS) {
        int64_t warp = now - vm->timers->expire - TIMER_MAX_LATE_NS;
        vm->clock_offset += warp;
        vm->clock_warp_ns += warp;
        now -= warp;
    }
    while (vm->timers && vm->timers->expire <= now) {
        mvm_timer *t = vm->timers;
        vm->timers = t->next;
        t->active = false;
        t->next = NULL;
        t->cb(t->opaque);
    }
    return vm->timers ? vm->timers->expire : INT64_MAX;
}

int64_t host_clock_ns(void) { return host_ns(); }
