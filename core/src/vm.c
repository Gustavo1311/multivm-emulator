/* Ciclo de vida da VM, laco principal e API publica. */
#define _GNU_SOURCE
#include "internal.h"

#include <stdarg.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/mman.h>
#include <time.h>

int64_t timers_run(mvm_vm *vm);
int64_t host_clock_ns(void);

#define SLICE_INSNS 32768 /* timers e entrada sao tratados entre fatias */

static const char *arch_names[] = {"i386", "x86_64", "arm", "arm64"};

const char *mvm_arch_name(mvm_arch arch)
{
    return (unsigned)arch < ARRAY_SIZE(arch_names) ? arch_names[arch] : "?";
}

int mvm_arch_from_name(const char *name)
{
    if (!name)
        return -1;
    for (unsigned i = 0; i < ARRAY_SIZE(arch_names); i++)
        if (!strcasecmp(name, arch_names[i]))
            return (int)i;
    if (!strcasecmp(name, "x86-64") || !strcasecmp(name, "amd64"))
        return MVM_ARCH_X86_64;
    if (!strcasecmp(name, "x86") || !strcasecmp(name, "i686"))
        return MVM_ARCH_I386;
    if (!strcasecmp(name, "aarch64"))
        return MVM_ARCH_ARM64;
    if (!strcasecmp(name, "armv7") || !strcasecmp(name, "aarch32"))
        return MVM_ARCH_ARM;
    return -1;
}

void mvm_config_init(mvm_config *cfg, mvm_arch arch)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->arch = arch;
    cfg->ram_mb = 256;
}

static char *dupstr(const char *s) { return s ? strdup(s) : NULL; }

void vm_request_shutdown(mvm_vm *vm)
{
    vm->shutdown = 1;
    atomic_store(&vm->cpu_exit, 1);
}

void vm_request_guest_reset(mvm_vm *vm)
{
    static int dump = -1;
    if (dump < 0)
        dump = getenv("MVM_DUMP_ON_RESET") != NULL;
    if (dump) { /* depuracao: estado da CPU no momento em que o convidado pediu o reset */
        LOGI("reset pedido pelo convidado; estado da CPU:");
        mvm_debug_dump(vm);
    }
    if (vm->no_reboot) {
        vm->reboot_exit = true;
        vm->shutdown = 1;
        atomic_store(&vm->cpu_exit, 1);
        return;
    }
    atomic_store(&vm->reset_req, 1);
    atomic_store(&vm->cpu_exit, 1);
}

void vm_fatal(mvm_vm *vm, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    LOGE("erro fatal: %s", buf);
    vm->fatal = 1;
}

