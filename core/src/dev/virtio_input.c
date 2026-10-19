/*
 * virtio-input (id 18): teclado e mouse para a maquina ARM "virt", que nao tem PS/2.
 * Fila 0 = eventos (dispositivo -> convidado, 8 bytes: le16 tipo, le16 codigo, le32 valor),
 * fila 1 = status (LEDs do convidado; ignorado). A configuracao responde por "select/subsel":
 * nome, IDs, bitmaps de tipos/codigos suportados. Os codigos de tecla ja chegam em evdev.
 * O Linux usa o driver virtio_input; o edk2 (VirtioKeyboardDxe) usa o teclado no firmware/GRUB.
 */
#include "devices.h"

#include <stdlib.h>

#define VIRTIO_F_VERSION_1 32

#define CFG_ID_NAME 0x01
#define CFG_ID_SERIAL 0x02
#define CFG_ID_DEVIDS 0x03
#define CFG_PROP_BITS 0x10
#define CFG_EV_BITS 0x11
#define CFG_ABS_INFO 0x12

#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_LED 0x11
#define EV_REP 0x14
#define REL_X 0x00
#define REL_Y 0x01
#define REL_WHEEL 0x08
#define BTN_LEFT 0x110

#define EVQ_MAX 512 /* eventos guardados enquanto o convidado nao poe buffers */

typedef struct {
    uint16_t type, code;
    int32_t value;
} vin_event;

typedef struct {
    int kind; /* 0 = teclado, 1 = mouse */
    uint8_t select, subsel;
    vin_event q[EVQ_MAX];
    unsigned head, count;
    uint32_t buttons;
} vinput;

static void set_bit(uint8_t *bm, unsigned n) { bm[n / 8] |= (uint8_t)(1u << (n % 8)); }

/* monta a uniao de configuracao para o select atual; retorna o tamanho */
static unsigned cfg_payload(vinput *v, uint8_t *u)
{
    memset(u, 0, 128);
    const char *name = v->kind ? "MultiVM Mouse" : "MultiVM Keyboard";
    switch (v->select) {
    case CFG_ID_NAME:
        if (v->subsel)
            return 0;
        memcpy(u, name, strlen(name));
        return (unsigned)strlen(name);
    case CFG_ID_SERIAL:
        return 0;
    case CFG_ID_DEVIDS: /* BUS_VIRTUAL, vendor/produto do QEMU (0x0627), versao 1 */
        if (v->subsel)
            return 0;
        u[0] = 0x06;
        u[2] = 0x27; u[3] = 0x06;
        u[4] = (uint8_t)(v->kind ? 2 : 1);
        u[6] = 1;
        return 8;
    case CFG_PROP_BITS:
        return 0;
    case CFG_EV_BITS:
        if (v->kind == 0) {
            if (v->subsel == EV_KEY) {
                for (unsigned k = 1; k < 0x100; k++) /* KEY_ESC .. teclas multimidia comuns */
                    set_bit(u, k);
                return 32;
            }
            if (v->subsel == EV_REP)
                return 1; /* o convidado faz a repeticao automatica */
            if (v->subsel == EV_LED) {
                u[0] = 0x07; /* NUML, CAPSL, SCROLLL */
                return 1;
            }
            return 0;
        }
        if (v->subsel == EV_KEY) {
            for (unsigned b = 0; b < 5; b++) /* LEFT, RIGHT, MIDDLE, SIDE, EXTRA */
                set_bit(u, BTN_LEFT + b);
            return (BTN_LEFT + 5 + 7) / 8;
        }
        if (v->subsel == EV_REL) {
            set_bit(u, REL_X);
            set_bit(u, REL_Y);
            set_bit(u, REL_WHEEL);
            return 2;
        }
        return 0;
    default:
        return 0;
    }
}

static uint32_t vin_cfg_read(virtio_dev *d, uint32_t off, unsigned size)
{
    vinput *v = d->priv;
    uint8_t cfg[136] = {0};
    cfg[0] = v->select;
    cfg[1] = v->subsel;
    cfg[2] = (uint8_t)cfg_payload(v, cfg + 8);
    uint32_t r = 0;
    for (unsigned i = 0; i < size && off + i < sizeof(cfg); i++)
        r |= (uint32_t)cfg[off + i] << (8 * i);
    return r;
}

static void vin_cfg_write(virtio_dev *d, uint32_t off, uint32_t val, unsigned size)
{
    vinput *v = d->priv;
    for (unsigned i = 0; i < size; i++) {
        uint8_t b = (uint8_t)(val >> (8 * i));
        if (off + i == 0)
            v->select = b;
        else if (off + i == 1)
            v->subsel = b;
    }
}

