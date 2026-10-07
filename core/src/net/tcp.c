/*
 * TCP do NAT em modo usuario. Cada conexao do convidado termina aqui e vira um
 * socket TCP do host (e o contrario para as portas redirecionadas). O enlace com
 * o convidado nao perde pacotes, mas a fila dele pode encher: os dados so saem
 * quando ha espaco (net_rx_room) e ha retransmissao por tempo (volta-N).
 */
#include "net_int.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#define TH_FIN 0x01
#define TH_SYN 0x02
#define TH_RST 0x04
#define TH_PSH 0x08
#define TH_ACK 0x10

#define SBUF_SIZE (256 * 1024) /* host -> convidado, ainda sem ACK */
#define RBUF_SIZE (64 * 1024)  /* convidado -> host, ainda nao escrito no socket */
#define RTO_MIN 250
#define RTO_MAX 4000
#define MAX_RETRIES 12

enum {
    T_CONNECTING, /* SYN do convidado; connect() no host em andamento */
    T_SYN_RCVD,   /* SYN-ACK enviado ao convidado */
    T_FWD_SYN,    /* porta redirecionada: SYN enviado ao convidado */
    T_ESTAB,
    T_CLOSED,
};

typedef struct tcp_conn {
    struct tcp_conn *next;
    int fd;
    int state;
    uint32_t gip, rip;     /* convidado / outro lado (como o convidado o ve) */
    uint16_t gport, rport;
    /* envio (para o convidado) */
    uint32_t iss, snd_una, snd_nxt, snd_wnd;
    uint16_t mss;
    uint8_t *sbuf;
    size_t slen;           /* bytes em sbuf, a partir de snd_una */
    bool host_eof, fin_sent;
    /* recepcao (do convidado) */
    uint32_t rcv_nxt;
    uint8_t *rbuf;
    size_t rlen;
    bool guest_fin, shut_wr;
    uint16_t adv_wnd;      /* ultima janela anunciada */
    /* temporizador */
    int64_t rto_at;
    int rto, retries;
    int64_t close_at;      /* fechamento adiado (ultimo ACK) */
} tcp_conn;

static void conn_free(mvm_net *n, tcp_conn *c)
{
    for (tcp_conn **pp = &n->tcp; *pp; pp = &(*pp)->next)
        if (*pp == c) {
            *pp = c->next;
            break;
        }
    if (c->fd >= 0)
        close(c->fd);
    free(c->sbuf);
    free(c->rbuf);
    free(c);
}

static uint16_t rcv_window(tcp_conn *c)
{
    size_t w = RBUF_SIZE - c->rlen;
    return (uint16_t)(w > 65535 ? 65535 : w);
}

/* segmento para o convidado */
static void send_seg(mvm_net *n, tcp_conn *c, uint8_t flags, uint32_t seq, const uint8_t *data, size_t len)
{
    uint8_t seg[NET_MTU];
    size_t hl = TCP_HLEN + ((flags & TH_SYN) ? 4 : 0);
    if (hl + len > NET_MTU - IP_HLEN)
        return;
    wr_be16(seg, c->rport);
    wr_be16(seg + 2, c->gport);
    wr_be32(seg + 4, seq);
    wr_be32(seg + 8, (flags & TH_ACK) ? c->rcv_nxt : 0);
    seg[12] = (uint8_t)((hl / 4) << 4);
    seg[13] = flags;
    c->adv_wnd = rcv_window(c);
    wr_be16(seg + 14, c->adv_wnd);
    wr_be16(seg + 16, 0);
    wr_be16(seg + 18, 0);
    if (flags & TH_SYN) { /* MSS */
        seg[20] = 2;
        seg[21] = 4;
        wr_be16(seg + 22, 1460);
    }
    if (len)
        memcpy(seg + hl, data, len);
    uint16_t tl = (uint16_t)(hl + len);
    wr_be16(seg + 16, net_csum_fold(net_csum_add(net_pseudo_sum(c->rip, c->gip, IPPROTO_TCP_, tl), seg, tl)));
    net_send_ip(n, IPPROTO_TCP_, c->rip, c->gip, seg, tl);
}