mvm_vm *mvm_create(const mvm_config *cfg, char *err, size_t errlen)
{
    char dummy[8];
    if (!err) {
        err = dummy;
        errlen = sizeof(dummy);
    }
    err[0] = 0;
    if ((unsigned)cfg->arch > MVM_ARCH_ARM64) {
        snprintf(err, errlen, "arquitetura invalida");
        return NULL;
    }
    if (cfg->ram_mb < 4 || cfg->ram_mb > 3072) {
        snprintf(err, errlen, "memoria invalida (%u MiB; 4..3072)", cfg->ram_mb);
        return NULL;
    }

    mvm_vm *vm = calloc(1, sizeof(*vm));
    vm->cfg = *cfg;
    vm->s_kernel = dupstr(cfg->kernel);
    vm->s_initrd = dupstr(cfg->initrd);
    vm->s_cmdline = dupstr(cfg->cmdline);
    vm->s_dtb = dupstr(cfg->dtb);
    vm->s_firmware = dupstr(cfg->firmware);
    vm->s_vga_bios = dupstr(cfg->vga_bios);
    vm->s_boot_order = dupstr(cfg->boot_order);
    vm->cfg.vga_bios = vm->s_vga_bios;
    vm->cfg.boot_order = vm->s_boot_order;
    vm->cfg.kernel = vm->s_kernel;
    vm->cfg.initrd = vm->s_initrd;
    vm->cfg.cmdline = vm->s_cmdline;
    vm->cfg.dtb = vm->s_dtb;
    vm->cfg.firmware = vm->s_firmware;
    for (int i = 0; i < MVM_MAX_DISKS; i++) {
        vm->s_disks[i] = dupstr(cfg->disks[i].path);
        vm->cfg.disks[i].path = vm->s_disks[i];
    }
    for (int i = 0; i < MVM_MAX_DISKS; i++) {
        vm->s_names[i] = dupstr(cfg->disks[i].name);
        vm->cfg.disks[i].name = vm->s_names[i];
    }
    vm->s_dns = dupstr(cfg->net.dns);
    vm->cfg.net.dns = vm->s_dns;
    bool x86 = cfg->arch == MVM_ARCH_I386 || cfg->arch == MVM_ARCH_X86_64;
    if (vm->cfg.net.model == MVM_NIC_AUTO)
        vm->cfg.net.model = x86 && cfg->firmware ? MVM_NIC_RTL8139 : MVM_NIC_VIRTIO;
    if (vm->cfg.audio.model == MVM_SND_AUTO)
        vm->cfg.audio.model = x86 ? MVM_SND_AC97 : MVM_SND_VIRTIO;
    if (!x86 && vm->cfg.net.model != MVM_NIC_NONE)
        vm->cfg.net.model = MVM_NIC_VIRTIO;
    if (!x86 && vm->cfg.audio.model != MVM_SND_NONE)
        vm->cfg.audio.model = MVM_SND_VIRTIO;

    vm->mem.vm = vm;
    vm->io.vm = vm;
    chr_init(&vm->serial);
    pthread_mutex_init(&vm->in_lock, NULL);
    pthread_mutex_init(&vm->fb_lock, NULL);
    pthread_mutex_init(&vm->lock, NULL);
    pthread_mutex_init(&vm->media_lock, NULL);
    pthread_cond_init(&vm->media_cond, NULL);
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&vm->cond, &ca);
    pthread_condattr_destroy(&ca);
    vm->clock_offset = host_clock_ns();

    vm->ram_size = (uint64_t)cfg->ram_mb << 20;
    vm->ram = mmap(NULL, vm->ram_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (vm->ram == MAP_FAILED) {
        vm->ram = NULL;
        snprintf(err, errlen, "sem memoria para %u MiB de RAM", cfg->ram_mb);
        mvm_destroy(vm);
        return NULL;
    }

    if (cfg->fb_width && cfg->fb_height) {
        if (cfg->fb_width > 4096 || cfg->fb_height > 4096) {
            snprintf(err, errlen, "framebuffer grande demais");
            mvm_destroy(vm);
            return NULL;
        }
        vm->fb_w = cfg->fb_width;
        vm->fb_h = cfg->fb_height;
        vm->fb_stride = cfg->fb_width * 4;
        vm->fb_size = ((uint64_t)vm->fb_stride * vm->fb_h + 0xffff) & ~0xffffULL;
        vm->fb = calloc(1, vm->fb_size);
    }

    for (int i = 0; i < MVM_MAX_DISKS; i++) {
        if (cfg->disks[i].empty) /* drive sem midia */
            continue;
        if (cfg->disks[i].has_fd) {
            vm->disks[i] = blk_open_fd(cfg->disks[i].fd, cfg->disks[i].readonly, err, errlen);
            if (!vm->disks[i]) {
                mvm_destroy(vm);
                return NULL;
            }
            continue;
        }
        if (!cfg->disks[i].path)
            continue;
        vm->disks[i] = blk_open(cfg->disks[i].path, cfg->disks[i].readonly, err, errlen);
        if (!vm->disks[i]) {
            mvm_destroy(vm);
            return NULL;
        }
    }

    if (vm->cfg.net.model != MVM_NIC_NONE) {
        vm->net = net_new(vm, &vm->cfg.net, err, errlen);
        if (!vm->net) {
            mvm_destroy(vm);
            return NULL;
        }
    }
    if (vm->cfg.audio.model != MVM_SND_NONE)
        vm->audio = audio_new(vm, &vm->cfg.audio);

    switch (cfg->arch) {
    case MVM_ARCH_I386:
    case MVM_ARCH_X86_64:
        vm->machine = &machine_pc_ops;
        break;
    default:
        vm->machine = &machine_virt_ops;
        break;
    }
    if (vm->machine->init(vm, err, errlen) < 0) {
        if (!err[0])
            snprintf(err, errlen, "falha ao iniciar a maquina");
        mvm_destroy(vm);
        return NULL;
    }
    LOGI("VM criada: %s, maquina '%s', %u MiB", mvm_arch_name(cfg->arch), vm->machine->name,
         cfg->ram_mb);
    return vm;
}