/* entrega os eventos guardados enquanto houver buffers */
static void vin_flush(virtio_dev *d)
{
    vinput *v = d->priv;
    if (!(d->status & 4) || !d->vq[0].ready) /* DRIVER_OK */
        return;
    bool any = false;
    while (v->count) {
        virtq_elem e;
        if (!virtq_pop(d, 0, &e))
            break;
        vin_event *ev = &v->q[v->head];
        uint8_t raw[8];
        raw[0] = (uint8_t)ev->type; raw[1] = (uint8_t)(ev->type >> 8);
        raw[2] = (uint8_t)ev->code; raw[3] = (uint8_t)(ev->code >> 8);
        for (int i = 0; i < 4; i++)
            raw[4 + i] = (uint8_t)((uint32_t)ev->value >> (8 * i));
        virtq_write(d, &e, 0, raw, 8);
        if (v->kind == 0 && ev->type == EV_KEY && ev->value == 1)
            atomic_fetch_add(&d->vm->kbd_delivered, 1);
        virtq_push(d, 0, &e, 8);
        v->head = (v->head + 1) % EVQ_MAX;
        v->count--;
        any = true;
    }
    if (any)
        virtio_notify_irq(d);
}

static void vin_notify(virtio_dev *d, int q)
{
    if (q == 1) { /* status (LEDs): so devolve os buffers */
        virtq_elem e;
        bool any = false;
        while (virtq_pop(d, 1, &e)) {
            virtq_push(d, 1, &e, 0);
            any = true;
        }
        if (any)
            virtio_notify_irq(d);
        return;
    }
    vin_flush(d);
}

static void vin_reset(virtio_dev *d)
{
    vinput *v = d->priv;
    v->head = v->count = 0;
    v->buttons = 0;
}

void virtio_input_event(virtio_dev *d, uint16_t type, uint16_t code, int32_t value)
{
    vinput *v = d->priv;
    static int dbg = -1;
    if (dbg < 0)
        dbg = getenv("MVM_INPUT_DEBUG") != NULL;
    if (dbg)
        LOGI("virtio-input %d: evento %u/%u/%d status=%x fila0 pronta=%d fila=%u", v->kind, type, code, value, d->status,
             d->vq[0].ready, v->count);
    bool press = v->kind == 0 && type == EV_KEY && value == 1;
    if (!(d->status & 4)) { /* sem driver: descarta (nao acumula teclas antigas) */
        if (press)
            atomic_fetch_add(&d->vm->kbd_dropped, 1);
        return;
    }
    if (v->count == EVQ_MAX) {
        if (type != EV_SYN) {
            if (press)
                atomic_fetch_add(&d->vm->kbd_dropped, 1);
            return;
        }
        /* fila cheia: troca o ultimo por SYN para nao deixar um pacote pela metade */
        v->q[(v->head + v->count - 1) % EVQ_MAX] = (vin_event){EV_SYN, 0, 0};
    } else {
        v->q[(v->head + v->count) % EVQ_MAX] = (vin_event){type, code, value};
        v->count++;
    }
    if (type == EV_SYN)
        vin_flush(d);
}

void virtio_input_key(virtio_dev *d, uint16_t code, bool pressed)
{
    virtio_input_event(d, EV_KEY, code, pressed ? 1 : 0);
    virtio_input_event(d, EV_SYN, 0, 0);
}

void virtio_input_mouse(virtio_dev *d, int dx, int dy, int dz, uint32_t buttons)
{
    vinput *v = d->priv;
    if (dx)
        virtio_input_event(d, EV_REL, REL_X, dx);
    if (dy)
        virtio_input_event(d, EV_REL, REL_Y, dy);
    if (dz) /* app: negativo = para cima; evdev: positivo = para cima */
        virtio_input_event(d, EV_REL, REL_WHEEL, -dz);
    uint32_t ch = (buttons ^ v->buttons) & 0x1f;
    for (unsigned b = 0; b < 5; b++) /* bit 0 esquerdo, 1 direito, 2 meio (como no PS/2) = ordem do evdev */
        if (ch & (1u << b))
            virtio_input_event(d, EV_KEY, BTN_LEFT + b, (buttons >> b) & 1);
    v->buttons = buttons & 0x1f;
    virtio_input_event(d, EV_SYN, 0, 0);
}

virtio_dev *virtio_input_new(mvm_vm *vm, int kind)
{
    virtio_dev *d = calloc(1, sizeof(*d));
    vinput *v = calloc(1, sizeof(*v));
    v->kind = kind;
    d->vm = vm;
    if (kind == 0)
        vm->kbd_stats = true;
    d->priv = v;
    d->device_id = 18;
    d->host_features = 1ULL << VIRTIO_F_VERSION_1;
    d->nvq = 2;
    d->vq[0].num_max = 64;
    d->vq[1].num_max = 64;
    d->cfg_read = vin_cfg_read;
    d->cfg_write = vin_cfg_write;
    d->notify = vin_notify;
    d->reset = vin_reset;
    return d;
}