/* RST para um segmento sem conexao */
static void send_rst(mvm_net *n, uint32_t src, uint32_t dst, const uint8_t *in, size_t inlen)
{
    uint8_t flags = in[13];
    if (flags & TH_RST)
        return;
    tcp_conn tmp = {0};
    tmp.rip = dst;
    tmp.gip = src;
    tmp.rport = rd_be16(in + 2);
    tmp.gport = rd_be16(in);
    size_t dlen = inlen - (size_t)(in[12] >> 4) * 4;
    uint32_t seq = rd_be32(in + 4);
    if (flags & TH_ACK) {
        tmp.rcv_nxt = 0;
        send_seg(n, &tmp, TH_RST, rd_be32(in + 8), NULL, 0);
    } else {
        tmp.rcv_nxt = seq + (uint32_t)dlen + ((flags & TH_SYN) ? 1 : 0) + ((flags & TH_FIN) ? 1 : 0);
        send_seg(n, &tmp, TH_RST | TH_ACK, 0, NULL, 0);
    }
}

static void arm_rto(tcp_conn *c)
{
    if (!c->rto)
        c->rto = RTO_MIN;
    c->rto_at = net_now_ms() + c->rto;
}

static void abort_conn(mvm_net *n, tcp_conn *c)
{
    if (c->state == T_ESTAB || c->state == T_SYN_RCVD)
        send_seg(n, c, TH_RST | TH_ACK, c->snd_nxt, NULL, 0);
    conn_free(n, c);
}

/* envia o que for possivel ao convidado */
static void output(mvm_net *n, tcp_conn *c)
{
    if (c->state != T_ESTAB)
        return;
    for (;;) {
        uint32_t inflight = c->snd_nxt - c->snd_una;
        size_t sent_data = inflight > c->slen ? c->slen : inflight; /* o FIN ocupa 1 numero */
        size_t unsent = c->slen - sent_data;
        if (!unsent || inflight >= c->snd_wnd)
            break;
        size_t len = unsent;
        if (len > c->mss)
            len = c->mss;
        if (len > c->snd_wnd - inflight)
            len = c->snd_wnd - inflight;
        if (!net_rx_room(n, len + 80))
            return;
        send_seg(n, c, TH_ACK | TH_PSH, c->snd_nxt, c->sbuf + sent_data, len);
        c->snd_nxt += (uint32_t)len;
        if (!c->rto_at)
            arm_rto(c);
    }
    /* fim do host: FIN depois de todos os dados */
    if (c->host_eof && !c->fin_sent && c->snd_nxt - c->snd_una == c->slen && net_rx_room(n, 80)) {
        send_seg(n, c, TH_ACK | TH_FIN, c->snd_nxt, NULL, 0);
        c->snd_nxt++;
        c->fin_sent = true;
        if (!c->rto_at)
            arm_rto(c);
    }
    /* janela reaberta: avisa o convidado */
    if (c->adv_wnd < c->mss && rcv_window(c) >= c->mss && net_rx_room(n, 80))
        send_seg(n, c, TH_ACK, c->snd_nxt, NULL, 0);
}

/* dados do convidado para o socket do host; false se a conexao foi abortada (e liberada) */
static bool flush_to_host(mvm_net *n, tcp_conn *c)
{
    while (c->rlen) {
        ssize_t w = send(c->fd, c->rbuf, c->rlen, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                return true;
            abort_conn(n, c);
            return false;
        }
        memmove(c->rbuf, c->rbuf + w, c->rlen - (size_t)w);
        c->rlen -= (size_t)w;
    }
    if (c->guest_fin && !c->shut_wr) {
        shutdown(c->fd, SHUT_WR);
        c->shut_wr = true;
    }
    return true;
}

static bool finished(tcp_conn *c)
{
    return c->guest_fin && c->fin_sent && c->snd_una == c->snd_nxt && c->shut_wr;
}

