/* Dispositivos emulados. */
#ifndef MVM_DEVICES_H
#define MVM_DEVICES_H

#include "../internal.h"

/* Linha de interrupcao generica: o controlador recebe (opaque, n, nivel). */
typedef struct {
    void (*set)(void *opaque, int n, int level);
    void *opaque;
    int n;
} irq_line;

static inline void irq_set(const irq_line *l, int level)
{
    if (l->set)
        l->set(l->opaque, l->n, level);
}

/* ---------------------------------------------------------------- virtio */

#define VIRTIO_MAX_QUEUES 4
#define VIRTQ_DESC_F_NEXT 1
#define VIRTQ_DESC_F_WRITE 2
#define VIRTQ_DESC_F_INDIRECT 4

typedef struct {
    uint32_t num;
    uint32_t num_max;
    bool ready;
    uint64_t desc, avail, used;
    uint16_t last_avail;
} virtq;

typedef struct {
    uint64_t addr;
    uint32_t len;
    bool write;
} virtq_seg;

#define VIRTQ_MAX_SEGS 256

typedef struct {
    uint16_t head;
    int nseg;
    virtq_seg seg[VIRTQ_MAX_SEGS];
} virtq_elem;

typedef struct virtio_dev {
    mvm_vm *vm;
    uint32_t device_id;
    uint64_t host_features;
    uint64_t guest_features;
    uint32_t status;
    uint32_t isr;
    uint32_t cfg_gen;
    int nvq;
    virtq vq[VIRTIO_MAX_QUEUES];
    irq_line irq;
    /* implementacao do dispositivo */
    void *priv;
    uint32_t (*cfg_read)(struct virtio_dev *d, uint32_t off, unsigned size);
    void (*cfg_write)(struct virtio_dev *d, uint32_t off, uint32_t val, unsigned size);
    void (*notify)(struct virtio_dev *d, int q);
    void (*reset)(struct virtio_dev *d);
    void (*poll)(struct virtio_dev *d);
    void (*destroy)(struct virtio_dev *d);
    bool legacy; /* transporte legado (virtio 0.9): tabelas contiguas */
    /* trava propria (discos): o MMIO do dispositivo nao toma a bql, entao a leitura do disco nao
     * para as outras CPUs; a interrupcao e levantada depois de soltar a trava (com a bql) */
    bool own_lock;
    pthread_mutex_t mu;
    _Atomic int irq_dirty;
} virtio_dev;
void virtio_set_own_lock(virtio_dev *d);

void virtio_reset(virtio_dev *d);
bool virtq_pop(virtio_dev *d, int q, virtq_elem *e);
void virtq_push(virtio_dev *d, int q, const virtq_elem *e, uint32_t len);
void virtio_notify_irq(virtio_dev *d);
void virtio_update_irq(virtio_dev *d);
/* copia entre segmentos do convidado e buffer do host */
size_t virtq_read(virtio_dev *d, const virtq_elem *e, size_t off, void *buf, size_t len);
size_t virtq_write(virtio_dev *d, const virtq_elem *e, size_t off, const void *buf, size_t len);

virtio_dev *virtio_blk_new(mvm_vm *vm, mvm_blk *blk);
virtio_dev *virtio_input_new(mvm_vm *vm, int kind); /* 0 = teclado, 1 = mouse (tablet relativo) */
void virtio_input_event(virtio_dev *d, uint16_t type, uint16_t code, int32_t value);
void virtio_input_key(virtio_dev *d, uint16_t code, bool pressed); /* codigo evdev */
/* movimento relativo (dy positivo = para baixo), roda (negativo = para cima), botoes como no PS/2 */
void virtio_input_mouse(virtio_dev *d, int dx, int dy, int dz, uint32_t buttons);
/* virtio-gpu 2D: tela preferida pref_w x pref_h, desenhada em vm->fb */
virtio_dev *virtio_gpu_new(mvm_vm *vm, uint32_t pref_w, uint32_t pref_h);
/* nova resolucao preferida: o convidado recebe o evento de tela (config change) e reconsulta */
void virtio_gpu_set_preferred(virtio_dev *d, uint32_t w, uint32_t h);
/* virtio-scsi (ARM virt): CDs com troca de midia e discos conectaveis com a VM ligada */
virtio_dev *virtio_scsi_new(mvm_vm *vm);
int virtio_scsi_attach(virtio_dev *d, mvm_blk *blk, bool cd); /* alvo usado, ou -1 */
int virtio_scsi_free_target(virtio_dev *d);
/* CD: troca a midia (blk NULL = ejeta); disco: blk conecta no alvo, NULL desconecta */
bool virtio_scsi_change(virtio_dev *d, int target, mvm_blk *blk, bool cd);
void virtio_dev_free(virtio_dev *d);

