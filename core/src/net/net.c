/*
 * Rede em modo usuario (NAT no estilo slirp/QEMU): o convidado fica em
 * 10.0.2.15/24, o gateway 10.0.2.2 e o proprio host (127.0.0.1) e o DNS 10.0.2.3
 * e repassado ao servidor DNS do host. Sem tap e sem root: cada conexao do
 * convidado vira um socket comum do host.
 *
 * Uma thread de rede faz toda a pilha (poll() nos sockets). A placa emulada, na
 * thread da VM, so troca quadros Ethernet pelas filas TX/RX:
 *   net_send()    convidado -> rede
 *   net_rx_pop()  rede -> convidado (net_poll() chama rx_ready quando ha quadros)
 */
#include "net_int.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

int net_debug;

#define UDP_IDLE_MS 120000
#define DNS_IDLE_MS 15000

typedef struct udp_flow {
    struct udp_flow *next;
    int fd;
    uint16_t gport;  /* porta de origem no convidado */
    int64_t last;
    bool dns;
} udp_flow;

/* ------------------------------------------------------------ utilitarios */

int64_t net_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

uint32_t net_csum_add(uint32_t sum, const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len > 1) {
        sum += (uint32_t)(p[0] << 8 | p[1]);
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (uint32_t)(p[0] << 8);
    return sum;
}