static uint16_t parse_mss(const uint8_t *seg, size_t hl)
{
    for (size_t i = TCP_HLEN; i < hl;) {
        uint8_t k = seg[i];
        if (k == 0)
            break;
        if (k == 1) {
            i++;
            continue;
        }
        if (i + 1 >= hl || seg[i + 1] < 2)
            break;
        if (k == 2 && seg[i + 1] == 4 && i + 4 <= hl)
            return rd_be16(seg + i + 2);
        i += seg[i + 1];
    }
    return 536;
}

static tcp_conn *find(mvm_net *n, uint32_t rip, uint16_t rport, uint16_t gport)
{
    for (tcp_conn *c = n->tcp; c; c = c->next)
        if (c->rip == rip && c->rport == rport && c->gport == gport)
            return c;
    return NULL;
}

static tcp_conn *conn_new(mvm_net *n)
{
    tcp_conn *c = calloc(1, sizeof(*c));
    c->fd = -1;
    c->sbuf = malloc(SBUF_SIZE);
    c->rbuf = malloc(RBUF_SIZE);
    c->iss = (uint32_t)rand() << 1 ^ (uint32_t)rand();
    c->snd_una = c->snd_nxt = c->iss;
    c->mss = 536;
    c->next = n->tcp;
    n->tcp = c;
    return c;
}

/* SYN do convidado: abre o socket no host */
static void open_conn(mvm_net *n, uint32_t src, uint32_t dst, const uint8_t *seg, size_t hl)
{
    uint16_t sport = rd_be16(seg), dport = rd_be16(seg + 2);
    if (((dst & NET_MASK) == NET_NET && dst != NET_GW) || (dst >> 28) == 0xe || dst == 0xffffffffu) {
        send_rst(n, src, dst, seg, hl);
        return;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        send_rst(n, src, dst, seg, hl);
        return;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, 1 /* TCP_NODELAY */, &one, sizeof(one));
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(dport)};
    to.sin_addr.s_addr = htonl(net_map_out(n, dst));
    int r = connect(fd, (struct sockaddr *)&to, sizeof(to));
    if (r < 0 && errno != EINPROGRESS) {
        close(fd);
        send_rst(n, src, dst, seg, hl);
        return;
    }
    NETDBG("TCP %u.%u.%u.%u:%u", dst >> 24, (dst >> 16) & 255, (dst >> 8) & 255, dst & 255, dport);
    tcp_conn *c = conn_new(n);
    c->fd = fd;
    c->gip = src;
    c->rip = dst;
    c->gport = sport;
    c->rport = dport;
    c->rcv_nxt = rd_be32(seg + 4) + 1;
    c->snd_wnd = rd_be16(seg + 14);
    uint16_t mss = parse_mss(seg, hl);
    c->mss = mss > 1460 ? 1460 : mss;
    c->state = T_CONNECTING;
    if (r == 0) { /* conectou na hora (localhost) */
        c->state = T_SYN_RCVD;
        send_seg(n, c, TH_SYN | TH_ACK, c->iss, NULL, 0);
        c->snd_nxt = c->iss + 1;
        arm_rto(c);
    }
}