/* transporte MMIO */
extern const mvm_io_ops virtio_mmio_ops;
void *virtio_mmio_wrap(virtio_dev *d);
void virtio_mmio_unwrap(void *opaque);

/* ---------------------------------------------------------- fw_cfg */
typedef struct fw_cfg fw_cfg;
fw_cfg *fw_cfg_new(mvm_vm *vm);
void fw_cfg_free(fw_cfg *f);
void fw_cfg_add_bytes(fw_cfg *f, uint16_t key, const void *data, uint32_t len);
void fw_cfg_add_u32(fw_cfg *f, uint16_t key, uint32_t v);
void fw_cfg_add_u64(fw_cfg *f, uint16_t key, uint64_t v);
void fw_cfg_add_file(fw_cfg *f, const char *name, const void *data, uint32_t len);
extern const mvm_io_ops fw_cfg_ops; /* portas 0x510-0x51B */
extern const mvm_io_ops fw_cfg_mmio_ops; /* ARM "virt": 0x18 bytes */
/* arquivo que o convidado pode gravar por DMA; cb recebe o conteudo inteiro depois de cada escrita */
typedef void (*fw_cfg_write_cb)(void *opaque, const uint8_t *data, uint32_t len);
void fw_cfg_add_file_rw(fw_cfg *f, const char *name, const void *data, uint32_t len, fw_cfg_write_cb cb,
                        void *opaque);

/* ---------------------------------------------------------- ARM */
typedef struct pl011 pl011;
pl011 *pl011_new(mvm_vm *vm, mvm_chardev *chr, irq_line irq);
void pl011_poll(pl011 *u);
extern const mvm_io_ops pl011_ops;

typedef struct gicv2 gicv2;
/* cpus[k] e o argumento de cpu_irq para a CPU k */
gicv2 *gicv2_new(mvm_vm *vm, void (*cpu_irq)(void *cpu, int line, int level), void *const *cpus, int ncpus);
void gicv2_set_irq(void *opaque, int irq, int level); /* irq = INTID (SPI) */
void gicv2_set_ppi(gicv2 *g, int cpu, int irq, int level); /* PPI 16-31 de uma CPU */
void gicv2_reset(gicv2 *g);
extern const mvm_io_ops gicv2_dist_ops;
extern const mvm_io_ops gicv2_cpu_ops;

typedef struct pl031 pl031;
pl031 *pl031_new(mvm_vm *vm, irq_line irq);
extern const mvm_io_ops pl031_ops;

/* ---------------------------------------------------------- x86 / PC */
typedef struct uart16550 uart16550;
uart16550 *uart16550_new(mvm_vm *vm, mvm_chardev *chr, irq_line irq);
void uart16550_poll(uart16550 *u);
extern const mvm_io_ops uart16550_ops;

typedef struct i8259 i8259;
i8259 *i8259_new(mvm_vm *vm, void (*cpu_intr)(void *cpu, int level), void *cpu);
void i8259_set_irq(void *opaque, int irq, int level);
int i8259_ack(i8259 *p); /* retorna o vetor */
bool i8259_pending(i8259 *p);
void i8259_debug_dump(i8259 *p);
void uart16550_debug_dump(uart16550 *u);
void i8259_reset(i8259 *p);
extern const mvm_io_ops i8259_master_ops, i8259_slave_ops, i8259_elcr_ops;

typedef struct i8254 i8254;
i8254 *i8254_new(mvm_vm *vm, irq_line irq0);
void i8254_reset(i8254 *p);
uint8_t i8254_port61_read(i8254 *p);
void i8254_debug_dump(i8254 *p);
void i8254_port61_write(i8254 *p, uint8_t v);
extern const mvm_io_ops i8254_ops;

typedef struct cmos cmos;
cmos *cmos_new(mvm_vm *vm, irq_line irq8);
void cmos_set(cmos *c, int idx, uint8_t v);
void cmos_reset(cmos *c);
extern const mvm_io_ops cmos_ops;