uint16_t net_csum_fold(uint32_t sum)
{
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

uint32_t net_pseudo_sum(uint32_t src, uint32_t dst, uint8_t proto, uint16_t len)
{
    return (src >> 16) + (src & 0xffff) + (dst >> 16) + (dst & 0xffff) + proto + len;
}

static void set_nonblock(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
}

uint32_t net_map_out(mvm_net *n, uint32_t ip)
{
    if (ip == NET_GW)
        return LOCALHOST;
    if (ip == NET_DNS)
        return n->dns_host;
    return ip;
}

uint32_t net_map_in(mvm_net *n, uint32_t ip)
{
    (void)n;
    if ((ip >> 24) == 127)
        return NET_GW;
    return ip;
}

static void wake(mvm_net *n)
{
    uint64_t one = 1;
    ssize_t r = write(n->wake_fd, &one, sizeof(one));
    (void)r;
}

/* ------------------------------------------------------------ filas */

bool net_rx_room(mvm_net *n, size_t bytes)
{
    pthread_mutex_lock(&n->rxq_lock);
    unsigned used = (n->rxq_head - n->rxq_tail) % RXQ_FRAMES;
    bool ok = used < RXQ_FRAMES - 8 && n->rxq_bytes + bytes <= RXQ_LIMIT;
    pthread_mutex_unlock(&n->rxq_lock);
    if (!ok)
        atomic_store(&n->rx_waiting, 1);
    return ok;
}

/* quadro Ethernet completo para o convidado */
static void rx_push(mvm_net *n, const uint8_t *frame, size_t len)
{
    size_t padded = len < 60 ? 60 : len;
    net_frame *f = malloc(sizeof(*f) + padded);
    if (!f)
        return;
    memcpy(f->data, frame, len);
    memset(f->data + len, 0, padded - len);
    f->len = padded;
    pthread_mutex_lock(&n->rxq_lock);
    unsigned used = (n->rxq_head - n->rxq_tail) % RXQ_FRAMES;
    if (used >= RXQ_FRAMES - 1) {
        pthread_mutex_unlock(&n->rxq_lock);
        free(f);
        return;
    }
    n->rxq[n->rxq_head % RXQ_FRAMES] = f;
    n->rxq_head++;
    n->rxq_bytes += padded;
    pthread_mutex_unlock(&n->rxq_lock);
    atomic_store(&n->rx_pending, 1);
    vm_kick(n->vm, true);
}

static void send_eth(mvm_net *n, const uint8_t *dst_mac, uint16_t type, const uint8_t *payload, size_t len)
{
    uint8_t buf[ETH_HLEN + NET_MTU + 64];
    if (len > sizeof(buf) - ETH_HLEN)
        return;
    memcpy(buf, dst_mac, 6);
    memcpy(buf + 6, n->gw_mac, 6);
    wr_be16(buf + 12, type);
    memcpy(buf + ETH_HLEN, payload, len);
    rx_push(n, buf, ETH_HLEN + len);
}

static void send_ip_to(mvm_net *n, const uint8_t *dst_mac, uint8_t proto, uint32_t src, uint32_t dst,
                       const uint8_t *l4, size_t len)
{
    uint8_t pkt[NET_MTU + 64];
    if (len + IP_HLEN > sizeof(pkt))
        return;
    uint8_t *ip = pkt;
    ip[0] = 0x45;
    ip[1] = 0;
    wr_be16(ip + 2, (uint16_t)(IP_HLEN + len));
    wr_be16(ip + 4, n->ip_id++);
    wr_be16(ip + 6, 0x4000); /* DF */
    ip[8] = 64;
    ip[9] = proto;
    wr_be16(ip + 10, 0);
    wr_be32(ip + 12, src);
    wr_be32(ip + 16, dst);
    wr_be16(ip + 10, net_csum_fold(net_csum_add(0, ip, IP_HLEN)));
    memcpy(pkt + IP_HLEN, l4, len);
    send_eth(n, dst_mac, 0x0800, pkt, IP_HLEN + len);
}

void net_send_ip(mvm_net *n, uint8_t proto, uint32_t src, uint32_t dst, const uint8_t *l4, size_t len)
{
    send_ip_to(n, n->guest_mac, proto, src, dst, l4, len);
}

/* UDP para o convidado (monta o cabecalho e o checksum) */
static void send_udp(mvm_net *n, const uint8_t *dst_mac, uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport,
                     const uint8_t *data, size_t len)
{
    uint8_t seg[NET_MTU];
    if (len + 8 > NET_MTU - IP_HLEN)
        return; /* sem fragmentacao: datagramas grandes sao descartados */
    wr_be16(seg, sport);
    wr_be16(seg + 2, dport);
    wr_be16(seg + 4, (uint16_t)(8 + len));
    wr_be16(seg + 6, 0);
    memcpy(seg + 8, data, len);
    uint16_t c = net_csum_fold(net_csum_add(net_pseudo_sum(src, dst, IPPROTO_UDP_, (uint16_t)(8 + len)), seg, 8 + len));
    wr_be16(seg + 6, c ? c : 0xffff);
    send_ip_to(n, dst_mac, IPPROTO_UDP_, src, dst, seg, 8 + len);
}

/* ------------------------------------------------------------ ARP */

static void arp_input(mvm_net *n, const uint8_t *p, size_t len)
{
    if (len < 28 || rd_be16(p) != 1 || rd_be16(p + 2) != 0x0800 || rd_be16(p + 6) != 1)
        return;
    uint32_t sender = rd_be32(p + 14), target = rd_be32(p + 24);
    if (sender == NET_GUEST)
        memcpy(n->guest_mac, p + 8, 6);
    /* responde por toda a rede 10.0.2.0/24, menos pelo proprio convidado (deteccao de conflito) */
    if ((target & NET_MASK) != NET_NET || target == NET_GUEST || target == sender)
        return;
    uint8_t r[28];
    wr_be16(r, 1);
    wr_be16(r + 2, 0x0800);
    r[4] = 6;
    r[5] = 4;
    wr_be16(r + 6, 2);
    memcpy(r + 8, n->gw_mac, 6);
    wr_be32(r + 14, target);
    memcpy(r + 18, p + 8, 6);
    memcpy(r + 24, p + 14, 4);
    send_eth(n, p + 8, 0x0806, r, sizeof(r));
}

/* ------------------------------------------------------------ DHCP */

static void dhcp_input(mvm_net *n, const uint8_t *src_mac, const uint8_t *p, size_t len)
{
    if (len < 240 || p[0] != 1 || rd_be32(p + 236) != 0x63825363)
        return;
    int type = 0;
    uint32_t requested = 0;
    for (size_t i = 240; i < len;) {
        uint8_t opt = p[i];
        if (opt == 255)
            break;
        if (opt == 0) {
            i++;
            continue;
        }
        if (i + 1 >= len || i + 2 + p[i + 1] > len)
            break;
        uint8_t ol = p[i + 1];
        if (opt == 53 && ol >= 1)
            type = p[i + 2];
        if (opt == 50 && ol == 4)
            requested = rd_be32(p + i + 2);
        i += 2 + ol;
    }
    int reply;
    if (type == 1)
        reply = 2; /* DISCOVER -> OFFER */
    else if (type == 3)
        reply = requested && requested != NET_GUEST ? 6 : 5; /* REQUEST -> ACK (ou NAK) */
    else if (type == 8)
        reply = 5; /* INFORM -> ACK */
    else
        return;
    NETDBG("DHCP %s -> %s", type == 1 ? "DISCOVER" : type == 3 ? "REQUEST" : "INFORM",
           reply == 2 ? "OFFER" : reply == 5 ? "ACK" : "NAK");
    memcpy(n->guest_mac, p + 28, 6);

    uint8_t r[300];
    memset(r, 0, sizeof(r));
    r[0] = 2;
    r[1] = 1;
    r[2] = 6;
    memcpy(r + 4, p + 4, 4);   /* xid */
    memcpy(r + 10, p + 10, 2); /* flags */
    if (type == 8)
        memcpy(r + 12, p + 12, 4); /* ciaddr */
    else if (reply != 6)
        wr_be32(r + 16, NET_GUEST); /* yiaddr */
    wr_be32(r + 20, NET_GW);   /* siaddr */
    memcpy(r + 28, p + 28, 16); /* chaddr */
    wr_be32(r + 236, 0x63825363);
    uint8_t *o = r + 240;
    *o++ = 53; *o++ = 1; *o++ = (uint8_t)reply;
    *o++ = 54; *o++ = 4; wr_be32(o, NET_GW); o += 4;
    if (reply != 6) {
        if (type != 8) {
            *o++ = 51; *o++ = 4; wr_be32(o, 86400); o += 4;
        }
        *o++ = 1; *o++ = 4; wr_be32(o, NET_MASK); o += 4;
        *o++ = 3; *o++ = 4; wr_be32(o, NET_GW); o += 4;
        *o++ = 6; *o++ = 4; wr_be32(o, NET_DNS); o += 4;
        *o++ = 28; *o++ = 4; wr_be32(o, NET_NET | 0xff); o += 4;
    }
    *o++ = 255;
    static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    (void)src_mac;
    send_udp(n, bcast, NET_GW, 67, 0xffffffffu, 68, r, sizeof(r));
}

/* ------------------------------------------------------------ UDP (NAT) */

static udp_flow *udp_flow_get(mvm_net *n, uint16_t gport, bool dns)
{
    for (udp_flow *f = n->udp; f; f = f->next)
        if (f->gport == gport && f->dns == dns)
            return f;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return NULL;
    set_nonblock(fd);
    udp_flow *f = calloc(1, sizeof(*f));
    f->fd = fd;
    f->gport = gport;
    f->dns = dns;
    f->next = n->udp;
    n->udp = f;
    return f;
}

static void udp_input(mvm_net *n, const uint8_t *src_mac, uint32_t src, uint32_t dst, const uint8_t *p, size_t len)
{
    if (len < 8)
        return;
    uint16_t sport = rd_be16(p), dport = rd_be16(p + 2);
    size_t ulen = rd_be16(p + 4);
    if (ulen < 8 || ulen > len)
        return;
    const uint8_t *data = p + 8;
    size_t dlen = ulen - 8;

    if (dport == 67) {
        dhcp_input(n, src_mac, data, dlen);
        return;
    }
    if (dst == 0xffffffffu || (dst >> 28) == 0xe || dst == (NET_NET | 0xff))
        return; /* broadcast/multicast (NetBIOS, mDNS, SSDP...) */
    if (src != NET_GUEST && src != 0)
        return;

    /* resposta do convidado para uma porta UDP redirecionada */
    if (dst == NET_GW) {
        for (int i = 0; i < n->cfg.nforwards; i++) {
            mvm_port_forward *fw = &n->cfg.forwards[i];
            if (fw->udp && fw->guest_port == sport && n->listen_fd[i] >= 0 && n->udp_fwd_peer[i]) {
                sendto(n->listen_fd[i], data, dlen, 0, (struct sockaddr *)n->udp_fwd_peer[i], sizeof(struct sockaddr_in));
                return;
            }
        }
    }

    bool dns = dst == NET_DNS;
    if ((dst & NET_MASK) == NET_NET && dst != NET_GW && !dns)
        return;
    if (dns && dport != 53)
        return;
    udp_flow *f = udp_flow_get(n, sport, dns);
    if (!f)
        return;
    f->last = net_now_ms();
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(dport)};
    to.sin_addr.s_addr = htonl(net_map_out(n, dst));
    sendto(f->fd, data, dlen, 0, (struct sockaddr *)&to, sizeof(to));
}

