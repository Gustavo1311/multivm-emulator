/* virtio-net: fila 0 = recepcao, fila 1 = transmissao. Os quadros vao/vem da rede em modo usuario (net/). */
#include "devices.h"

#include <stdlib.h>

#define VIRTIO_F_VERSION_1 32
#define VIRTIO_NET_F_MAC 5
#define VIRTIO_NET_F_STATUS 16

typedef struct {
    virtio_dev *d;
    mvm_net *net;
    uint8_t mac[6];
    uint8_t frame[2048];
} vnet;

/* cabecalho: 10 bytes no legado; 12 (com num_buffers) a partir da versao 1 */
static size_t hdr_len(virtio_dev *d) { return (d->guest_features >> VIRTIO_F_VERSION_1) & 1 ? 12 : 10; }

static uint32_t net_cfg_read(virtio_dev *d, uint32_t off, unsigned size)
{
    vnet *v = d->priv;
    uint8_t cfg[8];
    memcpy(cfg, v->mac, 6);
    cfg[6] = 1; /* VIRTIO_NET_S_LINK_UP */
    cfg[7] = 0;
    uint32_t r = 0;
    for (unsigned i = 0; i < size && off + i < sizeof(cfg); i++)
        r |= (uint32_t)cfg[off + i] << (8 * i);
    return r;
}

/* entrega quadros pendentes enquanto houver buffers de recepcao */
static void net_rx(virtio_dev *d)
{
    vnet *v = d->priv;
    if (!(d->status & 4) || !d->vq[0].ready) /* DRIVER_OK */
        return;
    bool any = false;
    size_t hl = hdr_len(d);
    for (;;) {
        size_t len = net_rx_peek(v->net);
        if (!len)
            break;
        virtq_elem e;
        if (!virtq_pop(d, 0, &e))
            break;
        len = net_rx_pop(v->net, v->frame, sizeof(v->frame));
        uint8_t hdr[12] = {0};
        hdr[10] = 1; /* num_buffers */
        virtq_write(d, &e, 0, hdr, hl);
        size_t w = virtq_write(d, &e, hl, v->frame, len);
        virtq_push(d, 0, &e, (uint32_t)(hl + w));
        any = true;
    }
    if (any)
        virtio_notify_irq(d);
}

static void net_tx(virtio_dev *d)
{
    vnet *v = d->priv;
    size_t hl = hdr_len(d);
    bool any = false;
    virtq_elem e;
    while (virtq_pop(d, 1, &e)) {
        size_t total = 0;
        for (int i = 0; i < e.nseg; i++)
            if (!e.seg[i].write)
                total += e.seg[i].len;
        if (total > hl && total - hl <= sizeof(v->frame)) {
            size_t n = virtq_read(d, &e, hl, v->frame, total - hl);
            net_send(v->net, v->frame, n);
        }
        virtq_push(d, 1, &e, 0);
        any = true;
    }
    if (any)
        virtio_notify_irq(d);
}

static void net_notify(virtio_dev *d, int q)
{
    if (q == 1)
        net_tx(d);
    else
        net_rx(d); /* buffers novos de recepcao */
}

static void net_rx_ready(void *opaque) { net_rx(opaque); }

virtio_dev *virtio_net_new(mvm_vm *vm, mvm_net *net)
{
    virtio_dev *d = calloc(1, sizeof(*d));
    vnet *v = calloc(1, sizeof(*v));
    v->d = d;
    v->net = net;
    net_get_mac(net, v->mac);
    d->vm = vm;
    d->priv = v;
    d->device_id = 1;
    d->host_features = (1ULL << VIRTIO_F_VERSION_1) | (1ULL << VIRTIO_NET_F_MAC) | (1ULL << VIRTIO_NET_F_STATUS);
    d->nvq = 2;
    d->vq[0].num_max = 256;
    d->vq[1].num_max = 256;
    d->cfg_read = net_cfg_read;
    d->notify = net_notify;
    net_set_client(net, net_rx_ready, d);
    return d;
}