void mvm_destroy(mvm_vm *vm)
{
    if (!vm)
        return;
    if (vm->machine && vm->machine->destroy)
        vm->machine->destroy(vm);
    net_free(vm->net);
    audio_free(vm->audio);
    free(vm->s_dns);
    if (vm->cpu && vm->cpu_ops)
        vm->cpu_ops->destroy(vm->cpu);
    for (int i = 0; i < MVM_MAX_DISKS; i++) {
        blk_close(vm->disks[i]);
        free(vm->s_disks[i]);
        free(vm->s_names[i]);
    }
    for (int i = 0; i < MVM_MAX_MEDIA; i++) /* midias abertas depois do boot */
        if (vm->media[i].used && vm->media[i].disk_index < 0)
            blk_close(vm->media[i].blk);
    pthread_mutex_destroy(&vm->media_lock);
    pthread_cond_destroy(&vm->media_cond);
    if (vm->ram)
        munmap(vm->ram, vm->ram_size);
    free(vm->fb);
    free(vm->s_kernel);
    free(vm->s_initrd);
    free(vm->s_cmdline);
    free(vm->s_dtb);
    free(vm->s_firmware);
    free(vm->s_vga_bios);
    free(vm->s_boot_order);
    pthread_mutex_destroy(&vm->fb_lock);
    chr_destroy(&vm->serial);
    pthread_mutex_destroy(&vm->in_lock);
    pthread_mutex_destroy(&vm->lock);
    pthread_cond_destroy(&vm->cond);
    free(vm);
}