static void udp_readable(mvm_net *n, udp_flow *f)
{
    uint8_t buf[NET_MTU];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        ssize_t r = recvfrom(f->fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (r < 0)
            return;
        f->last = net_now_ms();
        uint32_t src = f->dns ? NET_DNS : net_map_in(n, ntohl(from.sin_addr.s_addr));
        uint16_t sport = f->dns ? 53 : ntohs(from.sin_port);
        if (!net_rx_room(n, (size_t)r + 64))
            continue; /* fila cheia: UDP pode perder */
        send_udp(n, n->guest_mac, src, sport, NET_GUEST, f->gport, buf, (size_t)r);
    }
}

static void udp_expire(mvm_net *n, int64_t now)
{
    for (udp_flow **pp = &n->udp; *pp;) {
        udp_flow *f = *pp;
        if (now - f->last > (f->dns ? DNS_IDLE_MS : UDP_IDLE_MS)) {
            *pp = f->next;
            close(f->fd);
            free(f);
        } else {
            pp = &f->next;
        }
    }
}

/* datagrama numa porta UDP redirecionada do host */
static void udp_forward_readable(mvm_net *n, int i)
{
    uint8_t buf[NET_MTU];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        ssize_t r = recvfrom(n->listen_fd[i], buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (r < 0)
            return;
        if (!n->udp_fwd_peer[i])
            n->udp_fwd_peer[i] = malloc(sizeof(struct sockaddr_in));
        *n->udp_fwd_peer[i] = from;
        if (net_rx_room(n, (size_t)r + 64))
            send_udp(n, n->guest_mac, NET_GW, ntohs(from.sin_port), NET_GUEST, n->cfg.forwards[i].guest_port, buf,
                     (size_t)r);
    }
}

