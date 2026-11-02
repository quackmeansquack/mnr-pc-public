/* The PS3's player-to-player (P2P) UDP port and NP signaling, compatible with RPCS3 (which most other players use).
 *
 * On the console every game shares one UDP port (3658) for direct traffic between players. Sockets of type
 * SOCK_DGRAM_P2P (6) bind a "virtual port" on it, and every datagram carries a 6-byte header: destination vport,
 * source vport, flags (1 = P2P), all little-endian (RPCS3's framing, which the other players expect). Virtual port 0
 * is reserved: subset 0 carries the RPCN server's replies (our public address), subset 1 NP signaling packets.
 *
 * Here a host thread owns the real port. Datagrams for the game go to the game's P2P socket, which is a loopback UDP
 * socket, prefixed with the sender's address, port and vport (so the game's poll/select work unchanged); the game's
 * sends leave through the real port with the header added. The thread also:
 *  - pings the RPCN server's UDP port (3657) every 5 s from the real port with our RPCN user id, so the server knows
 *    our public address and can give it to players who want to connect to us;
 *  - answers signaling packets (ping, connect, confirm, info, finished) like RPCS3, so peers' connections to us
 *    become active;
 *  - follows RPCN's SignalingHelper notifications (via the launcher, which holds the RPCN session) by sending "info"
 *    packets to that player, to open the way through both routers.
 * The RPCN session details come from the launcher's helper (MNR_NP_HELPER): GET /rpcn/info -> "<user id> <rpcn ipv4>",
 * GET /rpcn/events -> lines "helper <npid> <ipv4> <port>". MNR_P2P_LOG=1 logs the traffic. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "p2p.h"

int net_http_get(const char *hostport, const char *path, uint8_t **body);

#define SIG_SIGNATURE 0x5349474Eu   /* "SIGN", stored big-endian */
#define SIG_VERSION 3
enum { SIG_PING, SIG_PONG, SIG_CONNECT, SIG_CONNECT_ACK, SIG_CONFIRM, SIG_FINISHED, SIG_FINISHED_ACK, SIG_INFO };
#define SIG_PACKET_SIZE 72          /* RPCS3's signaling_packet (natural alignment) */
#define MAX_PEERS 64
#define MAX_BOUND 16

static SOCKET real = INVALID_SOCKET;   /* the shared P2P port */
static uint16_t real_port;
static SRWLOCK lock = SRWLOCK_INIT;
static int logon = -1;
static struct { uint16_t vport; SOCKET loop; struct sockaddr_in loop_addr; } bound[MAX_BOUND];
static int nbound;
static char self_npid[17];
static int64_t rpcn_user_id; static uint32_t rpcn_ip;   /* network order */
static volatile uint32_t pub_addr; static volatile uint16_t pub_port;   /* as RPCN sees us */

typedef struct {
    char npid[17];
    uint32_t addr; uint16_t port;      /* network-order address, host-order port */
    int status;                        /* 0 inactive, 1 pending, 2 active */
    int confirmed;                     /* the peer confirmed our connect_ack */
    uint64_t last_recv;                /* ms */
    uint64_t next_ack, next_ping, next_info; int info_left;
    uint8_t ack[SIG_PACKET_SIZE];      /* the connect_ack we repeat until confirmed */
} peer_t;
static peer_t peers[MAX_PEERS];        /* conn_id = index + 1 */