static void wait_until(mvm_vm *vm, int64_t host_deadline)
{
    /* O relogio do host (host_clock_ns) pode nao ter a mesma base do CLOCK_MONOTONIC da
     * condicao (no AArch64 o contador generico continua contando com o aparelho
     * suspenso): converte o prazo em espera relativa, limitada a 1 s. */
    int64_t delta = host_deadline - host_clock_ns();
    if (delta < 0)
        delta = 0;
    if (delta > 1000000000LL)
        delta = 1000000000LL;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t abs_ns = (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec + delta;
    struct timespec ts = {.tv_sec = abs_ns / 1000000000LL, .tv_nsec = abs_ns % 1000000000LL};
    pthread_mutex_lock(&vm->lock);
    if (!atomic_exchange(&vm->kick, 0))
        pthread_cond_timedwait(&vm->cond, &vm->lock, &ts);
    atomic_store(&vm->kick, 0);
    pthread_mutex_unlock(&vm->lock);
}

static void kick(mvm_vm *vm)
{
    pthread_mutex_lock(&vm->lock);
    atomic_store(&vm->kick, 1);
    pthread_cond_broadcast(&vm->cond);
    pthread_mutex_unlock(&vm->lock);
}

void vm_kick(mvm_vm *vm, bool urgent)
{
    if (urgent)
        atomic_store(&vm->cpu_exit, 1);
    kick(vm);
}

static void drain_input(mvm_vm *vm)
{
    for (;;) {
        mvm_input_event ev;
        pthread_mutex_lock(&vm->in_lock);
        if (vm->inq_tail == vm->inq_head) {
            pthread_mutex_unlock(&vm->in_lock);
            return;
        }
        ev = vm->inq[vm->inq_tail];
        vm->inq_tail = (vm->inq_tail + 1) % INPUT_QUEUE;
        pthread_mutex_unlock(&vm->in_lock);
        if (vm->machine->input)
            vm->machine->input(vm, &ev);
    }
}

/* ------------------------------------------------------------ midias com a VM ligada */

enum { MREQ_INSERT, MREQ_EJECT, MREQ_ADD };

struct media_req {
    int op, slot;
    mvm_blk *blk;
    char name[64];
    int result;
    char err[256];
    bool done;
};

static void base_name(char *dst, size_t n, const char *path)
{
    const char *b = path ? strrchr(path, '/') : NULL;
    snprintf(dst, n, "%s", b ? b + 1 : path ? path : "");
}

int vm_media_add(mvm_vm *vm, mvm_media_kind kind, mvm_media_bus bus, int unit, int disk_index)
{
    pthread_mutex_lock(&vm->media_lock);
    int r = -1;
    for (int i = 0; i < MVM_MAX_MEDIA; i++) {
        vm_media *m = &vm->media[i];
        if (m->used)
            continue;
        m->used = true;
        m->kind = kind;
        m->bus = bus;
        m->unit = unit;
        m->disk_index = disk_index;
        m->blk = disk_index >= 0 ? vm->disks[disk_index] : NULL;
        if (disk_index >= 0 && m->blk) {
            const mvm_disk_config *d = &vm->cfg.disks[disk_index];
            if (d->name)
                snprintf(m->name, sizeof(m->name), "%s", d->name);
            else
                base_name(m->name, sizeof(m->name), d->path);
        }
        r = i;
        break;
    }
    pthread_mutex_unlock(&vm->media_lock);
    return r;
}

/* a midia antiga saiu do dispositivo: fecha o arquivo */
static void media_release(mvm_vm *vm, vm_media *m)
{
    if (!m->blk)
        return;
    if (m->disk_index >= 0)
        vm->disks[m->disk_index] = NULL;
    blk_close(m->blk);
    m->blk = NULL;
    m->disk_index = -1;
}

/* aplica o pedido (thread da VM, ou a de quem chamou se a VM nao estiver rodando) */
static void media_apply(mvm_vm *vm, media_req *r)
{
    r->result = -1;
    const mvm_machine_ops *mo = vm->machine;
    if (r->op == MREQ_ADD) {
        if (!mo->media_hotplug) {
            snprintf(r->err, sizeof(r->err), "esta maquina nao aceita conectar discos com a VM ligada");
            return;
        }
        int slot = mo->media_hotplug(vm, r->blk, r->err, sizeof(r->err));
        if (slot < 0)
            return;
        pthread_mutex_lock(&vm->media_lock);
        vm->media[slot].blk = r->blk;
        snprintf(vm->media[slot].name, sizeof(vm->media[slot].name), "%s", r->name);
        pthread_mutex_unlock(&vm->media_lock);
        r->blk = NULL;
        r->result = slot;
        return;
    }
    if (r->slot < 0 || r->slot >= MVM_MAX_MEDIA || !vm->media[r->slot].used) {
        snprintf(r->err, sizeof(r->err), "drive %d nao existe", r->slot);
        return;
    }
    vm_media *m = &vm->media[r->slot];
    if (!mo->media_change) {
        snprintf(r->err, sizeof(r->err), "esta maquina nao aceita trocar midias com a VM ligada");
        return;
    }
    if (r->op == MREQ_INSERT && m->blk && m->kind == MVM_MEDIA_HDD) {
        snprintf(r->err, sizeof(r->err), "a porta ja tem um disco: remova-o antes");
        return;
    }
    mvm_blk *nb = r->op == MREQ_INSERT ? r->blk : NULL;
    if (!mo->media_change(vm, r->slot, nb, r->err, sizeof(r->err)))
        return;
    pthread_mutex_lock(&vm->media_lock);
    media_release(vm, m);
    m->blk = nb;
    m->disk_index = -1;
    snprintf(m->name, sizeof(m->name), "%s", nb ? r->name : "");
    pthread_mutex_unlock(&vm->media_lock);
    r->blk = NULL;
    r->result = 0;
}

/* thread da VM: atende o pedido pendente */
static void media_service(mvm_vm *vm)
{
    pthread_mutex_lock(&vm->media_lock);
    media_req *r = vm->media_pending;
    vm->media_pending = NULL;
    pthread_mutex_unlock(&vm->media_lock);
    if (!r)
        return;
    media_apply(vm, r);
    pthread_mutex_lock(&vm->media_lock);
    r->done = true;
    pthread_cond_broadcast(&vm->media_cond);
    pthread_mutex_unlock(&vm->media_lock);
}

static int media_submit(mvm_vm *vm, media_req *r, char *err, size_t errlen)
{
    pthread_mutex_lock(&vm->media_lock);
    while (vm->media_pending) /* um pedido por vez */
        pthread_cond_wait(&vm->media_cond, &vm->media_lock);
    if (!atomic_load(&vm->running)) {
        pthread_mutex_unlock(&vm->media_lock);
        media_apply(vm, r);
    } else {
        vm->media_pending = r;
        pthread_mutex_unlock(&vm->media_lock);
        vm_kick(vm, true);
        pthread_mutex_lock(&vm->media_lock);
        while (!r->done) {
            if (!atomic_load(&vm->running) && vm->media_pending == r) { /* a VM parou: faz aqui */
                vm->media_pending = NULL;
                pthread_mutex_unlock(&vm->media_lock);
                media_apply(vm, r);
                pthread_mutex_lock(&vm->media_lock);
                break;
            }
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 100000000;
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&vm->media_cond, &vm->media_lock, &ts);
        }
        pthread_cond_broadcast(&vm->media_cond);
        pthread_mutex_unlock(&vm->media_lock);
    }
    if (r->blk) /* nao usado: fecha */
        blk_close(r->blk);
    if (r->result < 0 && err)
        snprintf(err, errlen, "%s", r->err[0] ? r->err : "falha ao trocar a midia");
    return r->result;
}