/* ------------------------------------------------------------ ICMP */

static void icmp_input(mvm_net *n, uint32_t src, uint32_t dst, const uint8_t *p, size_t len)
{
    if (len < 8 || p[0] != 8)
        return; /* so echo request */
    if (dst == NET_GW || dst == NET_DNS) {
        uint8_t r[NET_MTU];
        if (len > sizeof(r))
            return;
        memcpy(r, p, len);
        r[0] = 0;
        wr_be16(r + 2, 0);
        wr_be16(r + 2, net_csum_fold(net_csum_add(0, r, len)));
        net_send_ip(n, IPPROTO_ICMP_, dst, src, r, len);
        return;
    }
    if ((dst & NET_MASK) == NET_NET || n->icmp_fd < 0)
        return;
    /* ping de verdade por socket ICMP sem privilegio (o kernel troca o identificador) */
    int slot = n->ping_next++ % 64;
    n->pings[slot].dst = dst;
    n->pings[slot].seq = rd_be16(p + 6);
    n->pings[slot].guest_id = rd_be16(p + 4);
    n->pings[slot].used = true;
    struct sockaddr_in to = {.sin_family = AF_INET};
    to.sin_addr.s_addr = htonl(dst);
    sendto(n->icmp_fd, p, len, 0, (struct sockaddr *)&to, sizeof(to));
}

static void icmp_readable(mvm_net *n)
{
    uint8_t buf[NET_MTU];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        ssize_t r = recvfrom(n->icmp_fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (r < 8)
            return;
        if (buf[0] != 0)
            continue;
        uint32_t src = ntohl(from.sin_addr.s_addr);
        uint16_t seq = rd_be16(buf + 6);
        for (int i = 0; i < 64; i++) {
            if (n->pings[i].used && n->pings[i].dst == src && n->pings[i].seq == seq) {
                n->pings[i].used = false;
                wr_be16(buf + 4, n->pings[i].guest_id);
                wr_be16(buf + 2, 0);
                wr_be16(buf + 2, net_csum_fold(net_csum_add(0, buf, (size_t)r)));
                if (net_rx_room(n, (size_t)r + 64))
                    net_send_ip(n, IPPROTO_ICMP_, src, NET_GUEST, buf, (size_t)r);
                break;
            }
        }
    }
}