void tcp_input(mvm_net *n, uint32_t src, uint32_t dst, const uint8_t *seg, size_t len)
{
    if (len < TCP_HLEN)
        return;
    size_t hl = (size_t)(seg[12] >> 4) * 4;
    if (hl < TCP_HLEN || hl > len)
        return;
    uint16_t sport = rd_be16(seg), dport = rd_be16(seg + 2);
    uint32_t seq = rd_be32(seg + 4), ack = rd_be32(seg + 8);
    uint8_t flags = seg[13];
    uint16_t wnd = rd_be16(seg + 14);
    const uint8_t *data = seg + hl;
    size_t dlen = len - hl;

    tcp_conn *c = find(n, dst, dport, sport);
    if (!c) {
        if ((flags & (TH_SYN | TH_ACK | TH_RST)) == TH_SYN)
            open_conn(n, src, dst, seg, hl);
        else
            send_rst(n, src, dst, seg, len);
        return;
    }
    if (flags & TH_RST) {
        conn_free(n, c);
        return;
    }

    switch (c->state) {
    case T_CONNECTING:
        return; /* SYN repetido: responde quando o connect terminar */
    case T_SYN_RCVD:
        if (flags & TH_SYN) { /* SYN retransmitido */
            send_seg(n, c, TH_SYN | TH_ACK, c->iss, NULL, 0);
            return;
        }
        if (!(flags & TH_ACK) || ack != c->iss + 1)
            return;
        c->state = T_ESTAB;
        c->snd_una = ack;
        c->rto_at = 0;
        c->rto = RTO_MIN;
        break;
    case T_FWD_SYN:
        if ((flags & (TH_SYN | TH_ACK)) != (TH_SYN | TH_ACK) || ack != c->iss + 1)
            return;
        c->rcv_nxt = seq + 1;
        c->snd_una = ack;
        c->snd_wnd = wnd;
        uint16_t mss = parse_mss(seg, hl);
        c->mss = mss > 1460 ? 1460 : mss;
        c->state = T_ESTAB;
        c->rto_at = 0;
        c->rto = RTO_MIN;
        send_seg(n, c, TH_ACK, c->snd_nxt, NULL, 0);
        return;
    }
    if (c->state != T_ESTAB)
        return;

    /* ACK do que enviamos */
    if (flags & TH_ACK) {
        uint32_t adv = ack - c->snd_una;
        if (adv && adv <= c->snd_nxt - c->snd_una) {
            size_t d = adv > c->slen ? c->slen : adv;
            memmove(c->sbuf, c->sbuf + d, c->slen - d);
            c->slen -= d;
            c->snd_una = ack;
            c->retries = 0;
            c->rto = RTO_MIN;
            c->rto_at = c->snd_una != c->snd_nxt ? net_now_ms() + c->rto : 0;
        }
        c->snd_wnd = wnd;
    }

    /* dados do convidado */
    bool need_ack = false;
    if (dlen) {
        if (seq == c->rcv_nxt) {
            size_t room = RBUF_SIZE - c->rlen;
            size_t take = dlen < room ? dlen : room;
            memcpy(c->rbuf + c->rlen, data, take);
            c->rlen += take;
            c->rcv_nxt += (uint32_t)take;
            if (take < dlen)
                flags &= (uint8_t)~TH_FIN; /* o FIN vem depois dos dados que faltaram */
        }
        need_ack = true;
    }
    if ((flags & TH_FIN) && seq + dlen == c->rcv_nxt && !c->guest_fin) {
        c->rcv_nxt++;
        c->guest_fin = true;
        need_ack = true;
    } else if ((flags & TH_FIN) && c->guest_fin) {
        need_ack = true; /* FIN retransmitido */
    }
    if ((c->rlen || (c->guest_fin && !c->shut_wr)) && !flush_to_host(n, c))
        return; /* abortada */
    if (need_ack)
        send_seg(n, c, TH_ACK, c->snd_nxt, NULL, 0);
    output(n, c);
    if (finished(c))
        conn_free(n, c);
}

int tcp_pollfds(mvm_net *n, struct pollfd *fds, void **owners, int max)
{
    int k = 0;
    for (tcp_conn *c = n->tcp; c && k < max; c = c->next) {
        if (c->fd < 0)
            continue;
        short ev = 0;
        if (c->state == T_CONNECTING)
            ev = POLLOUT;
        else if (c->state == T_ESTAB) {
            if (!c->host_eof && c->slen < SBUF_SIZE)
                ev |= POLLIN;
            if (c->rlen)
                ev |= POLLOUT;
        }
        if (!ev)
            continue;
        fds[k] = (struct pollfd){c->fd, ev, 0};
        owners[k] = c;
        k++;
    }
    return k;
}