static mvm_blk *media_open(const char *path, int fd, bool ro, char *err, size_t errlen)
{
    if (fd >= 0)
        return blk_open_fd(fd, ro, err, errlen);
    if (!path) {
        snprintf(err, errlen, "sem arquivo");
        return NULL;
    }
    return blk_open(path, ro, err, errlen);
}

int mvm_media_list(mvm_vm *vm, mvm_media_info *out, int max)
{
    int n = 0;
    pthread_mutex_lock(&vm->media_lock);
    for (int i = 0; i < MVM_MAX_MEDIA && n < max; i++) {
        vm_media *m = &vm->media[i];
        if (!m->used)
            continue;
        mvm_media_info *o = &out[n++];
        o->kind = m->kind;
        o->bus = m->bus;
        o->unit = m->unit;
        o->present = m->blk != NULL;
        o->readonly = m->blk ? m->blk->readonly : false;
        o->changeable = vm->machine->media_change &&
                        (m->kind != MVM_MEDIA_HDD || m->bus == MVM_BUS_SATA);
        snprintf(o->name, sizeof(o->name), "%s", m->name); /* slots nunca sao liberados: indice = slot */
    }
    pthread_mutex_unlock(&vm->media_lock);
    return n;
}

int mvm_media_insert(mvm_vm *vm, int slot, const char *path, int fd, bool readonly, const char *name, char *err,
                     size_t errlen)
{
    media_req r = {.op = MREQ_INSERT, .slot = slot};
    if (slot >= 0 && slot < MVM_MAX_MEDIA && vm->media[slot].kind == MVM_MEDIA_CD)
        readonly = true;
    r.blk = media_open(path, fd, readonly, err, errlen);
    if (!r.blk)
        return -1;
    if (name)
        snprintf(r.name, sizeof(r.name), "%s", name);
    else
        base_name(r.name, sizeof(r.name), path);
    return media_submit(vm, &r, err, errlen) < 0 ? -1 : 0;
}

int mvm_media_eject(mvm_vm *vm, int slot, char *err, size_t errlen)
{
    media_req r = {.op = MREQ_EJECT, .slot = slot};
    return media_submit(vm, &r, err, errlen) < 0 ? -1 : 0;
}