/* ------------------------------------------------------------ entrada do convidado */

static void ip_input(mvm_net *n, const uint8_t *src_mac, const uint8_t *p, size_t len)
{
    if (len < IP_HLEN || (p[0] >> 4) != 4)
        return;
    size_t ihl = (size_t)(p[0] & 15) * 4;
    size_t tot = rd_be16(p + 2);
    if (ihl < IP_HLEN || tot < ihl || tot > len)
        return;
    if (rd_be16(p + 6) & 0x3fff)
        return; /* fragmentos: nao suportados (o convidado usa DF com MTU 1500) */
    uint32_t src = rd_be32(p + 12), dst = rd_be32(p + 16);
    if (src == NET_GUEST)
        memcpy(n->guest_mac, src_mac, 6);
    const uint8_t *l4 = p + ihl;
    size_t l4len = tot - ihl;
    switch (p[9]) {
    case IPPROTO_UDP_: udp_input(n, src_mac, src, dst, l4, l4len); break;
    case IPPROTO_TCP_:
        if (src == NET_GUEST)
            tcp_input(n, src, dst, l4, l4len);
        break;
    case IPPROTO_ICMP_:
        if (src == NET_GUEST)
            icmp_input(n, src, dst, l4, l4len);
        break;
    }
}

static void eth_input(mvm_net *n, const uint8_t *f, size_t len)
{
    if (len < ETH_HLEN)
        return;
    uint16_t type = rd_be16(f + 12);
    if (type == 0x0806)
        arp_input(n, f + ETH_HLEN, len - ETH_HLEN);
    else if (type == 0x0800)
        ip_input(n, f + 6, f + ETH_HLEN, len - ETH_HLEN);
}

/* ------------------------------------------------------------ thread */

static void process_tx(mvm_net *n)
{
    for (;;) {
        net_frame *batch[64];
        int cnt = 0;
        pthread_mutex_lock(&n->txq_lock);
        while (cnt < 64 && n->txq_n > 0) {
            batch[cnt++] = n->txq[0];
            memmove(n->txq, n->txq + 1, (size_t)(n->txq_n - 1) * sizeof(*n->txq));
            n->txq_n--;
        }
        pthread_mutex_unlock(&n->txq_lock);
        if (!cnt)
            return;
        for (int i = 0; i < cnt; i++) {
            eth_input(n, batch[i]->data, batch[i]->len);
            free(batch[i]);
        }
    }
}

enum { OWN_WAKE = 1, OWN_ICMP, OWN_UDP, OWN_LISTEN, OWN_TCP };

#define MAX_FDS 512

