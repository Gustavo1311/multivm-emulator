/* Estruturas internas compartilhadas pelo nucleo do emulador. */
#ifndef MVM_INTERNAL_H
#define MVM_INTERNAL_H

#include "mvm.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define UNUSED(x) ((void)(x))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define PAGE_BITS 12
#define PAGE_SIZE 4096ULL
#define PAGE_MASK (~(PAGE_SIZE - 1))

/* ---- log ---- */
void mvm_log(mvm_log_level level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define LOGE(...) mvm_log(MVM_LOG_ERROR, __VA_ARGS__)
#define LOGW(...) mvm_log(MVM_LOG_WARN, __VA_ARGS__)
#define LOGI(...) mvm_log(MVM_LOG_INFO, __VA_ARGS__)
#define LOGD(...) mvm_log(MVM_LOG_DEBUG, __VA_ARGS__)
extern mvm_log_level mvm_log_threshold;

/* ---- espaco de enderecamento (memoria fisica ou portas de E/S) ---- */
typedef struct {
    uint64_t (*read)(void *opaque, uint64_t off, unsigned size);
    void (*write)(void *opaque, uint64_t off, uint64_t val, unsigned size);
} mvm_io_ops;

typedef struct {
    uint64_t base, size;
    uint8_t *host;        /* nao-NULL: RAM/ROM mapeada diretamente */
    bool readonly;
    _Atomic uint32_t *dirty_gen; /* incrementado a cada escrita (framebuffer/VRAM) */
    const mvm_io_ops *ops;
    void *opaque;
    const char *name;
} mvm_region;

#define MVM_MAX_REGIONS 96

typedef struct mvm_space {
    mvm_region r[MVM_MAX_REGIONS];
    int n;
    int last;
    struct mvm_vm *vm;
    /* paginas (4 KiB) com codigo traduzido pelo JIT: escritas nelas chamam code_hook */
    uint8_t *code_bits;
    uint64_t code_limit;
    void (*code_hook)(void *opaque, uint64_t pa, uint64_t len);
    void *code_opaque;
} mvm_space;

static inline bool space_is_code(const mvm_space *s, uint64_t pa)
{
    return s->code_bits && pa < s->code_limit && ((s->code_bits[pa >> 15] >> ((pa >> 12) & 7)) & 1);
}

/* avisa o JIT se [pa, pa+len) contem paginas com codigo traduzido */
static inline void space_code_check(mvm_space *s, uint64_t pa, uint64_t len)
{
    if (!s->code_bits || !len)
        return;
    for (uint64_t p = pa & ~0xfffULL; p < pa + len; p += 0x1000)
        if (space_is_code(s, p))
            s->code_hook(s->code_opaque, p, 0x1000);
}

mvm_region *space_add_ram(mvm_space *s, uint64_t base, uint64_t size, uint8_t *host,
                          bool readonly, const char *name);
mvm_region *space_add_io(mvm_space *s, uint64_t base, uint64_t size, const mvm_io_ops *ops,
                         void *opaque, const char *name);
mvm_region *space_find(mvm_space *s, uint64_t addr);
bool space_remove_at(mvm_space *s, uint64_t base);
uint64_t space_read(mvm_space *s, uint64_t addr, unsigned size);
void space_write(mvm_space *s, uint64_t addr, uint64_t val, unsigned size);
/* Ponteiro de host para [addr, addr+len) se for RAM contigua; senao NULL. */
uint8_t *space_ram_ptr(mvm_space *s, uint64_t addr, uint64_t len, bool write);
/* Copias genericas (funcionam com MMIO tambem). */
void space_memread(mvm_space *s, uint64_t addr, void *buf, uint64_t len);
void space_memwrite(mvm_space *s, uint64_t addr, const void *buf, uint64_t len);

static inline uint64_t ld_le(const uint8_t *p, unsigned size)
{
    switch (size) {
    case 1: return p[0];
    case 2: { uint16_t v; memcpy(&v, p, 2); return v; }
    case 4: { uint32_t v; memcpy(&v, p, 4); return v; }
    default: { uint64_t v; memcpy(&v, p, 8); return v; }
    }
}

static inline void st_le(uint8_t *p, uint64_t v, unsigned size)
{
    switch (size) {
    case 1: p[0] = (uint8_t)v; break;
    case 2: { uint16_t x = (uint16_t)v; memcpy(p, &x, 2); break; }
    case 4: { uint32_t x = (uint32_t)v; memcpy(p, &x, 4); break; }
    default: memcpy(p, &v, 8); break;
    }
}

/* ---- temporizadores (relogio virtual em ns) ---- */
typedef struct mvm_timer {
    int64_t expire;
    void (*cb)(void *opaque);
    void *opaque;
    bool active;
    struct mvm_timer *next;
} mvm_timer;

void timer_init(mvm_timer *t, void (*cb)(void *), void *opaque);
void timer_mod(struct mvm_vm *vm, mvm_timer *t, int64_t expire_ns);
void timer_del(struct mvm_vm *vm, mvm_timer *t);
int64_t mvm_now(struct mvm_vm *vm);

/* ---- dispositivo de caracteres (console serial) ---- */
#define CHR_IN_SIZE 4096
typedef struct {
    pthread_mutex_t lock;
    uint8_t in[CHR_IN_SIZE];
    unsigned in_head, in_tail;
    mvm_serial_cb out_cb;
    void *out_opaque;
} mvm_chardev;

void chr_init(mvm_chardev *c);
void chr_destroy(mvm_chardev *c);
void chr_write(mvm_chardev *c, const uint8_t *buf, size_t len);
size_t chr_push_input(mvm_chardev *c, const uint8_t *buf, size_t len);
int chr_read_byte(mvm_chardev *c); /* -1 se vazio */
bool chr_has_input(mvm_chardev *c);

/* ---- disco ---- */
struct blk_driver;
typedef struct {
    int fd;
    uint64_t size;          /* tamanho do disco virtual visto pelo convidado */
    bool readonly;
    bool can_discard;       /* discard (TRIM) devolve espaco ao host */
    const struct blk_driver *drv; /* formato (img/); NULL = raw */
    void *st;               /* estado do driver */
} mvm_blk;

mvm_blk *blk_open(const char *path, bool readonly, char *err, size_t errlen);
mvm_blk *blk_open_fd(int fd, bool readonly, char *err, size_t errlen);
void blk_close(mvm_blk *b);
int blk_read(mvm_blk *b, uint64_t off, void *buf, size_t len);
int blk_write(mvm_blk *b, uint64_t off, const void *buf, size_t len);
int blk_flush(mvm_blk *b);
int blk_discard(mvm_blk *b, uint64_t off, uint64_t len); /* zera o trecho; pode liberar espaco */
const char *blk_format(const mvm_blk *b);

/* ---- entrada ---- */
typedef struct {
    uint8_t type;   /* 0 = tecla, 1 = ponteiro, 2 = botao de energia */
    uint8_t pressed;
    uint16_t code;
    int16_t dx, dy, dz;
    uint32_t buttons;
} mvm_input_event;

#define INPUT_QUEUE 256

/* ---- CPU ---- */
typedef struct {
    void (*reset)(void *cpu);
    /* Executa ate 'budget' instrucoes. Retorna o numero executado. */
    int64_t (*run)(void *cpu, int64_t budget);
    bool (*halted)(void *cpu);  /* aguardando interrupcao (HLT/WFI) */
    void (*dump)(void *cpu, FILE *f);
    void (*destroy)(void *cpu);
    void (*tlb_flush)(void *cpu); /* mapa fisico mudou (ex.: BAR PCI realocado) */
} mvm_cpu_ops;

/* ---- maquina ---- */
typedef struct {
    const char *name;
    int (*init)(struct mvm_vm *vm, char *err, size_t errlen);
    void (*reset)(struct mvm_vm *vm);      /* recarrega kernel/estado de boot */
    void (*poll)(struct mvm_vm *vm);       /* chamado a cada fatia de execucao */
    void (*input)(struct mvm_vm *vm, const mvm_input_event *ev);
    size_t (*text_screen)(struct mvm_vm *vm, char *buf, size_t len);
    void (*destroy)(struct mvm_vm *vm);
    /* midias com a VM ligada (thread da VM); NULL = nao suportado */
    bool (*media_change)(struct mvm_vm *vm, int slot, mvm_blk *blk, char *err, size_t errlen);
    int (*media_hotplug)(struct mvm_vm *vm, mvm_blk *blk, char *err, size_t errlen); /* devolve o slot */
} mvm_machine_ops;

/* drive registrado pela maquina (ver vm_media_add) */
typedef struct {
    mvm_media_kind kind;
    mvm_media_bus bus;
    int unit;
    mvm_blk *blk;      /* midia atual (NULL = vazio) */
    int disk_index;    /* blk veio de vm->disks[i] (-1 = aberto depois, dono e a tabela) */
    bool used;
    char name[64];
} vm_media;

typedef struct media_req media_req;

extern const mvm_machine_ops machine_pc_ops;
extern const mvm_machine_ops machine_virt_ops;

struct mvm_vm {
    mvm_config cfg;
    char *s_kernel, *s_initrd, *s_cmdline, *s_dtb, *s_firmware, *s_vga_bios, *s_boot_order;
    char *s_disks[MVM_MAX_DISKS];
    char *s_dns;
    char *s_names[MVM_MAX_DISKS];
    /* drives (midias trocaveis) e pedidos de troca vindos de outras threads */
    vm_media media[MVM_MAX_MEDIA];
    pthread_mutex_t media_lock;
    pthread_cond_t media_cond;
    media_req *media_pending;
    _Atomic int running;       /* mvm_run em execucao (senao os pedidos sao feitos na hora) */
    struct mvm_net *net;     /* NAT em modo usuario (NULL = sem rede) */
    struct mvm_audio *audio; /* aneis de audio para o host (NULL = sem som) */

    mvm_space mem;
    mvm_space io;           /* portas de E/S (x86) */
    uint8_t *ram;
    uint64_t ram_base;
    uint64_t ram_size;

    void *cpu;
    const mvm_cpu_ops *cpu_ops;

    const mvm_machine_ops *machine;
    void *mach;

    mvm_timer *timers;
    int64_t clock_offset;   /* ajusta o relogio durante pausas */
    _Atomic int64_t clock_last; /* maior valor ja devolvido por mvm_now (monotonico) */
    int64_t clock_warp_ns;  /* quanto o relogio do convidado foi freado por atraso */
    int64_t paused_at;

    mvm_chardev serial;
    mvm_blk *disks[MVM_MAX_DISKS];

    /* framebuffer */
    uint8_t *fb;
    pthread_mutex_t fb_lock;   /* protege dimensoes e conteudo durante copia/renderizacao */
    uint32_t fb_w, fb_h, fb_stride;
    uint64_t fb_size;
    _Atomic uint32_t fb_gen;

    /* entrada (produtor: threads externas; consumidor: thread da VM) */
    pthread_mutex_t in_lock;
    mvm_input_event inq[INPUT_QUEUE];
    unsigned inq_head, inq_tail;

    /* controle */
    pthread_mutex_t lock;
    pthread_cond_t cond;
    _Atomic int stop_req;
    _Atomic int reset_req;
    bool no_reboot, reboot_exit;
    _Atomic int pause_req;
    _Atomic int kick;           /* acorda a thread da VM */
    _Atomic int cpu_exit;       /* a CPU deve sair do laco ja (reset/desligamento) */
    int shutdown;               /* definido por dispositivos (desligamento do convidado) */
    int fatal;                  /* erro fatal da CPU */
    _Atomic uint64_t insn_count;
};

typedef struct mvm_vm mvm_vm;

/* Chamado por dispositivos */
void vm_request_shutdown(mvm_vm *vm);
/* a maquina registra um drive (para listar e trocar midia); devolve o slot ou -1 */
int vm_media_add(mvm_vm *vm, mvm_media_kind kind, mvm_media_bus bus, int unit, int disk_index);
/* Acorda a thread da VM (de qualquer thread); urgent tambem interrompe a fatia da CPU. */
void vm_kick(mvm_vm *vm, bool urgent);
void vm_request_guest_reset(mvm_vm *vm);
void vm_fatal(mvm_vm *vm, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static inline void vm_fb_touch(mvm_vm *vm) { atomic_fetch_add_explicit(&vm->fb_gen, 1, memory_order_relaxed); }

/* ---- rede em modo usuario (net/) ---- */
typedef struct mvm_net mvm_net;
mvm_net *net_new(mvm_vm *vm, const mvm_net_config *cfg, char *err, size_t errlen);
void net_free(mvm_net *n);
void net_get_mac(mvm_net *n, uint8_t mac[6]);
void net_send(mvm_net *n, const uint8_t *frame, size_t len);   /* convidado -> rede */
size_t net_rx_peek(mvm_net *n);                                 /* tamanho do proximo quadro (0 = nenhum) */
size_t net_rx_pop(mvm_net *n, uint8_t *buf, size_t max);        /* rede -> convidado */
/* a placa e avisada (na thread da VM, via net_poll) quando ha quadros para receber */
void net_set_client(mvm_net *n, void (*rx_ready)(void *opaque), void *opaque);
void net_poll(mvm_net *n);

/* ---- audio (audio.c) ---- */
typedef struct mvm_audio mvm_audio;
typedef struct {
    uint64_t pos;     /* posicao em ponto fixo 32.32 */
    int16_t last[2];
} audio_resampler;
mvm_audio *audio_new(mvm_vm *vm, const mvm_audio_config *cfg);
void audio_free(mvm_audio *a);
void audio_out_active(mvm_audio *a, bool on);
void audio_in_active(mvm_audio *a, bool on);
bool audio_has_mic(mvm_audio *a);
/* PCM do convidado (8/16 bits com sinal em 16, sem sinal em 8; 1 ou 2 canais); volume em 1/256 */
void audio_out_write(mvm_audio *a, audio_resampler *rs, const void *pcm, size_t frames, int nch, int bits,
                     uint32_t rate, int vol_l, int vol_r);
size_t audio_in_read(mvm_audio *a, audio_resampler *rs, void *pcm, size_t frames, int nch, int bits, uint32_t rate);

/* ---- deflate pela zlib do sistema (zlib_dl.c) ---- */
typedef struct zdl_stream zdl_stream;
bool zdl_available(void);
zdl_stream *zdl_new(int level);
void zdl_free(zdl_stream *z);
long zdl_compress(zdl_stream *z, const void *in, size_t inlen, void *out, size_t outcap);
size_t zdl_bound(size_t inlen);

/* ---- carregadores ---- */
uint8_t *load_file(const char *path, size_t *size, char *err, size_t errlen);
/* Descompacta gzip. Retorna buffer novo ou NULL. */
uint8_t *gunzip(const uint8_t *in, size_t in_len, size_t *out_len);
bool is_gzip(const uint8_t *p, size_t len);

typedef struct {
    uint64_t entry;
    uint64_t low, high;   /* faixa fisica ocupada */
    int is64;
} elf_info;
bool elf_probe(const uint8_t *img, size_t len);
bool elf_load(mvm_space *s, const uint8_t *img, size_t len, elf_info *info, char *err, size_t errlen);

/* ---- FDT (device tree) ---- */
typedef struct {
    uint8_t *st; size_t st_len, st_cap;
    char *str; size_t str_len, str_cap;
    int depth;
} fdt_builder;

void fdt_begin(fdt_builder *f);
void fdt_node(fdt_builder *f, const char *name);
void fdt_end_node(fdt_builder *f);
void fdt_prop(fdt_builder *f, const char *name, const void *data, size_t len);
void fdt_prop_u32(fdt_builder *f, const char *name, uint32_t v);
void fdt_prop_u64(fdt_builder *f, const char *name, uint64_t v);
void fdt_prop_str(fdt_builder *f, const char *name, const char *s);
void fdt_prop_strs(fdt_builder *f, const char *name, const char *const *s, int n);
void fdt_prop_cells(fdt_builder *f, const char *name, const uint32_t *cells, int n);
void fdt_prop_empty(fdt_builder *f, const char *name);
/* Finaliza; retorna blob (malloc) e tamanho. */
uint8_t *fdt_finish(fdt_builder *f, size_t *len);

/* ---- utilitarios ---- */
static inline uint64_t sext64(uint64_t v, unsigned bits)
{
    unsigned s = 64 - bits;
    return (uint64_t)((int64_t)(v << s) >> s);
}

static inline uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }
static inline uint64_t bswap64(uint64_t v) { return __builtin_bswap64(v); }

#endif