int mvm_media_add_disk(mvm_vm *vm, const char *path, int fd, bool readonly, const char *name, char *err,
                       size_t errlen)
{
    media_req r = {.op = MREQ_ADD};
    r.blk = media_open(path, fd, readonly, err, errlen);
    if (!r.blk)
        return -1;
    if (name)
        snprintf(r.name, sizeof(r.name), "%s", name);
    else
        base_name(r.name, sizeof(r.name), path);
    return media_submit(vm, &r, err, errlen);
}

/* ------------------------------------------------------------ execucao */

static int run_loop(mvm_vm *vm);

int mvm_run(mvm_vm *vm)
{
    atomic_store(&vm->running, 1);
    int r = run_loop(vm);
    if (vm->fb_flush) /* a ultima imagem fica disponivel depois da execucao */
        vm->fb_flush(vm->fb_flush_opaque);
    atomic_store(&vm->running, 0);
    media_service(vm); /* pedido que chegou na saida */
    pthread_mutex_lock(&vm->media_lock);
    pthread_cond_broadcast(&vm->media_cond);
    pthread_mutex_unlock(&vm->media_lock);
    return r;
}

static int run_loop(mvm_vm *vm)
{
    vm->shutdown = 0;
    vm->fatal = 0;
    for (;;) {
        if (atomic_load(&vm->stop_req)) {
            atomic_store(&vm->stop_req, 0);
            return MVM_EXIT_STOPPED;
        }
        if (vm->shutdown) {
            if (vm->reboot_exit) {
                LOGI("reinicio pedido pelo convidado: encerrando (sem reinicio)");
                return MVM_EXIT_REBOOT;
            }
            return MVM_EXIT_SHUTDOWN;
        }
        if (vm->fatal) {
            if (vm->cpu_ops->dump)
                vm->cpu_ops->dump(vm->cpu, stderr);
            return MVM_EXIT_ERROR;
        }
        atomic_store(&vm->cpu_exit, 0);
        if (atomic_exchange(&vm->reset_req, 0)) {
            LOGI("reiniciando a maquina");
            vm->machine->reset(vm);
        }
        if (atomic_load(&vm->pause_req)) {
            vm->paused_at = host_clock_ns();
            while (atomic_load(&vm->pause_req) && !atomic_load(&vm->stop_req)) {
                media_service(vm); /* troca de midia tambem com a VM pausada */
                if (vm->fb_flush && vm_fb_watched(vm, 500)) /* desenho adiado sem espectador */
                    vm->fb_flush(vm->fb_flush_opaque);
                wait_until(vm, host_clock_ns() + 100000000LL);
            }
            vm->clock_offset += host_clock_ns() - vm->paused_at;
            vm->paused_at = 0;
            continue;
        }

        drain_input(vm);
        if (vm->media_pending)
            media_service(vm);
        if (vm->machine->poll)
            vm->machine->poll(vm);
        int64_t next = timers_run(vm);

        if (vm->cpu_ops->halted(vm->cpu)) {
            int64_t now = mvm_now(vm);
            int64_t limit = now + 10000000LL; /* 10 ms: verifica entrada do console */
            if (next < limit)
                limit = next;
            if (limit > now)
                wait_until(vm, limit + vm->clock_offset);
            /* ainda executa a CPU: ela sai do HLT se houver interrupcao */
        }
        if (atomic_load(&vm->cpu_exit))
            continue;
        int64_t n = vm->cpu_ops->run(vm->cpu, SLICE_INSNS);
        atomic_fetch_add_explicit(&vm->insn_count, (uint64_t)n, memory_order_relaxed);
    }
}

void mvm_request_stop(mvm_vm *vm)
{
    atomic_store(&vm->stop_req, 1);
    kick(vm);
}

void mvm_set_no_reboot(mvm_vm *vm, bool on) { vm->no_reboot = on; }

void mvm_debug_dump(mvm_vm *vm)
{
    if (vm->cfg.arch == MVM_ARCH_X86_64 || vm->cfg.arch == MVM_ARCH_I386) {
        extern void x86_debug_dump(void *cpu);
        x86_debug_dump(vm->cpu);
    } else if (vm->cpu_ops && vm->cpu_ops->dump) {
        vm->cpu_ops->dump(vm->cpu, stderr);
    }
}