void tcp_events(mvm_net *n, void *owner, short revents)
{
    tcp_conn *c = NULL;
    for (tcp_conn *it = n->tcp; it; it = it->next)
        if (it == owner)
            c = it;
    if (!c)
        return;
    if (c->state == T_CONNECTING) {
        int err = 0;
        socklen_t el = sizeof(err);
        getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err || (revents & (POLLERR | POLLHUP) && !(revents & POLLOUT))) {
            /* recusada: RST para o SYN do convidado */
            send_seg(n, c, TH_RST | TH_ACK, 0, NULL, 0);
            conn_free(n, c);
            return;
        }
        c->state = T_SYN_RCVD;
        send_seg(n, c, TH_SYN | TH_ACK, c->iss, NULL, 0);
        c->snd_nxt = c->iss + 1;
        arm_rto(c);
        return;
    }
    if (c->state != T_ESTAB)
        return;
    if ((revents & POLLOUT) && !flush_to_host(n, c))
        return;
    if (revents & (POLLIN | POLLHUP | POLLERR)) {
        while (!c->host_eof && c->slen < SBUF_SIZE) {
            ssize_t r = recv(c->fd, c->sbuf + c->slen, SBUF_SIZE - c->slen, 0);
            if (r > 0) {
                c->slen += (size_t)r;
                continue;
            }
            if (r == 0) {
                c->host_eof = true;
                break;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                break;
            abort_conn(n, c);
            return;
        }
    }
    output(n, c);
    if (finished(c))
        conn_free(n, c);
}

void tcp_timers(mvm_net *n, int64_t now)
{
    for (tcp_conn *c = n->tcp, *next; c; c = next) {
        next = c->next;
        if (!c->rto_at || now < c->rto_at)
            continue;
        if (++c->retries > MAX_RETRIES) {
            abort_conn(n, c);
            continue;
        }
        c->rto = c->rto * 2 > RTO_MAX ? RTO_MAX : c->rto * 2;
        c->rto_at = now + c->rto;
        if (c->state == T_SYN_RCVD) {
            send_seg(n, c, TH_SYN | TH_ACK, c->iss, NULL, 0);
        } else if (c->state == T_FWD_SYN) {
            send_seg(n, c, TH_SYN, c->iss, NULL, 0);
        } else if (c->state == T_ESTAB) {
            /* volta-N: reenvia a partir do primeiro byte sem ACK */
            if (c->fin_sent && c->snd_una != c->snd_nxt)
                c->fin_sent = false;
            c->snd_nxt = c->snd_una;
            if (c->snd_wnd == 0 && c->slen) { /* sonda de janela zero: 1 byte */
                send_seg(n, c, TH_ACK, c->snd_nxt, c->sbuf, 1);
                c->snd_nxt++;
            } else {
                output(n, c);
            }
            if (c->snd_una == c->snd_nxt)
                c->rto_at = 0;
        }
    }
}

int64_t tcp_next_timer(mvm_net *n)
{
    int64_t t = 0;
    for (tcp_conn *c = n->tcp; c; c = c->next)
        if (c->rto_at && (!t || c->rto_at < t))
            t = c->rto_at;
    return t;
}

void tcp_output_all(mvm_net *n)
{
    for (tcp_conn *c = n->tcp, *next; c; c = next) {
        next = c->next;
        output(n, c);
    }
}

/* conexao numa porta redirecionada do host: SYN para o convidado */
void tcp_accept_forward(mvm_net *n, int listen_fd, uint16_t guest_port)
{
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int fd = accept(listen_fd, (struct sockaddr *)&from, &fl);
        if (fd < 0)
            return;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        tcp_conn *c = conn_new(n);
        c->fd = fd;
        c->gip = NET_GUEST;
        c->gport = guest_port;
        c->rip = NET_GW;
        c->rport = n->fwd_port_next++;
        if (n->fwd_port_next < 40000)
            n->fwd_port_next = 40000;
        c->state = T_FWD_SYN;
        c->snd_wnd = 65535;
        send_seg(n, c, TH_SYN, c->iss, NULL, 0);
        c->snd_nxt = c->iss + 1;
        arm_rto(c);
    }
}

void tcp_free_all(mvm_net *n)
{
    while (n->tcp)
        conn_free(n, n->tcp);
}