static void *net_thread(void *opaque)
{
    mvm_net *n = opaque;
    struct pollfd fds[MAX_FDS];
    void *owner[MAX_FDS];
    int kind[MAX_FDS];
    int64_t last_expire = net_now_ms();

    while (!atomic_load(&n->quit)) {
        int nf = 0;
        fds[nf] = (struct pollfd){n->wake_fd, POLLIN, 0};
        kind[nf++] = OWN_WAKE;
        if (n->icmp_fd >= 0) {
            fds[nf] = (struct pollfd){n->icmp_fd, POLLIN, 0};
            kind[nf++] = OWN_ICMP;
        }
        for (int i = 0; i < n->cfg.nforwards && nf < MAX_FDS; i++) {
            if (n->listen_fd[i] < 0)
                continue;
            fds[nf] = (struct pollfd){n->listen_fd[i], POLLIN, 0};
            owner[nf] = (void *)(intptr_t)i;
            kind[nf++] = OWN_LISTEN;
        }
        for (udp_flow *f = n->udp; f && nf < MAX_FDS; f = f->next) {
            fds[nf] = (struct pollfd){f->fd, POLLIN, 0};
            owner[nf] = f;
            kind[nf++] = OWN_UDP;
        }
        int t = tcp_pollfds(n, fds + nf, owner + nf, MAX_FDS - nf);
        for (int i = 0; i < t; i++)
            kind[nf + i] = OWN_TCP;
        nf += t;

        int64_t now = net_now_ms();
        int64_t next = tcp_next_timer(n);
        int timeout = 200;
        if (next && next - now < timeout)
            timeout = next - now < 0 ? 0 : (int)(next - now);
        int r = poll(fds, (nfds_t)nf, timeout);
        if (r < 0 && errno != EINTR)
            break;

        for (int i = 0; i < nf && r > 0; i++) {
            if (!fds[i].revents)
                continue;
            switch (kind[i]) {
            case OWN_WAKE: {
                uint64_t v;
                ssize_t rr = read(n->wake_fd, &v, sizeof(v));
                (void)rr;
                break;
            }
            case OWN_ICMP: icmp_readable(n); break;
            case OWN_UDP: udp_readable(n, owner[i]); break;
            case OWN_LISTEN: {
                int idx = (int)(intptr_t)owner[i];
                if (n->cfg.forwards[idx].udp)
                    udp_forward_readable(n, idx);
                else
                    tcp_accept_forward(n, n->listen_fd[idx], n->cfg.forwards[idx].guest_port);
                break;
            }
            case OWN_TCP: tcp_events(n, owner[i], fds[i].revents); break;
            }
        }
        process_tx(n);
        now = net_now_ms();
        tcp_timers(n, now);
        atomic_store(&n->rx_waiting, 0);
        tcp_output_all(n);
        if (now - last_expire > 1000) {
            udp_expire(n, now);
            last_expire = now;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------ API */

static uint32_t host_dns(const char *cfg)
{
    struct in_addr a;
    if (cfg && inet_pton(AF_INET, cfg, &a) == 1)
        return ntohl(a.s_addr);
    FILE *f = fopen("/etc/resolv.conf", "r");
    if (f) {
        char line[256], ip[64];
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "nameserver %63s", ip) == 1 && inet_pton(AF_INET, ip, &a) == 1) {
                fclose(f);
                return ntohl(a.s_addr);
            }
        }
        fclose(f);
    }
    return NET_ADDR(8, 8, 8, 8);
}

mvm_net *net_new(mvm_vm *vm, const mvm_net_config *cfg, char *err, size_t errlen)
{
    mvm_net *n = calloc(1, sizeof(*n));
    n->vm = vm;
    const char *dbg = getenv("MVM_NET_DEBUG");
    net_debug = dbg && atoi(dbg) > 0;
    n->cfg = *cfg;
    static const uint8_t def_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    memcpy(n->guest_mac, cfg->has_mac ? cfg->mac : def_mac, 6);
    memcpy(n->cfg.mac, n->guest_mac, 6);
    static const uint8_t gw_mac[6] = {0x52, 0x55, 0x0a, 0x00, 0x02, 0x02};
    memcpy(n->gw_mac, gw_mac, 6);
    n->dns_host = host_dns(cfg->dns);
    n->fwd_port_next = 40000;
    srand((unsigned)net_now_ms());
    pthread_mutex_init(&n->txq_lock, NULL);
    pthread_mutex_init(&n->rxq_lock, NULL);
    n->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (n->wake_fd < 0) {
        snprintf(err, errlen, "rede: eventfd falhou");
        net_free(n);
        return NULL;
    }
    n->icmp_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (n->icmp_fd >= 0)
        set_nonblock(n->icmp_fd);
    else
        LOGI("rede: sem socket ICMP (ping so responde pelo gateway)");

    for (int i = 0; i < MVM_MAX_FORWARDS; i++)
        n->listen_fd[i] = -1;
    for (int i = 0; i < cfg->nforwards && i < MVM_MAX_FORWARDS; i++) {
        const mvm_port_forward *fw = &cfg->forwards[i];
        int fd = socket(AF_INET, fw->udp ? SOCK_DGRAM : SOCK_STREAM, 0);
        if (fd < 0)
            continue;
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(fw->host_port)};
        a.sin_addr.s_addr = htonl(fw->lan ? INADDR_ANY : INADDR_LOOPBACK);
        if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || (!fw->udp && listen(fd, 8) < 0)) {
            LOGW("rede: nao consegui escutar na porta %s %u (%s)", fw->udp ? "UDP" : "TCP", fw->host_port,
                 strerror(errno));
            close(fd);
            continue;
        }
        set_nonblock(fd);
        n->listen_fd[i] = fd;
        LOGI("rede: %s %s:%u -> convidado:%u", fw->udp ? "UDP" : "TCP", fw->lan ? "0.0.0.0" : "127.0.0.1",
             fw->host_port, fw->guest_port);
    }

    if (pthread_create(&n->thread, NULL, net_thread, n) != 0) {
        snprintf(err, errlen, "rede: nao consegui criar a thread");
        net_free(n);
        return NULL;
    }
    n->thread_started = true;
    struct in_addr d = {htonl(n->dns_host)};
    LOGI("rede: NAT 10.0.2.0/24 (convidado 10.0.2.15, gateway 10.0.2.2, DNS 10.0.2.3 -> %s)", inet_ntoa(d));
    return n;
}