void mvm_request_reset(mvm_vm *vm)
{
    atomic_store(&vm->reset_req, 1);
    kick(vm);
}

void mvm_pause(mvm_vm *vm) { atomic_store(&vm->pause_req, 1); }

void mvm_resume(mvm_vm *vm)
{
    atomic_store(&vm->pause_req, 0);
    kick(vm);
}

bool mvm_is_paused(mvm_vm *vm) { return atomic_load(&vm->pause_req) != 0; }

void mvm_set_serial_output(mvm_vm *vm, mvm_serial_cb cb, void *opaque)
{
    vm->serial.out_opaque = opaque;
    vm->serial.out_cb = cb;
}

size_t mvm_serial_input(mvm_vm *vm, const uint8_t *data, size_t len)
{
    size_t n = chr_push_input(&vm->serial, data, len);
    kick(vm);
    return n;
}

bool mvm_fb_info_get(mvm_vm *vm, mvm_fb_info *info)
{
    if (!vm->fb)
        return false;
    pthread_mutex_lock(&vm->fb_lock);
    info->width = vm->fb_w;
    info->height = vm->fb_h;
    info->stride = vm->fb_w * 4; /* stride das copias feitas por mvm_fb_copy */
    info->format = MVM_FB_XRGB8888;
    pthread_mutex_unlock(&vm->fb_lock);
    return true;
}

/* Quem consulta a geracao esta mostrando a tela: a VGA so desenha modos graficos assim. */
uint32_t mvm_fb_generation(mvm_vm *vm)
{
    atomic_store_explicit(&vm->fb_watch_ns, host_clock_ns(), memory_order_relaxed);
    return atomic_load(&vm->fb_gen);
}

bool vm_fb_watched(mvm_vm *vm, int64_t ms)
{
    return host_clock_ns() - atomic_load_explicit(&vm->fb_watch_ns, memory_order_relaxed) < ms * 1000000LL;
}

void vm_fb_rows_changed(mvm_vm *vm, uint32_t y0, uint32_t y1)
{
    uint32_t g = atomic_fetch_add(&vm->fb_gen, 1) + 1;
    unsigned n = sizeof(vm->fb_log) / sizeof(vm->fb_log[0]);
    vm->fb_log[g % n].gen = g;
    vm->fb_log[g % n].y0 = y0;
    vm->fb_log[g % n].y1 = y1;
}

static void copy_row(uint32_t *d, const uint32_t *s, uint32_t w, mvm_fb_copy_fmt fmt)
{
    switch (fmt) {
    case MVM_FB_COPY_ARGB:
        for (uint32_t x = 0; x < w; x++)
            d[x] = s[x] | 0xff000000u;
        break;
    case MVM_FB_COPY_RGBA: /* 0x00RRGGBB -> bytes R,G,B,FF (little-endian: 0xFFBBGGRR) */
        for (uint32_t x = 0; x < w; x++) {
            uint32_t p = s[x];
            d[x] = 0xff000000u | (p & 0xff00u) | ((p >> 16) & 0xffu) | ((p & 0xffu) << 16);
        }
        break;
    default:
        memcpy(d, s, (size_t)w * 4);
        break;
    }
}

