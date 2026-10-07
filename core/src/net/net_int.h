/* Rede em modo usuario (NAT no estilo slirp): estruturas internas compartilhadas. */
#ifndef MVM_NET_INT_H
#define MVM_NET_INT_H

#include "../internal.h"

#include <netinet/in.h>
#include <poll.h>

/* enderecos da rede virtual (ordem do host) */
#define NET_ADDR(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
#define NET_GW NET_ADDR(10, 0, 2, 2)     /* gateway; tambem e o "host" (127.0.0.1) */
#define NET_DNS NET_ADDR(10, 0, 2, 3)    /* DNS repassado ao do host */
#define NET_GUEST NET_ADDR(10, 0, 2, 15) /* entregue por DHCP */
#define NET_MASK 0xffffff00u
#define NET_NET NET_ADDR(10, 0, 2, 0)
#define LOCALHOST NET_ADDR(127, 0, 0, 1)

#define ETH_HLEN 14
#define IP_HLEN 20
#define TCP_HLEN 20
#define NET_MTU 1500

#define IPPROTO_ICMP_ 1
#define IPPROTO_TCP_ 6
#define IPPROTO_UDP_ 17

typedef struct net_frame {
    size_t len;
    uint8_t data[];
} net_frame;

#define RXQ_FRAMES 1024
#define RXQ_LIMIT (512 * 1024) /* bytes pendentes para o convidado: acima disso o TCP espera */

struct tcp_conn;
struct udp_flow;

typedef struct mvm_net {
    mvm_vm *vm;
    mvm_net_config cfg;
    uint8_t guest_mac[6];
    uint8_t gw_mac[6];
    uint32_t dns_host; /* servidor DNS real (ordem do host) */

    pthread_t thread;
    bool thread_started;
    _Atomic int quit;
    int wake_fd; /* eventfd: acorda a thread de rede */

    /* convidado -> rede (produtor: thread da VM) */
    pthread_mutex_t txq_lock;
    net_frame **txq;
    int txq_n, txq_cap;

    /* rede -> convidado (produtor: thread de rede; consumidor: thread da VM) */
    pthread_mutex_t rxq_lock;
    net_frame *rxq[RXQ_FRAMES];
    unsigned rxq_head, rxq_tail;
    size_t rxq_bytes;
    _Atomic int rx_waiting; /* a thread de rede espera espaco na fila */
    _Atomic int rx_pending; /* ha quadros novos (avisa a placa no proximo poll) */

    void (*rx_ready)(void *opaque);
    void *rx_opaque;

    /* estado da pilha (so a thread de rede mexe) */
    uint16_t ip_id;
    struct tcp_conn *tcp;
    struct udp_flow *udp;
    int icmp_fd;
    struct {
        uint32_t dst;
        uint16_t seq, guest_id;
        bool used;
    } pings[64];
    int ping_next;
    int listen_fd[MVM_MAX_FORWARDS];
    struct sockaddr_in *udp_fwd_peer[MVM_MAX_FORWARDS]; /* ultimo cliente de cada porta UDP redirecionada */
    uint16_t fwd_port_next;
} mvm_net;

/* MVM_NET_DEBUG=1: registra DHCP, conexoes e erros da pilha */
extern int net_debug;
#define NETDBG(...) do { if (net_debug) LOGI("rede: " __VA_ARGS__); } while (0)

/* tempo monotonico em ms */
int64_t net_now_ms(void);

/* checksum da internet */
uint32_t net_csum_add(uint32_t sum, const void *data, size_t len);
uint16_t net_csum_fold(uint32_t sum);
uint32_t net_pseudo_sum(uint32_t src, uint32_t dst, uint8_t proto, uint16_t len);

static inline uint16_t rd_be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t rd_be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline void wr_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void wr_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

/* ha espaco na fila do convidado? (senao marca rx_waiting) */
bool net_rx_room(mvm_net *n, size_t bytes);
/* entrega um pacote IP ao convidado (monta Ethernet + cabecalho IP; l4 ja com checksum) */
void net_send_ip(mvm_net *n, uint8_t proto, uint32_t src, uint32_t dst, const uint8_t *l4, size_t len);

/* endereco visto pelo convidado <-> endereco real no host */
uint32_t net_map_out(mvm_net *n, uint32_t ip);
uint32_t net_map_in(mvm_net *n, uint32_t ip);

/* TCP (tcp.c) */
void tcp_input(mvm_net *n, uint32_t src, uint32_t dst, const uint8_t *seg, size_t len);
int tcp_pollfds(mvm_net *n, struct pollfd *fds, void **owners, int max);
void tcp_events(mvm_net *n, void *owner, short revents);
void tcp_timers(mvm_net *n, int64_t now);
void tcp_output_all(mvm_net *n);
void tcp_accept_forward(mvm_net *n, int listen_fd, uint16_t guest_port);
void tcp_free_all(mvm_net *n);
int64_t tcp_next_timer(mvm_net *n);

#endif