void net_free(mvm_net *n)
{
    if (!n)
        return;
    if (n->thread_started) {
        atomic_store(&n->quit, 1);
        wake(n);
        pthread_join(n->thread, NULL);
    }
    tcp_free_all(n);
    while (n->udp) {
        udp_flow *f = n->udp;
        n->udp = f->next;
        close(f->fd);
        free(f);
    }
    for (int i = 0; i < MVM_MAX_FORWARDS; i++) {
        if (n->listen_fd[i] >= 0)
            close(n->listen_fd[i]);
        free(n->udp_fwd_peer[i]);
    }
    if (n->icmp_fd >= 0)
        close(n->icmp_fd);
    if (n->wake_fd >= 0)
        close(n->wake_fd);
    for (int i = 0; i < n->txq_n; i++)
        free(n->txq[i]);
    free(n->txq);
    while (n->rxq_tail != n->rxq_head)
        free(n->rxq[n->rxq_tail++ % RXQ_FRAMES]);
    pthread_mutex_destroy(&n->txq_lock);
    pthread_mutex_destroy(&n->rxq_lock);
    free(n);
}

void net_get_mac(mvm_net *n, uint8_t mac[6]) { memcpy(mac, n->cfg.mac, 6); }

void net_send(mvm_net *n, const uint8_t *frame, size_t len)
{
    if (len < ETH_HLEN || len > ETH_HLEN + NET_MTU + 4)
        return;
    net_frame *f = malloc(sizeof(*f) + len);
    if (!f)
        return;
    memcpy(f->data, frame, len);
    f->len = len;
    pthread_mutex_lock(&n->txq_lock);
    if (n->txq_n >= 4096) { /* a thread de rede nao da conta: descarta */
        pthread_mutex_unlock(&n->txq_lock);
        free(f);
        return;
    }
    if (n->txq_n == n->txq_cap) {
        n->txq_cap = n->txq_cap ? n->txq_cap * 2 : 64;
        n->txq = realloc(n->txq, (size_t)n->txq_cap * sizeof(*n->txq));
    }
    n->txq[n->txq_n++] = f;
    pthread_mutex_unlock(&n->txq_lock);
    wake(n);
}

size_t net_rx_peek(mvm_net *n)
{
    size_t len = 0;
    pthread_mutex_lock(&n->rxq_lock);
    if (n->rxq_tail != n->rxq_head)
        len = n->rxq[n->rxq_tail % RXQ_FRAMES]->len;
    pthread_mutex_unlock(&n->rxq_lock);
    return len;
}

size_t net_rx_pop(mvm_net *n, uint8_t *buf, size_t max)
{
    net_frame *f = NULL;
    pthread_mutex_lock(&n->rxq_lock);
    if (n->rxq_tail != n->rxq_head) {
        f = n->rxq[n->rxq_tail % RXQ_FRAMES];
        n->rxq_tail++;
        n->rxq_bytes -= f->len;
    }
    bool low = n->rxq_bytes < RXQ_LIMIT / 2;
    pthread_mutex_unlock(&n->rxq_lock);
    if (!f)
        return 0;
    size_t len = f->len < max ? f->len : max;
    memcpy(buf, f->data, len);
    free(f);
    /* a thread de rede parou por falta de espaco: agora ha */
    if (low && atomic_exchange(&n->rx_waiting, 0))
        wake(n);
    return len;
}

void net_set_client(mvm_net *n, void (*rx_ready)(void *opaque), void *opaque)
{
    n->rx_ready = rx_ready;
    n->rx_opaque = opaque;
}

void net_poll(mvm_net *n)
{
    if (!n || !n->rx_ready)
        return;
    if (atomic_exchange(&n->rx_pending, 0) || net_rx_peek(n))
        n->rx_ready(n->rx_opaque);
}