int mvm_fb_copy_rows(mvm_vm *vm, void *dst, uint32_t dst_w, uint32_t dst_h, size_t dst_stride,
                     mvm_fb_copy_fmt fmt, uint32_t *since, uint32_t *y0, uint32_t *y1, mvm_fb_info *info)
{
    if (!vm->fb)
        return -1;
    atomic_store_explicit(&vm->fb_watch_ns, host_clock_ns(), memory_order_relaxed);
    pthread_mutex_lock(&vm->fb_lock);
    mvm_fb_info fi = {vm->fb_w, vm->fb_h, vm->fb_w * 4, MVM_FB_XRGB8888};
    if (info)
        *info = fi;
    if (fi.width != dst_w || fi.height != dst_h || dst_stride < (size_t)fi.width * 4) {
        pthread_mutex_unlock(&vm->fb_lock);
        return -1;
    }
    uint32_t cur = atomic_load(&vm->fb_gen);
    if (cur == *since) {
        pthread_mutex_unlock(&vm->fb_lock);
        return 0;
    }
    /* uniao das faixas de cada geracao em (since, cur]; sem historico completo, tudo */
    unsigned n = sizeof(vm->fb_log) / sizeof(vm->fb_log[0]);
    uint32_t a = 0, b = fi.height;
    uint32_t span = cur - *since;
    if (*since != 0 && span < n) {
        a = UINT32_MAX;
        b = 0;
        for (uint32_t g = *since + 1; g != cur + 1; g++) {
            if (vm->fb_log[g % n].gen != g) {
                a = 0;
                b = fi.height;
                break;
            }
            if (vm->fb_log[g % n].y0 < a)
                a = vm->fb_log[g % n].y0;
            if (vm->fb_log[g % n].y1 > b)
                b = vm->fb_log[g % n].y1;
        }
        if (b > fi.height)
            b = fi.height;
        if (a > b)
            a = b;
    }
    for (uint32_t y = a; y < b; y++)
        copy_row((uint32_t *)((uint8_t *)dst + (size_t)y * dst_stride),
                 (const uint32_t *)(vm->fb + (size_t)y * vm->fb_stride), fi.width, fmt);
    pthread_mutex_unlock(&vm->fb_lock);
    *since = cur;
    if (y0)
        *y0 = a;
    if (y1)
        *y1 = b;
    return a < b ? 1 : 0;
}

bool mvm_fb_copy(mvm_vm *vm, void *dst, size_t dst_size, mvm_fb_info *info)
{
    if (!vm->fb)
        return false;
    atomic_store_explicit(&vm->fb_watch_ns, host_clock_ns(), memory_order_relaxed);
    pthread_mutex_lock(&vm->fb_lock);
    mvm_fb_info fi = {vm->fb_w, vm->fb_h, vm->fb_w * 4, MVM_FB_XRGB8888};
    bool ok = (size_t)fi.stride * fi.height <= dst_size;
    if (ok) {
        uint8_t *d = dst;
        for (uint32_t y = 0; y < fi.height; y++)
            memcpy(d + (size_t)y * fi.stride, vm->fb + (size_t)y * vm->fb_stride, fi.stride);
    }
    pthread_mutex_unlock(&vm->fb_lock);
    if (info)
        *info = fi;
    return ok;
}

size_t mvm_text_screen(mvm_vm *vm, char *buf, size_t len)
{
    if (!vm->machine->text_screen || !len)
        return 0;
    pthread_mutex_lock(&vm->fb_lock);
    size_t n = vm->machine->text_screen(vm, buf, len);
    pthread_mutex_unlock(&vm->fb_lock);
    return n;
}

static void push_input(mvm_vm *vm, const mvm_input_event *ev)
{
    pthread_mutex_lock(&vm->in_lock);
    unsigned next = (vm->inq_head + 1) % INPUT_QUEUE;
    if (next != vm->inq_tail) {
        vm->inq[vm->inq_head] = *ev;
        vm->inq_head = next;
    }
    pthread_mutex_unlock(&vm->in_lock);
    kick(vm);
}

void mvm_key_event(mvm_vm *vm, uint32_t keycode, bool pressed)
{
    mvm_input_event ev = {.type = 0, .pressed = pressed, .code = (uint16_t)keycode};
    push_input(vm, &ev);
}

void mvm_power_button(mvm_vm *vm)
{
    mvm_input_event ev = {.type = 2};
    push_input(vm, &ev);
}

void mvm_pointer_event(mvm_vm *vm, int dx, int dy, int dz, uint32_t buttons)
{
    mvm_input_event ev = {.type = 1, .dx = (int16_t)dx, .dy = (int16_t)dy, .dz = (int16_t)dz,
                          .buttons = buttons};
    push_input(vm, &ev);
}

uint64_t mvm_instruction_count(mvm_vm *vm) { return atomic_load(&vm->insn_count); }

int64_t mvm_clock_lag_ns(mvm_vm *vm) { return vm->clock_warp_ns; }
