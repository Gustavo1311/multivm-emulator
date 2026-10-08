/*
 * MultiVM - emulador de sistema completo (x86-64, i386, ARM, ARM64).
 *
 * API publica em C. E consumida pela ponte JNI (Android) e pela CLI de host.
 * Todas as funcoes sao thread-safe, exceto mvm_create/mvm_destroy, e mvm_run
 * deve ser chamada por uma unica thread (a "thread da VM").
 */
#ifndef MVM_H
#define MVM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MVM_VERSION "0.1.0"
#define MVM_MAX_DISKS 10 /* ex.: 4 discos + 4 CDs + 2 disquetes */

typedef enum {
    MVM_ARCH_I386 = 0,
    MVM_ARCH_X86_64 = 1,
    MVM_ARCH_ARM = 2,
    MVM_ARCH_ARM64 = 3,
} mvm_arch;

typedef enum {
    MVM_EXIT_STOPPED = 0,  /* mvm_request_stop() */
    MVM_EXIT_SHUTDOWN = 1, /* o convidado desligou a maquina */
    MVM_EXIT_ERROR = 2,    /* erro fatal da emulacao (ver log) */
    MVM_EXIT_REBOOT = 3,   /* o convidado pediu reinicio com mvm_set_no_reboot(vm, true) */
} mvm_exit_reason;

typedef enum {
    MVM_LOG_ERROR = 0,
    MVM_LOG_WARN = 1,
    MVM_LOG_INFO = 2,
    MVM_LOG_DEBUG = 3,
} mvm_log_level;

typedef enum {
    MVM_DISK_AUTO = 0,   /* virtio-blk no boot direto; IDE quando ha BIOS (x86) */
    MVM_DISK_VIRTIO = 1,
    MVM_DISK_IDE = 2,    /* disco ATA (somente x86) */
    MVM_DISK_CDROM = 3,  /* CD-ROM ATAPI com imagem ISO (somente x86) */
    MVM_DISK_SATA = 4,   /* disco SATA no controlador AHCI (somente x86) */
    MVM_DISK_SATA_CDROM = 5, /* CD-ROM ATAPI no AHCI (somente x86) */
    MVM_DISK_FLOPPY = 6, /* disquete no controlador 82078 (somente x86, ate 2) */
} mvm_disk_type;

typedef struct {
    const char *path;   /* caminho da imagem (raw) */
    int fd;             /* alternativa ao caminho: descritor ja aberto (use com has_fd) */
    bool has_fd;        /* true: usa 'fd' (a VM passa a ser dona do descritor) */
    bool readonly;
    mvm_disk_type type;
    bool empty;         /* CD/disquete sem midia (o drive existe e aceita midia depois) */
    const char *name;   /* nome para mostrar (opcional; padrao: nome do arquivo) */
} mvm_disk_config;

/* ---- rede (NAT em modo usuario: o convidado fica em 10.0.2.15/24) ---- */
typedef enum {
    MVM_NIC_NONE = 0,
    MVM_NIC_AUTO = 1,    /* PC com BIOS: RTL8139; boot direto/ARM: virtio-net */
    MVM_NIC_RTL8139 = 2, /* Realtek 8139 (XP, ReactOS, Linux) */
    MVM_NIC_E1000 = 3,   /* Intel 82540EM (Windows 7+, Linux, ReactOS) */
    MVM_NIC_VIRTIO = 4,  /* virtio-net (Linux) */
} mvm_nic_model;

#define MVM_MAX_FORWARDS 16

typedef struct {
    bool udp;            /* false = TCP */
    bool lan;            /* true: escuta em todas as interfaces; false: so 127.0.0.1 */
    uint16_t host_port;
    uint16_t guest_port;
} mvm_port_forward;

typedef struct {
    mvm_nic_model model;
    bool has_mac;
    uint8_t mac[6];      /* padrao 52:54:00:12:34:56 */
    const char *dns;     /* servidor DNS do host (IPv4); NULL = /etc/resolv.conf ou 8.8.8.8 */
    int nforwards;
    mvm_port_forward forwards[MVM_MAX_FORWARDS];
} mvm_net_config;