static uint64_t now_ms(void) { return GetTickCount64(); }
static uint64_t now_us(void) { LARGE_INTEGER f, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); return (uint64_t)((double)t.QuadPart * 1e6 / (double)f.QuadPart); }
static void wr16le(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32le(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void wr64le(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint16_t rd16le(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32le(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64le(const uint8_t *p) { return (uint64_t)rd32le(p) | (uint64_t)rd32le(p + 4) << 32; }
static const char *ipstr(uint32_t a) { static __thread char b[4][24]; static __thread int k; char *s = b[k++ & 3]; const uint8_t *q = (const uint8_t *)&a; snprintf(s, 24, "%u.%u.%u.%u", q[0], q[1], q[2], q[3]); return s; }

static void send_to(const uint8_t *data, int len, uint32_t addr, uint16_t port) {
    struct sockaddr_in d = {0}; d.sin_family = AF_INET; d.sin_addr.s_addr = addr; d.sin_port = htons(port);
    sendto(real, (const char *)data, len, 0, (struct sockaddr *)&d, sizeof d);
}

/* ------------------------------------------------------------------ signaling */
static void sig_build(uint8_t *p, int cmd, uint64_t ts_s, uint64_t ts_r, uint32_t to_addr, uint16_t to_port) {
    memset(p, 0, SIG_PACKET_SIZE);
    p[0] = 'S'; p[1] = 'I'; p[2] = 'G'; p[3] = 'N';
    wr32le(p + 4, SIG_VERSION); wr64le(p + 8, ts_s); wr64le(p + 16, ts_r); wr32le(p + 24, (uint32_t)cmd);
    memcpy(p + 28, &to_addr, 4);              /* sent_addr: the destination as we addressed it (raw network order) */
    wr16le(p + 32, to_port);
    memcpy(p + 34, self_npid, strlen(self_npid));   /* SceNpId: handle data[16], term, dummy[3], opt[8], reserved[8] */
}
static void sig_send(const uint8_t *pkt, uint32_t addr, uint16_t port) {
    uint8_t b[3 + SIG_PACKET_SIZE];
    wr16le(b, 0); b[2] = 1;                   /* vport 0, subset 1: signaling */
    memcpy(b + 3, pkt, SIG_PACKET_SIZE);
    memcpy(b + 3 + 28, &addr, 4); wr16le(b + 3 + 32, port);
    send_to(b, sizeof b, addr, port);
}
static peer_t *peer_get(const char *npid, int create) {
    for (int i = 0; i < MAX_PEERS; i++) if (peers[i].npid[0] && !strcmp(peers[i].npid, npid)) return &peers[i];
    if (!create) return NULL;
    for (int i = 0; i < MAX_PEERS; i++) if (!peers[i].npid[0]) { memset(&peers[i], 0, sizeof peers[i]); snprintf(peers[i].npid, sizeof peers[i].npid, "%s", npid); return &peers[i]; }
    return NULL;
}
static const char *cmd_name(int c) { static const char *n[] = {"PING", "PONG", "CONNECT", "CONNECT_ACK", "CONFIRM", "FINISHED", "FINISHED_ACK", "INFO"}; return c >= 0 && c < 8 ? n[c] : "?"; }

static void sig_receive(const uint8_t *p, int len, uint32_t from, uint16_t from_port) {
    if (len != SIG_PACKET_SIZE || memcmp(p, "SIGN", 4) || rd32le(p + 4) != SIG_VERSION) { if (logon) printf("[p2p] bad signaling packet (%d bytes) from %s:%u\n", len, ipstr(from), from_port); return; }
    char npid[17] = {0}; memcpy(npid, p + 34, 16);
    if (!npid[0] || p[34 + 16]) return;
    int cmd = (int)rd32le(p + 24);
    uint64_t ts_s = rd64le(p + 8), ts_r = rd64le(p + 16), t = now_ms();
    if (logon) printf("[p2p] signaling %s from %s at %s:%u\n", cmd_name(cmd), npid, ipstr(from), from_port);
    peer_t *pe = peer_get(npid, cmd == SIG_CONNECT || cmd == SIG_INFO);
    uint8_t out[SIG_PACKET_SIZE];
    if (!pe) {   /* unknown peer: only a FINISHED gets an answer (its FINISHED_ACK may have been lost) */
        if (cmd == SIG_FINISHED) { sig_build(out, SIG_FINISHED_ACK, 0, 0, from, from_port); sig_send(out, from, from_port); }
        return;
    }
    pe->last_recv = t;
    switch (cmd) {
    case SIG_PING:
        sig_build(out, SIG_PONG, ts_s, 0, from, from_port); sig_send(out, from, from_port);
        break;
    case SIG_PONG:
        pe->next_ping = t + 10000;
        break;
    case SIG_INFO:
        pe->addr = from; pe->port = from_port;
        break;
    case SIG_CONNECT:   /* a peer connects to us: acknowledge until it confirms (RPCS3 repeats every 200 ms) */
        pe->addr = from; pe->port = from_port;
        if (pe->status == 0) pe->status = 1;
        sig_build(pe->ack, SIG_CONNECT_ACK, ts_s, now_us(), from, from_port);
        sig_send(pe->ack, from, from_port); pe->next_ack = t + 200; pe->confirmed = 0;
        break;
    case SIG_CONNECT_ACK:   /* (we never start connections ourselves; confirm anyway so the peer is satisfied) */
        pe->addr = from; pe->port = from_port; pe->status = 2;
        sig_build(out, SIG_CONFIRM, 0, ts_r, from, from_port); sig_send(out, from, from_port);
        if (!pe->next_ping) pe->next_ping = t;
        break;
    case SIG_CONFIRM:
        pe->addr = from; pe->port = from_port; pe->confirmed = 1; pe->status = 2; pe->next_ack = 0;
        if (!pe->next_ping) pe->next_ping = t;   /* keep the way through the routers open */
        break;
    case SIG_FINISHED:
        sig_build(out, SIG_FINISHED_ACK, 0, 0, from, from_port); sig_send(out, from, from_port);
        pe->status = 0; pe->next_ack = pe->next_ping = 0;
        break;
    case SIG_FINISHED_ACK:
        pe->status = 0; pe->next_ack = pe->next_ping = 0;
        break;
    }
}
static void sig_timers(void) {
    uint64_t t = now_ms();
    uint8_t out[SIG_PACKET_SIZE];
    for (int i = 0; i < MAX_PEERS; i++) {
        peer_t *pe = &peers[i];
        if (!pe->npid[0] || !pe->addr) continue;
        if (pe->status && pe->last_recv && t - pe->last_recv > 60000) {   /* nothing for a minute: the connection is dead */
            if (logon) printf("[p2p] %s timed out\n", pe->npid);
            pe->status = 0; pe->next_ack = pe->next_ping = 0;
        }
        if (pe->next_ack && t >= pe->next_ack && !pe->confirmed) {
            wr64le(pe->ack + 16, now_us()); sig_send(pe->ack, pe->addr, pe->port); pe->next_ack = t + 200;
        }
        if (pe->next_ping && t >= pe->next_ping && pe->status == 2) {
            sig_build(out, SIG_PING, now_us(), 0, pe->addr, pe->port); sig_send(out, pe->addr, pe->port); pe->next_ping = t + 500;
        }
        if (pe->info_left > 0 && t >= pe->next_info) {
            sig_build(out, SIG_INFO, 0, 0, pe->addr, pe->port); sig_send(out, pe->addr, pe->port); pe->info_left--; pe->next_info = t + 200;
        }
    }
}

/* ------------------------------------------------------------------ RPCN (through the launcher) */
static void rpcn_info(void) {   /* our RPCN user id and the server's address, once the launcher has signed in */
    const char *helper = getenv("MNR_NP_HELPER");
    if (!helper || !*helper || rpcn_user_id) return;
    uint8_t *b = NULL; int n = net_http_get(helper, "/rpcn/info", &b);
    if (n > 0) {
        char s[128]; int l = n < 127 ? n : 127; memcpy(s, b, l); s[l] = 0;
        long long id = 0; char ip[64] = "";
        if (sscanf(s, "%lld %63s", &id, ip) == 2) {
            struct in_addr a;
            if (inet_pton(AF_INET, ip, &a) == 1) { rpcn_ip = a.s_addr; rpcn_user_id = id; if (logon) printf("[p2p] RPCN user id %lld, server %s\n", id, ip); }
        }
    }
    free(b);
}
static void rpcn_events(void) {
    const char *helper = getenv("MNR_NP_HELPER");
    if (!helper || !*helper || !rpcn_user_id) return;
    uint8_t *b = NULL; int n = net_http_get(helper, "/rpcn/events", &b);
    if (n > 0) {
        char *s = malloc(n + 1); memcpy(s, b, n); s[n] = 0;
        for (char *line = strtok(s, "\n"); line; line = strtok(NULL, "\n")) {
            char kind[16], npid[32], ip[64]; unsigned port;
            if (sscanf(line, "%15s %31s %63s %u", kind, npid, ip, &port) != 4 || strcmp(kind, "helper")) continue;
            struct in_addr a; if (inet_pton(AF_INET, ip, &a) != 1) continue;
            AcquireSRWLockExclusive(&lock);
            peer_t *pe = peer_get(npid, 1);   /* a player is connecting to us: send it info packets (10, every 200 ms) */
            if (pe) { pe->addr = a.s_addr; pe->port = (uint16_t)port; pe->info_left = 10; pe->next_info = 0; pe->last_recv = now_ms(); }
            ReleaseSRWLockExclusive(&lock);
            if (logon) printf("[p2p] RPCN: %s is connecting from %s:%u\n", npid, ip, port);
        }
        free(s);
    }
    free(b);
}
static void rpcn_ping(void) {   /* 13 bytes from the P2P port to RPCN's UDP port: {1, user id (le64), local ipv4 (be)} */
    if (!rpcn_user_id) return;
    uint8_t p[13]; p[0] = 1; wr64le(p + 1, (uint64_t)rpcn_user_id);
    uint32_t local = 0;
    { extern const char *net_local_ip(void); struct in_addr a; if (inet_pton(AF_INET, net_local_ip(), &a) == 1) local = a.s_addr; }
    memcpy(p + 9, &local, 4);
    send_to(p, sizeof p, rpcn_ip, 3657);
}

/* ------------------------------------------------------------------ the port's thread */
static void dispatch(const uint8_t *b, int n, const struct sockaddr_in *from) {
    if (n < 2) return;
    uint16_t dst = rd16le(b);
    if (dst == 0) {
        if (n < 3) return;
        if (b[2] == 0 && n - 3 == 6) {   /* RPCN: our address as the server sees it {addr (raw), port (be)} */
            uint32_t a; memcpy(&a, b + 3, 4); uint16_t pt = (uint16_t)(b[7] << 8 | b[8]);
            if (logon && (a != pub_addr || pt != pub_port)) printf("[p2p] RPCN sees us at %s:%u\n", ipstr(a), pt);
            pub_addr = a; pub_port = pt;
        } else if (b[2] == 1) {
            AcquireSRWLockExclusive(&lock);
            sig_receive(b + 3, n - 3, from->sin_addr.s_addr, ntohs(from->sin_port));
            ReleaseSRWLockExclusive(&lock);
        }
        return;
    }
    if (n < 6 || !(rd16le(b + 4) & 1)) return;   /* P2P flag (stream P2P is not used by the game) */
    uint16_t src = rd16le(b + 2);
    AcquireSRWLockShared(&lock);
    for (int i = 0; i < nbound; i++) {
        if (bound[i].vport != dst) continue;
        uint8_t *f = malloc(n - 6 + 8);   /* to the game's loopback socket: {addr (raw), port (be), src vport (be)} + data */
        memcpy(f, &from->sin_addr.s_addr, 4); memcpy(f + 4, &from->sin_port, 2); f[6] = (uint8_t)(src >> 8); f[7] = (uint8_t)src;
        memcpy(f + 8, b + 6, n - 6);
        sendto(bound[i].loop, (const char *)f, n - 6 + 8, 0, (struct sockaddr *)&bound[i].loop_addr, sizeof bound[i].loop_addr);
        free(f);
    }
    ReleaseSRWLockShared(&lock);
}
static DWORD WINAPI port_thread(void *arg) {
    (void)arg;
    static uint8_t buf[65536];
    uint64_t next_ping = 0, last_pong_seen = 0, next_events = 0; uint32_t seen_addr = 0;
    for (;;) {
        fd_set rd; FD_ZERO(&rd); FD_SET(real, &rd);
        struct timeval tv = {0, 50000};
        if (select(0, &rd, NULL, NULL, &tv) > 0) {
            struct sockaddr_in from; int fl = sizeof from;
            int n = recvfrom(real, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
            if (n > 0) dispatch(buf, n, &from);
        }
        uint64_t t = now_ms();
        if (!rpcn_user_id && t >= next_events) { rpcn_info(); next_events = t + 2000; }
        if (rpcn_user_id) {
            if (pub_addr != seen_addr || pub_addr == 0) { if (pub_addr) { last_pong_seen = t; seen_addr = pub_addr; } }
            /* every 5 s, or every 500 ms until the server has answered */
            if (t >= next_ping) { rpcn_ping(); next_ping = t + (pub_addr ? 5000 : 500); }
            if (t >= next_events) { rpcn_events(); next_events = t + 1000; }
        }
        (void)last_pong_seen;
        AcquireSRWLockExclusive(&lock); sig_timers(); ReleaseSRWLockExclusive(&lock);
    }
    return 0;
}

/* ------------------------------------------------------------------ the game's side */
int p2p_bind(uint16_t port, uint16_t vport, SOCKET game_sock) {
    if (logon < 0) logon = getenv("MNR_P2P_LOG") != NULL;
    AcquireSRWLockExclusive(&lock);
    if (real == INVALID_SOCKET) {
        const char *id = getenv("MNR_NP_ID"); snprintf(self_npid, sizeof self_npid, "%s", id ? id : "");
        real = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        BOOL off = FALSE; DWORD br = 0; WSAIoctl(real, _WSAIOW(IOC_VENDOR, 12), &off, sizeof off, NULL, 0, &br, NULL, NULL);   /* SIO_UDP_CONNRESET off */
        int rcv = 131072; setsockopt(real, SOL_SOCKET, SO_RCVBUF, (char *)&rcv, sizeof rcv);
        struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons(port);
        if (bind(real, (struct sockaddr *)&a, sizeof a) != 0) {
            int e = WSAGetLastError(); closesocket(real); real = INVALID_SOCKET; ReleaseSRWLockExclusive(&lock);
            printf("[p2p] can't use UDP port %u (error %d)\n", port, e);
            return -1;
        }
        real_port = port;
        CloseHandle(CreateThread(NULL, 1 << 18, port_thread, NULL, 0, NULL));
        printf("[p2p] P2P port %u open (signaling as %s)\n", port, self_npid);
    }
    if (nbound < MAX_BOUND) {
        bound[nbound].vport = vport; bound[nbound].loop = game_sock;
        int l = sizeof bound[nbound].loop_addr; getsockname(game_sock, (struct sockaddr *)&bound[nbound].loop_addr, &l);
        nbound++;
    }
    ReleaseSRWLockExclusive(&lock);
    if (logon) printf("[p2p] vport %u bound\n", vport);
    return 0;
}
void p2p_unbind(SOCKET game_sock) {
    AcquireSRWLockExclusive(&lock);
    for (int i = 0; i < nbound; i++) if (bound[i].loop == game_sock) { bound[i] = bound[--nbound]; break; }
    ReleaseSRWLockExclusive(&lock);
}
int p2p_sendto(uint16_t src_vport, const uint8_t *data, int len, uint32_t addr, uint16_t port, uint16_t dst_vport) {
    if (real == INVALID_SOCKET || len < 0 || len > 65535 - 6) return -1;
    uint8_t *b = malloc(len + 6);
    wr16le(b, dst_vport); wr16le(b + 2, src_vport); wr16le(b + 4, 1);
    memcpy(b + 6, data, len);
    struct sockaddr_in d = {0}; d.sin_family = AF_INET; d.sin_addr.s_addr = addr; d.sin_port = htons(port);
    int r = sendto(real, (const char *)b, len + 6, 0, (struct sockaddr *)&d, sizeof d);
    free(b);
    if (logon > 0) { static volatile long n; if (InterlockedIncrement(&n) <= 40) printf("[p2p] send %d bytes to %s:%u vport %u\n", len, ipstr(addr), port, dst_vport); }
    return r < 0 ? -1 : r - 6;
}

/* sceNpSignaling: connection ids are peer slots + 1 */
int p2p_conn_status(uint32_t conn_id, uint32_t *addr, uint16_t *port) {
    AcquireSRWLockShared(&lock);
    int st = -1;
    if (conn_id >= 1 && conn_id <= MAX_PEERS && peers[conn_id - 1].npid[0]) {
        peer_t *pe = &peers[conn_id - 1]; st = pe->status; if (addr) *addr = pe->addr; if (port) *port = pe->port;
    }
    ReleaseSRWLockShared(&lock);
    return st;
}
void p2p_terminate(uint32_t conn_id) {
    AcquireSRWLockExclusive(&lock);
    if (conn_id >= 1 && conn_id <= MAX_PEERS && peers[conn_id - 1].npid[0] && peers[conn_id - 1].status && peers[conn_id - 1].addr) {
        peer_t *pe = &peers[conn_id - 1]; uint8_t out[SIG_PACKET_SIZE];
        sig_build(out, SIG_FINISHED, 0, 0, pe->addr, pe->port); sig_send(out, pe->addr, pe->port);
        pe->status = 0; pe->next_ack = pe->next_ping = 0;
    }
    ReleaseSRWLockExclusive(&lock);
}