typedef struct i8042 i8042;
i8042 *i8042_new(mvm_vm *vm, irq_line kbd_irq, irq_line mouse_irq);
void i8042_key(i8042 *k, uint16_t evdev_code, bool pressed);
void i8042_mouse(i8042 *k, int dx, int dy, int dz, uint32_t buttons);
extern const mvm_io_ops i8042_ops;

/* DMA ISA 8237 (dois controladores) e registradores de pagina */
typedef struct i8237 i8237;
i8237 *i8237_new(mvm_vm *vm);
void i8237_reset(i8237 *d);
bool i8237_ready(i8237 *d, int ch);      /* canal desmascarado */
int i8237_mode(i8237 *d, int ch);
bool i8237_tc(i8237 *d, int ch);
uint32_t i8237_write_mem(i8237 *d, int ch, const uint8_t *buf, uint32_t len); /* dispositivo -> RAM */
uint32_t i8237_read_mem(i8237 *d, int ch, uint8_t *buf, uint32_t len);        /* RAM -> dispositivo */
extern const mvm_io_ops i8237_dma1_ops;  /* 0x00-0x0f */
extern const mvm_io_ops i8237_dma2_ops;  /* 0xc0-0xdf */
extern const mvm_io_ops i8237_page_ops;  /* 0x81-0x8f */

/* controlador de disquete (82078), drives A: e B: */
typedef struct fdc fdc;
fdc *fdc_new(mvm_vm *vm, irq_line irq6, i8237 *dma);
bool fdc_attach(fdc *f, int drive, mvm_blk *blk); /* blk NULL = drive vazio */
bool fdc_change_media(fdc *f, int drive, mvm_blk *blk);
bool fdc_has_drive(fdc *f, int drive);
int fdc_cmos_type(uint64_t size);        /* tipo de drive no CMOS 0x10 para a midia */
void fdc_reset(fdc *f);
void fdc_free(fdc *f);
extern const mvm_io_ops fdc_ops;         /* 0x3f0-0x3f5 */
extern const mvm_io_ops fdc_dir_ops;     /* 0x3f7 */

/* EEPROM 93C46 (Microwire) das placas de rede */
typedef struct {
    uint16_t data[64];
    int state, bits, outbits;
    uint32_t shift;
    unsigned addr;
    uint16_t out;
    bool cs, sk, dout;
} eeprom93;
void ee93_reset(eeprom93 *e);
void ee93_write(eeprom93 *e, bool cs, bool sk, bool di);

/* placas de rede (quadros pela rede em modo usuario, net/) */
virtio_dev *virtio_net_new(mvm_vm *vm, mvm_net *net);
virtio_dev *virtio_snd_new(mvm_vm *vm); /* virtio-sound (ARM) */

typedef struct rtl8139 rtl8139;
rtl8139 *rtl8139_new(mvm_vm *vm, mvm_net *net, irq_line irq);
void rtl8139_reset(rtl8139 *r);
void rtl8139_free(rtl8139 *r);
extern const mvm_io_ops rtl8139_ops; /* BAR0 (E/S) e BAR1 (MMIO), 256 bytes */

typedef struct e1000 e1000;
e1000 *e1000_new(mvm_vm *vm, mvm_net *net, irq_line irq);
void e1000_reset(e1000 *e);
void e1000_free(e1000 *e);
extern const mvm_io_ops e1000_mmio_ops; /* BAR0, 128 KiB */
extern const mvm_io_ops e1000_io_ops;   /* BAR1: IOADDR/IODATA */

/* placas de som (audio pelo host em audio.c) */
typedef struct ac97 ac97;
ac97 *ac97_new(mvm_vm *vm, irq_line irq);
void ac97_reset(ac97 *a);
void ac97_free(ac97 *a);
extern const mvm_io_ops ac97_nam_ops;  /* BAR0: mixer, 256 bytes de E/S */
extern const mvm_io_ops ac97_nabm_ops; /* BAR1: bus master, 64 bytes de E/S */

typedef struct hda hda;
hda *hda_new(mvm_vm *vm, irq_line irq);
void hda_reset(hda *h);
void hda_free(hda *h);
extern const mvm_io_ops hda_ops; /* BAR0: 16 KiB de MMIO */

#endif