/* ---- som ---- */
typedef enum {
    MVM_SND_NONE = 0,
    MVM_SND_AUTO = 1,    /* x86: AC'97; ARM: virtio-sound */
    MVM_SND_AC97 = 2,    /* Intel 82801AA AC'97 (XP, ReactOS, Linux) */
    MVM_SND_HDA = 3,     /* Intel HD Audio (Windows Vista+, Linux) */
    MVM_SND_VIRTIO = 4,  /* virtio-sound (Linux) */
} mvm_snd_model;

typedef struct {
    mvm_snd_model model;
    bool mic;            /* entrada de audio (microfone) */
} mvm_audio_config;

typedef struct {
    mvm_arch arch;
    uint32_t ram_mb;          /* memoria do convidado em MiB */
    const char *kernel;       /* kernel Linux (bzImage/zImage/Image[.gz]) ou ELF/binario bruto */
    const char *initrd;       /* opcional */
    const char *cmdline;      /* linha de comando do kernel */
    const char *dtb;          /* ARM: DTB externo opcional (senao e gerado) */
    const char *firmware;     /* x86: imagem de BIOS (ex.: SeaBIOS bios-256k.bin) - opcional */
    const char *vga_bios;     /* x86: ROM de video (ex.: vgabios-stdvga.bin); padrao: ao lado da BIOS */
    const char *boot_order;   /* x86 com BIOS: "c" disco, "d" CD-ROM, "n" nenhum... (padrao "cd" ou "dc") */
    uint64_t raw_load_addr;   /* endereco de carga p/ binario bruto (0 = padrao) */
    mvm_disk_config disks[MVM_MAX_DISKS];
    uint32_t fb_width;        /* 0 = sem framebuffer */
    uint32_t fb_height;
    int64_t rtc_base;         /* data inicial do RTC em segundos Unix (0 = relogio do host) */
    mvm_net_config net;
    mvm_audio_config audio;
} mvm_config;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t stride;          /* bytes por linha */
    uint32_t format;          /* sempre MVM_FB_XRGB8888 por enquanto */
} mvm_fb_info;

#define MVM_FB_XRGB8888 1

typedef struct mvm_vm mvm_vm;

typedef void (*mvm_serial_cb)(void *opaque, const uint8_t *data, size_t len);
typedef void (*mvm_log_cb)(void *opaque, mvm_log_level level, const char *msg);

/* Configuracao padrao para uma arquitetura. */
void mvm_config_init(mvm_config *cfg, mvm_arch arch);

mvm_vm *mvm_create(const mvm_config *cfg, char *err, size_t errlen);
void mvm_destroy(mvm_vm *vm);

/* Executa a VM na thread atual ate parar. Retorna mvm_exit_reason. */
int mvm_run(mvm_vm *vm);

void mvm_request_stop(mvm_vm *vm);
void mvm_request_reset(mvm_vm *vm);
/* true: um reinicio pedido pelo convidado encerra mvm_run() com MVM_EXIT_REBOOT */
void mvm_set_no_reboot(mvm_vm *vm, bool on);
/* despeja o estado da CPU no log (depuracao; x86: pilha simbolizada por modulo PE) */
void mvm_debug_dump(mvm_vm *vm);
/* ---- audio (saida e microfone em 48 kHz, estereo, s16) ---- */
#define MVM_AUDIO_RATE 48000

typedef struct {
    /* a placa comecou/parou de tocar (input=false) ou de gravar (input=true) */
    void (*start)(void *opaque, bool input);
    void (*stop)(void *opaque, bool input);
    /* opcional: recebe a saida na hora (gravador de arquivo); sem ele, use mvm_audio_read */
    void (*push)(void *opaque, const int16_t *frames, size_t n);
} mvm_audio_backend;

void mvm_audio_set_backend(mvm_vm *vm, const mvm_audio_backend *be, void *opaque);
bool mvm_audio_enabled(mvm_vm *vm);
bool mvm_audio_mic_enabled(mvm_vm *vm);
/* backend puxa a saida (callback de audio do host): completa com silencio; retorna quadros reais */
size_t mvm_audio_read(mvm_vm *vm, int16_t *buf, size_t frames);
/* backend entrega o microfone */
void mvm_audio_write_input(mvm_vm *vm, const int16_t *buf, size_t frames);
void mvm_audio_set_muted(mvm_vm *vm, bool muted);
void mvm_audio_stats(mvm_vm *vm, uint64_t *underruns, uint64_t *overruns);

/* ---- servidor VNC (RFB) ---- */
typedef struct mvm_vnc mvm_vnc;
/* bind_addr: "127.0.0.1" (so este aparelho) ou "0.0.0.0" (rede local); password: ate 8 caracteres ou NULL */
mvm_vnc *mvm_vnc_start(mvm_vm *vm, const char *bind_addr, int port, const char *password, char *err, size_t errlen);
void mvm_vnc_stop(mvm_vnc *v);
int mvm_vnc_clients(mvm_vnc *v);

/* ---- midias com a VM ligada (CD/disquete: trocar; disco SATA: hot-plug) ---- */
#define MVM_MAX_MEDIA 16

typedef enum { MVM_MEDIA_HDD = 0, MVM_MEDIA_CD = 1, MVM_MEDIA_FLOPPY = 2 } mvm_media_kind;
typedef enum { MVM_BUS_IDE = 0, MVM_BUS_SATA = 1, MVM_BUS_FDC = 2, MVM_BUS_VIRTIO = 3 } mvm_media_bus;

typedef struct {
    mvm_media_kind kind;
    mvm_media_bus bus;
    int unit;           /* IDE: barramento*2+unidade; SATA: porta; FDC: drive */
    bool present;       /* tem midia */
    bool readonly;
    bool changeable;    /* aceita troca com a VM ligada */
    char name[64];
} mvm_media_info;

/* lista os drives; devolve quantos */
int mvm_media_list(mvm_vm *vm, mvm_media_info *out, int max);
/* poe midia no drive 'slot' (CD/disquete; disco num slot SATA vazio). fd >= 0 usa o descritor
 * (a VM passa a ser dona dele). Retorna 0 ou -1 com err. */
int mvm_media_insert(mvm_vm *vm, int slot, const char *path, int fd, bool readonly, const char *name, char *err,
                     size_t errlen);
/* tira a midia (CD/disquete) ou desconecta o disco SATA */
int mvm_media_eject(mvm_vm *vm, int slot, char *err, size_t errlen);
/* conecta um disco novo numa porta SATA livre; devolve o slot ou -1 */
int mvm_media_add_disk(mvm_vm *vm, const char *path, int fd, bool readonly, const char *name, char *err,
                       size_t errlen);

void mvm_pause(mvm_vm *vm);
void mvm_resume(mvm_vm *vm);
bool mvm_is_paused(mvm_vm *vm);

/* Console serial (UART principal). O callback e chamado na thread da VM. */
void mvm_set_serial_output(mvm_vm *vm, mvm_serial_cb cb, void *opaque);
size_t mvm_serial_input(mvm_vm *vm, const uint8_t *data, size_t len);

/* Framebuffer. Retorna false se nao houver. */
bool mvm_fb_info_get(mvm_vm *vm, mvm_fb_info *info);
/* Contador incrementado quando o convidado escreve no framebuffer. */
uint32_t mvm_fb_generation(mvm_vm *vm);
/* Copia o framebuffer (XRGB8888, linhas contiguas de largura*4 bytes) para dst.
 * Com BIOS o tamanho muda com o modo de video: 'info' recebe o tamanho copiado.
 * Retorna false se nao houver framebuffer ou se dst_size for insuficiente
 * (nesse caso 'info' traz o tamanho necessario). */
bool mvm_fb_copy(mvm_vm *vm, void *dst, size_t dst_size, mvm_fb_info *info);

/* Formatos de destino de mvm_fb_copy_rows. */
typedef enum {
    MVM_FB_COPY_XRGB, /* como o framebuffer (uint32 0x00RRGGBB) */
    MVM_FB_COPY_ARGB, /* uint32 0xFFRRGGBB (android.graphics.Color) */
    MVM_FB_COPY_RGBA, /* bytes R,G,B,0xFF (Bitmap ARGB_8888 / RGBA_8888) */
} mvm_fb_copy_fmt;

/* Copia so as linhas alteradas desde a geracao *since (copia tudo na primeira vez,
 * se o historico nao cobrir o intervalo ou se o tamanho mudou) para dst (dst_w x dst_h,
 * dst_stride bytes por linha) e atualiza *since. Devolve 1 se copiou, com as linhas em
 * [*y0, *y1); 0 se nada mudou; -1 se o tamanho do destino nao bate com o framebuffer
 * ('info' traz o tamanho atual) ou se nao ha framebuffer. */
int mvm_fb_copy_rows(mvm_vm *vm, void *dst, uint32_t dst_w, uint32_t dst_h, size_t dst_stride,
                     mvm_fb_copy_fmt fmt, uint32_t *since, uint32_t *y0, uint32_t *y1, mvm_fb_info *info);

/* Conteudo da tela em modo texto VGA (ASCII, linhas separadas por '\n').
 * Retorna o numero de bytes ou 0 se a tela nao estiver em modo texto. */
size_t mvm_text_screen(mvm_vm *vm, char *buf, size_t len);

/* Entrada. keycode = codigo evdev do Linux (KEY_A = 30, ...). */
void mvm_key_event(mvm_vm *vm, uint32_t keycode, bool pressed);
/* Botao de energia ACPI (x86): o sistema convidado normalmente se desliga. */
void mvm_power_button(mvm_vm *vm);
void mvm_pointer_event(mvm_vm *vm, int dx, int dy, int dz, uint32_t buttons);

/* Estatisticas */
uint64_t mvm_instruction_count(mvm_vm *vm);
/* Quanto o relogio do convidado ficou para tras do tempo real (ns) porque o
 * emulador nao acompanhou os timers (o convidado ve o tempo andar mais devagar). */
int64_t mvm_clock_lag_ns(mvm_vm *vm);

const char *mvm_arch_name(mvm_arch arch);
int mvm_arch_from_name(const char *name); /* -1 se desconhecida */

void mvm_set_log_callback(mvm_log_cb cb, void *opaque);
void mvm_set_log_level(mvm_log_level level);

/* ---- imagens de disco (raw, qcow2, VDI, VMDK, VHD, VHDX e o formato proprio MVD) ---- */

typedef struct {
    char format[16];       /* "raw", "qcow2", "vdi", "vmdk", "vhd", "vhdx", "mvd" */
    uint64_t virtual_size; /* bytes vistos pelo convidado */
    uint64_t file_size;    /* bytes ocupados pelo arquivo */
    bool writable;         /* o formato/arquivo aceita escrita */
} mvm_image_info;

enum { MVM_IMAGE_RAW = 0, MVM_IMAGE_MVD = 1 };
#define MVM_IMAGE_COMPRESS 1u /* MVD: blocos comprimidos com LZ4 (so leitura rapida; escrita reescreve) */

/* devolve false para cancelar */
typedef bool (*mvm_progress_cb)(void *opaque, uint64_t done, uint64_t total);

/* path ou, se path == NULL, fd (nao e fechado). 0 = ok, -1 = erro em err. */
int mvm_image_info_get(const char *path, int fd, mvm_image_info *info, char *err, size_t errlen);
/* cria uma imagem MVD vazia; block_size 0 = padrao (256 KiB); backing: arquivo base
 * (caminho relativo ao da imagem nova) ou NULL */
int mvm_image_create(const char *path, uint64_t size, uint32_t block_size, const char *backing, char *err,
                     size_t errlen);
/* converte src (path, ou fd se src == NULL) para dst no formato fmt (MVM_IMAGE_*).
 * Grava em dst.part e renomeia no fim; blocos zerados nao ocupam espaco. */
int mvm_image_convert(const char *src, int src_fd, const char *dst, int fmt, unsigned flags, uint32_t block_size,
                      mvm_progress_cb cb, void *opaque, char *err, size_t errlen);
/* le a imagem inteira verificando a estrutura; relatorio legivel em report */
int mvm_image_check(const char *path, char *report, size_t report_len, mvm_progress_cb cb, void *opaque);

#ifdef __cplusplus
}
#endif
#endif
