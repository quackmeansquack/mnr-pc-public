/* sys_net (the PS3's BSD sockets) on Winsock.
 *
 * Guest sockets are small numbers indexing a table of Winsock sockets. Addresses are the PS3's BSD layout
 * (u8 len, u8 family, be16 port, be32 address, 8 zero bytes; the P2P socket types keep a "virtual port" in the
 * first zero bytes, which plain UDP has no room for, so they are ordinary UDP here). Errors return 0x80010200 | errno
 * (e.g. 0x80010224 = EINPROGRESS, which the game checks for) and set the guest's errno word (Winsock's WSAExxx codes are 10000 + the same numbers).
 *
 * Host names go through MNR_HOSTS ("name=address;name2=address2", as RPCS3's IP/hosts switches) before DNS;
 * modnation.ps3.playstationcloud.com defaults to the MNR Revival server (Sony's own name is gone).
 * MNR_NET_LOG=1 logs every call. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "net.h"
#include "p2p.h"

#define ARG32(n) ((uint32_t)c->r[3 + (n)])
#define ARGS(n) ((int32_t)c->r[3 + (n)])
#define RET(v) (c->r[3] = (uint64_t)(int64_t)(v))

int net_enabled;
static int net_log;
static uint32_t (*galloc)(uint32_t);
static uint32_t errno_addr, h_errno_addr, ntoa_buf, hostent_buf;

#define MAX_SOCK 256
static struct {
    SOCKET s; int type, nbio, used; uint16_t vport;   /* vport: a bound P2P (type 6) socket's virtual port */
    int nodelay, reuse, keepalive;                     /* options to give a redialled socket (redial_overdue) */
    int pend, redials; ULONGLONG pend_t; struct sockaddr_in pend_a;   /* a nonblocking TCP connect not answered yet */
} socks[MAX_SOCK];
static SRWLOCK sock_lock = SRWLOCK_INIT;

enum { E_WOULDBLOCK = 35, E_INPROGRESS = 36, E_BADF = 9, E_INVAL = 22, E_AFNOSUPPORT = 47, E_NOTCONN = 57 };

static void set_errno(int e) { if (errno_addr) st_u32(errno_addr, (uint32_t)e); }
static int fail(ctx_t *c, int e) { set_errno(e); RET((int32_t)(0x80010200u | (e & 0xFF))); return -1; }   /* libnet returns SYS_NET_ERROR_<errno> (and sets errno) */
static int wsa_errno(void) { int w = WSAGetLastError(); return w >= 10000 && w < 11000 ? w - 10000 : w == 0 ? 0 : E_INVAL; }

static SOCKET sock_of(int fd) { return fd >= 0 && fd < MAX_SOCK && socks[fd].used ? socks[fd].s : INVALID_SOCKET; }

/* ------------------------------------------------------------------ host names */
static char hosts_map[2048];
static int map_host(const char *name, uint32_t *addr_be) {   /* MNR_HOSTS: 1 and the address when the name is listed */
    size_t n = strlen(name);
    for (const char *p = hosts_map; *p; ) {
        const char *e = strchr(p, ';'); if (!e) e = p + strlen(p);
        const char *eq = memchr(p, '=', e - p);
        if (eq && (size_t)(eq - p) == n && !_strnicmp(p, name, n)) {
            char ip[64]; size_t l = e - eq - 1; if (l >= sizeof ip) l = sizeof ip - 1;
            memcpy(ip, eq + 1, l); ip[l] = 0;
            struct in_addr a; if (inet_pton(AF_INET, ip, &a) == 1) { *addr_be = ntohl(a.s_addr); return 1; }
        }
        p = *e ? e + 1 : e;
    }
    return 0;
}

const char *net_local_ip(void) {
    static char ip[32];
    if (ip[0]) return ip;
    strcpy(ip, "192.168.1.2");
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);   /* the address Windows would use to reach the internet (nothing is sent) */
    if (s != INVALID_SOCKET) {
        struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons(53); inet_pton(AF_INET, "8.8.8.8", &a.sin_addr);
        if (connect(s, (struct sockaddr *)&a, sizeof a) == 0) {
            struct sockaddr_in me; int l = sizeof me;
            if (getsockname(s, (struct sockaddr *)&me, &l) == 0) inet_ntop(AF_INET, &me.sin_addr, ip, sizeof ip);
        }
        closesocket(s);
    }
    return ip;
}

void net_setup(uint32_t (*alloc)(uint32_t)) {
    galloc = alloc;
    net_enabled = getenv("MNR_NET") && atoi(getenv("MNR_NET"));
    net_log = getenv("MNR_NET_LOG") ? atoi(getenv("MNR_NET_LOG")) : 0;   /* 2: also the data (text) */
    strcpy(hosts_map, "modnation.ps3.playstationcloud.com=45.32.179.33");
    const char *h = getenv("MNR_HOSTS");
    if (h && *h) { snprintf(hosts_map, sizeof hosts_map, "%s;modnation.ps3.playstationcloud.com=45.32.179.33", h); }
    errno_addr = galloc(16); h_errno_addr = galloc(16);   /* set before the game first asks where errno is */
    if (net_enabled) {
        WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
        printf("[net] online: this PC is %s, host map %s\n", net_local_ip(), hosts_map);
    }}

/* Online on a server of our own: the launcher mapped the game's server name to an address other than MNR Revival's
 * (the default when MNR_HOSTS names none). Read from the environment, so it holds before net_setup runs. */
#define REVIVAL_IP "45.32.179.33"
int net_own_server(void) {
    const char *on = getenv("MNR_NET"), *h = getenv("MNR_HOSTS");
    if (!on || !atoi(on) || !h) return 0;
    const char *eq = strchr(h, '=');
    if (!eq) return 0;
    char ip[64]; size_t n = strcspn(eq + 1, ";, ");
    if (n == 0 || n >= sizeof ip) return 0;
    memcpy(ip, eq + 1, n); ip[n] = 0;
    return strcmp(ip, REVIVAL_IP) != 0;
}

void net_errno_loc(ctx_t *c) { RET(errno_addr); }
void net_h_errno_loc(ctx_t *c) { RET(h_errno_addr); }

void net_inet_aton(ctx_t *c) {   /* (const char *cp, struct in_addr *inp) -> 1 valid, 0 invalid */
    unsigned a, b, cc, d;
    if (sscanf((const char *)gp(ARG32(0)), "%u.%u.%u.%u", &a, &b, &cc, &d) == 4 && a < 256 && b < 256 && cc < 256 && d < 256) {
        if (ARG32(1)) st_u32(ARG32(1), a << 24 | b << 16 | cc << 8 | d);
        RET(1);
    } else RET(0);
}
void net_inet_addr(ctx_t *c) {
    unsigned a, b, cc, d;
    if (sscanf((const char *)gp(ARG32(0)), "%u.%u.%u.%u", &a, &b, &cc, &d) == 4 && a < 256 && b < 256 && cc < 256 && d < 256)
        RET(a << 24 | b << 16 | cc << 8 | d);
    else c->r[3] = 0xFFFFFFFFu;
}
void net_inet_ntoa(ctx_t *c) {   /* (struct in_addr by value) */
    if (!ntoa_buf) ntoa_buf = galloc(32);
    uint32_t a = ARG32(0);
    snprintf((char *)gp(ntoa_buf), 32, "%u.%u.%u.%u", a >> 24, a >> 16 & 255, a >> 8 & 255, a & 255);
    RET(ntoa_buf);
}
void net_gethostbyname(ctx_t *c) {   /* -> struct hostent {name, aliases, s32 addrtype, s32 length, addr_list} */
    const char *name = (const char *)gp(ARG32(0));
    uint32_t addr = 0;
    int ok = map_host(name, &addr);
    if (!ok && net_enabled) {
        struct addrinfo hint = {0}, *res = NULL; hint.ai_family = AF_INET;
        if (getaddrinfo(name, NULL, &hint, &res) == 0 && res) { addr = ntohl(((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr); ok = 1; }
        if (res) freeaddrinfo(res);
    }
    if (net_log || net_enabled) printf("[net] gethostbyname(%s) -> %s%u.%u.%u.%u\n", name, ok ? "" : "(not found) ", addr >> 24, addr >> 16 & 255, addr >> 8 & 255, addr & 255);
    if (!ok) { if (h_errno_addr) st_u32(h_errno_addr, 1); RET(0); return; }   /* HOST_NOT_FOUND */
    if (!hostent_buf) hostent_buf = galloc(512);
    uint32_t h = hostent_buf, nm = h + 64, al = h + 32, list = h + 40, a0 = h + 48;
    memset(gp(h), 0, 512);
    snprintf((char *)gp(nm), 256, "%s", name);
    st_u32(h, nm); st_u32(h + 4, al); st_u32(h + 8, 2); st_u32(h + 12, 4); st_u32(h + 16, list);
    st_u32(al, 0); st_u32(list, a0); st_u32(list + 4, 0); st_u32(a0, addr);
    RET(h);
}

/* ------------------------------------------------------------------ addresses */
static int addr_in(uint32_t ga, uint32_t len, struct sockaddr_in *out) {
    memset(out, 0, sizeof *out);
    if (!ga || len < 8) return 0;
    out->sin_family = AF_INET;
    out->sin_port = htons(ld_u16(ga + 2));
    out->sin_addr.s_addr = htonl(ld_u32(ga + 4));
    return 1;
}
static void addr_out(uint32_t ga, uint32_t plen, const struct sockaddr_in *a) {
    if (!ga) return;
    memset(gp(ga), 0, 16);
    st_u8(ga, 16); st_u8(ga + 1, 2);
    st_u16(ga + 2, ntohs(a->sin_port)); st_u32(ga + 4, ntohl(a->sin_addr.s_addr));
    if (plen) st_u32(plen, 16);
}
static const char *fmt_addr(const struct sockaddr_in *a) {
    static __thread char b[32];
    uint32_t x = ntohl(a->sin_addr.s_addr);
    snprintf(b, sizeof b, "%u.%u.%u.%u:%u", x >> 24, x >> 16 & 255, x >> 8 & 255, x & 255, ntohs(a->sin_port));
    return b;
}

/* ------------------------------------------------------------------ sockets */
void net_socket(ctx_t *c) {   /* (family, type, protocol) */
    int fam = ARGS(0), type = ARGS(1), proto = ARGS(2);
    if (!net_enabled) { fail(c, E_AFNOSUPPORT); return; }
    if (fam != 2) { fail(c, E_AFNOSUPPORT); return; }
    int wt = type == 1 || type == 10 ? SOCK_STREAM : SOCK_DGRAM;   /* 1 stream, 2 dgram, 6 dgram P2P, 10 stream P2P, 3 raw */
    if (type == 3) { fail(c, E_AFNOSUPPORT); return; }
    SOCKET s = socket(AF_INET, wt, wt == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP);
    if (s == INVALID_SOCKET) { fail(c, wsa_errno()); return; }
    AcquireSRWLockExclusive(&sock_lock);
    int fd = -1;
    for (int i = 1; i < MAX_SOCK; i++) if (!socks[i].used) { fd = i; socks[i].used = 1; socks[i].s = s; socks[i].type = type; socks[i].nbio = 0; socks[i].nodelay = socks[i].reuse = socks[i].keepalive = socks[i].pend = 0; break; }
    ReleaseSRWLockExclusive(&sock_lock);
    if (fd < 0) { closesocket(s); fail(c, 24); return; }   /* EMFILE */
    if (net_log) printf("[net] socket(%d, %d, %d) = %d\n", fam, type, proto, fd);
    RET(fd);
}
void net_socketclose(ctx_t *c) {
    int fd = ARGS(0);
    SOCKET s = sock_of(fd);
    if (s == INVALID_SOCKET) { fail(c, E_BADF); return; }
    if (socks[fd].type == 6) p2p_unbind(s);
    AcquireSRWLockExclusive(&sock_lock); socks[fd].used = 0; ReleaseSRWLockExclusive(&sock_lock);
    closesocket(s);
    if (net_log) printf("[net] close(%d)\n", fd);
    RET(0);
}
void net_shutdown(ctx_t *c) {
    SOCKET s = sock_of(ARGS(0));
    if (s == INVALID_SOCKET) { fail(c, E_BADF); return; }
    if (shutdown(s, ARGS(1)) != 0) { fail(c, wsa_errno()); return; }
    RET(0);
}
void net_setsockopt(ctx_t *c) {   /* (s, level, optname, optval, optlen) */
    int fd = ARGS(0), level = ARGS(1), opt = ARGS(2); uint32_t val = ARG32(3), len = ARG32(4);
    SOCKET s = sock_of(fd);
    if (s == INVALID_SOCKET) { fail(c, E_BADF); return; }
    int v = len >= 4 && val ? (int)ld_u32(val) : 0, r = 0;
    if (level == 0xFFFF) {
        switch (opt) {
        case 0x1100: { u_long nb = v != 0; r = ioctlsocket(s, FIONBIO, &nb); socks[fd].nbio = v != 0; break; }   /* SO_NBIO */
        case 0x0004: r = setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *)&v, 4); socks[fd].reuse = v != 0; break;
        case 0x0020: r = setsockopt(s, SOL_SOCKET, SO_BROADCAST, (char *)&v, 4); break;
        case 0x0008: r = setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (char *)&v, 4); socks[fd].keepalive = v != 0; break;
        case 0x1001: r = setsockopt(s, SOL_SOCKET, SO_SNDBUF, (char *)&v, 4); break;
        case 0x1002: r = setsockopt(s, SOL_SOCKET, SO_RCVBUF, (char *)&v, 4); break;
        case 0x1005: case 0x1006: {   /* SO_SNDTIMEO / SO_RCVTIMEO: struct timeval {s64 sec; s64 usec} */
            DWORD ms = len >= 16 ? (DWORD)(ld_u64(val) * 1000 + ld_u64(val + 8) / 1000) : 0;
            r = setsockopt(s, SOL_SOCKET, opt == 0x1005 ? SO_SNDTIMEO : SO_RCVTIMEO, (char *)&ms, 4); break;
        }
        case 0x0080: {   /* SO_LINGER {s32 onoff; s32 linger} */
            struct linger l = { (u_short)(len >= 8 ? ld_u32(val) != 0 : 0), (u_short)(len >= 8 ? ld_u32(val + 4) : 0) };
            r = setsockopt(s, SOL_SOCKET, SO_LINGER, (char *)&l, sizeof l); break;
        }
        default: break;   /* SO_USECRYPTO, SO_USESIGNATURE and other PS3-only options: accepted */
        }
    } else if (level == 6 && opt == 1) { r = setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)&v, 4); socks[fd].nodelay = v != 0; }
    else if (level == 0) {   /* IPPROTO_IP: TTL, multicast...: best effort */
        if (opt == 4) r = setsockopt(s, IPPROTO_IP, IP_TTL, (char *)&v, 4);
    }
    if (net_log) printf("[net] setsockopt(%d, 0x%x, 0x%x, %d) = %d\n", fd, level, opt, v, r);
    if (r != 0) { fail(c, wsa_errno()); return; }
    RET(0);
}
void net_bind(ctx_t *c) {
    int fd = ARGS(0); SOCKET s = sock_of(fd); struct sockaddr_in a;
    if (s == INVALID_SOCKET) { fail(c, E_BADF); return; }
    if (!addr_in(ARG32(1), ARG32(2), &a)) { fail(c, E_INVAL); return; }
    {   /* MNR_P2P_PORT=<port>: the game's race (P2P) port, 3658 on every console, on another local port. Two PCs behind
         * one router can't both have 3658: the second then looked unreachable (red globe in ModSpot). */
        static int p2p = -1; if (p2p < 0) { const char *e = getenv("MNR_P2P_PORT"); p2p = e ? atoi(e) : 0; if (p2p < 0 || p2p > 65535) p2p = 0; }
        if (p2p && ntohs(a.sin_port) == 3658) a.sin_port = htons((u_short)p2p);
    }
    if (socks[fd].type == 6) {   /* SOCK_DGRAM_P2P {len, family, port, addr, u16 vport}: a virtual port on the shared P2P port (p2p.c);
                                  * this socket becomes the loopback socket the P2P port delivers the vport's datagrams to */
        uint16_t vport = ARG32(2) >= 10 ? ld_u16(ARG32(1) + 8) : 0;
        if (!vport) { static uint16_t next = 30000; vport = next++; }
        struct sockaddr_in lo = {0}; lo.sin_family = AF_INET; lo.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(s, (struct sockaddr *)&lo, sizeof lo) != 0 || p2p_bind(ntohs(a.sin_port), vport, s) != 0) { fail(c, wsa_errno() ? wsa_errno() : 48); return; }   /* EADDRINUSE */
        socks[fd].vport = vport;
        if (net_log) printf("[net] bind(%d, %s vport %u) = 0 (P2P)\n", fd, fmt_addr(&a), vport);
        RET(0); return;
    }
    int r = bind(s, (struct sockaddr *)&a, sizeof a);
    if (net_log) printf("[net] bind(%d, %s) = %d\n", fd, fmt_addr(&a), r ? WSAGetLastError() : 0);
    if (r != 0) { fail(c, wsa_errno()); return; }
    RET(0);
}
void net_listen(ctx_t *c) {
    SOCKET s = sock_of(ARGS(0));
    if (s == INVALID_SOCKET) { fail(c, E_BADF); return; }
    if (listen(s, ARGS(1)) != 0) { fail(c, wsa_errno()); return; }
    RET(0);
}
void net_accept(ctx_t *c) {   /* (s, addr, paddrlen) */
    int fd = ARGS(0); SOCKET s = sock_of(fd);
    if (s == INVALID_SOCKET) { fail(c, E_BADF); return; }
    struct sockaddr_in a; int l = sizeof a;
    SOCKET n = accept(s, (struct sockaddr *)&a, &l);
    if (n == INVALID_SOCKET) { fail(c, wsa_errno()); return; }
    AcquireSRWLockExclusive(&sock_lock);
    int nfd = -1;
    for (int i = 1; i < MAX_SOCK; i++) if (!socks[i].used) { nfd = i; socks[i].used = 1; socks[i].s = n; socks[i].type = socks[fd].type; socks[i].nbio = 0; socks[i].nodelay = socks[i].reuse = socks[i].keepalive = socks[i].pend = 0; break; }
    ReleaseSRWLockExclusive(&sock_lock);
    if (nfd < 0) { closesocket(n); fail(c, 24); return; }
    addr_out(ARG32(1), ARG32(2), &a);
    if (net_log) printf("[net] accept(%d) = %d from %s\n", fd, nfd, fmt_addr(&a));
    RET(nfd);
}
/* A TCP connect still unanswered after REDIAL_MS is made again on a fresh socket (a new source port), the game keeping
 * its socket number. Routers drop some connections outright: from a PC reaching its own server through the router's
 * public address, about 1 in 11 never connected (2026-10-25), and the game waited 20-30 s for it, then went offline. */
#define REDIAL_MS 1000
#define REDIAL_MAX 8
static SRWLOCK redial_lock = SRWLOCK_INIT;
static SOCKET tcp_socket_like(int fd) {   /* nonblocking, with the options the game gave fd */
    SOCKET n = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (n == INVALID_SOCKET) return n;
    u_long nb = 1; ioctlsocket(n, FIONBIO, &nb);
    int one = 1;
    if (socks[fd].nodelay) setsockopt(n, IPPROTO_TCP, TCP_NODELAY, (char *)&one, 4);
    if (socks[fd].reuse) setsockopt(n, SOL_SOCKET, SO_REUSEADDR, (char *)&one, 4);
    if (socks[fd].keepalive) setsockopt(n, SOL_SOCKET, SO_KEEPALIVE, (char *)&one, 4);
    return n;
}
static SOCKET redial(int fd, const struct sockaddr_in *a) {   /* fd's socket replaced by a new one connecting to a */
    SOCKET n = tcp_socket_like(fd);
    if (n == INVALID_SOCKET) return INVALID_SOCKET;
    connect(n, (const struct sockaddr *)a, sizeof *a);
    AcquireSRWLockExclusive(&sock_lock); SOCKET old = socks[fd].s; socks[fd].s = n; ReleaseSRWLockExclusive(&sock_lock);
    closesocket(old);
    return n;
}
static int answered(SOCKET s) {   /* the connect finished: connected (writable) or failed */
    fd_set w, e; FD_ZERO(&w); FD_ZERO(&e); FD_SET(s, &w); FD_SET(s, &e);
    struct timeval tv = {0, 0};
    return select(0, NULL, &w, &e, &tv) > 0;
}
/* Redials the overdue nonblocking connects among the sockets a poll/select call is waiting on (mine[fd] set: the
 * calling thread's own, so no other thread is inside a select on the socket being replaced); returns how many of them
 * are still pending (the caller then waits in short slices so the next redial isn't held up by a long poll). */
static int redial_overdue(const unsigned char *mine) {
    int pending = 0;
    AcquireSRWLockExclusive(&redial_lock);
    ULONGLONG now = GetTickCount64();
    for (int fd = 1; fd < MAX_SOCK; fd++) {
        if (!mine[fd] || !socks[fd].used || !socks[fd].pend) continue;
        if (answered(socks[fd].s)) { socks[fd].pend = 0; continue; }
        if (socks[fd].redials >= REDIAL_MAX) { socks[fd].pend = 0; continue; }   /* the game's own timeout decides */
        pending++;
        if (now - socks[fd].pend_t < REDIAL_MS) continue;
        if (redial(fd, &socks[fd].pend_a) == INVALID_SOCKET) continue;
        socks[fd].pend_t = now; socks[fd].redials++;
        printf("[net] connect(%d, %s) unanswered after %d ms: retry %d on a new socket\n", fd, fmt_addr(&socks[fd].pend_a), REDIAL_MS, socks[fd].redials);
    }
    ReleaseSRWLockExclusive(&redial_lock);
    return pending;
}

void net_connect(ctx_t *c) {
    int fd = ARGS(0); SOCKET s = sock_of(fd); struct sockaddr_in a;
    if (s == INVALID_SOCKET) { fail(c, E_BADF); return; }
    if (!addr_in(ARG32(1), ARG32(2), &a)) { fail(c, E_INVAL); return; }
    int stream = socks[fd].type == 1 || socks[fd].type == 10;
    if (stream && !socks[fd].nbio) {   /* blocking: connected here, nonblocking underneath, redialled as above */
        u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
        int r = connect(s, (struct sockaddr *)&a, sizeof a), e = r ? WSAGetLastError() : 0;
        printf("[net] connect(%d, %s) = %d\n", fd, fmt_addr(&a), e == WSAEWOULDBLOCK ? 0 : e);
        if (r != 0 && e != WSAEWOULDBLOCK) { nb = 0; ioctlsocket(s, FIONBIO, &nb); fail(c, e == WSAEINVAL ? 37 : e - 10000); return; }
        for (int tries = 0; r != 0; tries++) {
            fd_set w, x; FD_ZERO(&w); FD_ZERO(&x); FD_SET(s, &w); FD_SET(s, &x);
            struct timeval tv = {REDIAL_MS / 1000, REDIAL_MS % 1000 * 1000};
            select(0, NULL, &w, &x, &tv);
            if (FD_ISSET(s, &w)) break;
            if (FD_ISSET(s, &x)) { int err = 0, l = 4; getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &l); fail(c, err >= 10000 ? err - 10000 : 61); return; }   /* ECONNREFUSED */
            if (tries >= REDIAL_MAX) { fail(c, 60); return; }   /* ETIMEDOUT */
            printf("[net] connect(%d, %s) unanswered after %d ms: retry %d on a new socket\n", fd, fmt_addr(&a), REDIAL_MS, tries + 1);
            if ((s = redial(fd, &a)) == INVALID_SOCKET) { fail(c, 55); return; }   /* ENOBUFS */
        }
        nb = 0; ioctlsocket(s, FIONBIO, &nb);
        RET(0); return;
    }
    int r = connect(s, (struct sockaddr *)&a, sizeof a), e = r ? WSAGetLastError() : 0;
    printf("[net] connect(%d, %s)%s = %d\n", fd, fmt_addr(&a), socks[fd].nbio ? " nonblocking" : "", e);
    if (r != 0) {
        if (e == WSAEWOULDBLOCK) {   /* BSD reports a pending connect as EINPROGRESS */
            if (stream) { socks[fd].pend_a = a; socks[fd].pend_t = GetTickCount64(); socks[fd].redials = 0; socks[fd].pend = 1; }
            fail(c, E_INPROGRESS); return;
        }
        if (e == WSAEINVAL) { fail(c, 37); return; }                  /* EALREADY */
        fail(c, e - 10000); return;
    }
    RET(0);
}
void net_getpeername(ctx_t *c) {
    SOCKET s = sock_of(ARGS(0));
    if (s == INVALID_SOCKET) { fail(c, E_BADF); return; }
    struct sockaddr_in a; int l = sizeof a;
    if (getpeername(s, (struct sockaddr *)&a, &l) != 0) { fail(c, wsa_errno()); return; }
    addr_out(ARG32(1), ARG32(2), &a);
    RET(0);
}

/* MSG_DONTWAIT (0x80) on a blocking socket: answer EWOULDBLOCK instead of waiting */
static int would_block(SOCKET s, int write) {
    fd_set f; FD_ZERO(&f); FD_SET(s, &f);
    struct timeval tv = {0, 0};
    return select(0, write ? NULL : &f, write ? &f : NULL, NULL, &tv) == 0;
}
static int win_flags(int f) { return (f & 2 ? MSG_PEEK : 0) | (f & 1 ? MSG_OOB : 0) | (f & 0x40 ? MSG_WAITALL : 0); }

static void dump_data(const char *what, int fd, const uint8_t *p, int n) {   /* MNR_NET_LOG=2 */
    printf("[net] %s(%d) data:\n", what, fd);
    for (int i = 0; i < n && i < 4096; i++) putchar(p[i] == 10 || (p[i] >= 32 && p[i] < 127) ? p[i] : '.');
    printf("\n[net] -- end\n");
}
static void xfer_result(ctx_t *c, int fd, const char *what, int r, uint32_t len) {
    if (r == SOCKET_ERROR) {
        int e = wsa_errno();
        if (net_log && e != E_WOULDBLOCK) printf("[net] %s(%d, %u) failed: errno %d\n", what, fd, len, e);
        fail(c, e); return;
    }
    socks[fd].pend = 0;   /* data moved: connected */
    if (net_log) printf("[net] %s(%d, %u) = %d\n", what, fd, len, r);
    if (net_log >= 2 && r > 0) dump_data(what, fd, gp(ARG32(1)), r);
    {   /* MNR_NET_HEX=1 (debug): UDP datagrams in hex, and where the P2P port number (3658) appears in them */
        static int hex = -1; if (hex < 0) hex = getenv("MNR_NET_HEX") != NULL;
        if (hex && r > 0 && socks[fd].type == 2) {
            const uint8_t *p = gp(ARG32(1)); static volatile long n;
            if (InterlockedIncrement(&n) <= 400 || r <= 40) {   /* short ones (relay control messages) all session */
                printf("[nethex] %llu ms %s(%d) %d:", (unsigned long long)GetTickCount64(), what, fd, r);
                for (int i = 0; i < r && i < 192; i++) printf(" %02x", p[i]);
                printf("\n");
                for (int i = 0; i + 1 < r; i++) if ((p[i] == 0x0E && p[i + 1] == 0x4A) || (p[i] == 0x4A && p[i + 1] == 0x0E)) printf("[nethex]   3658 (%s) at offset %d\n", p[i] == 0x0E ? "big-endian" : "little-endian", i);
            }
        }
    }
    RET(r);
}
void net_send(ctx_t *c) {   /* (s, buf, len, flags) */
    int fd = ARGS(0), fl = ARGS(3); SOCKET s = sock_of(fd);
    if (s == INVALID_SOCKET) { fail(c, E_BADF); return; }
    if ((fl & 0x80) && !socks[fd].nbio && would_block(s, 1)) { fail(c, E_WOULDBLOCK); return; }
    xfer_result(c, fd, "send", send(s, (const char *)gp(ARG32(1)), (int)ARG32(2), win_flags(fl) & MSG_OOB), ARG32(2));
}
void net_sendto(ctx_t *c) {   /* (s, buf, len, flags, addr, addrlen) */
    int fd = ARGS(0), fl = ARGS(3); SOCKET s = sock_of(fd); struct sockaddr_in a;
    if (s == INVALID_SOCKET) { fail(c, E_BADF); return; }
    if ((fl & 0x80) && !socks[fd].nbio && would_block(s, 1)) { fail(c, E_WOULDBLOCK); return; }
    if (socks[fd].type == 6) {   /* through the P2P port, with the destination's vport (sockaddr_in_p2p) */
        if (!addr_in(ARG32(4), ARG32(5), &a)) { fail(c, E_INVAL); return; }
        int r = p2p_sendto(socks[fd].vport, gp(ARG32(1)), (int)ARG32(2), a.sin_addr.s_addr, ntohs(a.sin_port), ld_u16(ARG32(4) + 8));
        if (r < 0) { fail(c, E_INVAL); return; }
        if (net_log > 1) printf("[net] sendto(%d, %u) = %d to %s vport %u (P2P)\n", fd, ARG32(2), r, fmt_addr(&a), ld_u16(ARG32(4) + 8));
        RET(r); return;
    }
    int r = addr_in(ARG32(4), ARG32(5), &a)
        ? sendto(s, (const char *)gp(ARG32(1)), (int)ARG32(2), 0, (struct sockaddr *)&a, sizeof a)
        : send(s, (const char *)gp(ARG32(1)), (int)ARG32(2), 0);
    xfer_result(c, fd, "sendto", r, ARG32(2));
}
void net_recv(ctx_t *c) {   /* (s, buf, len, flags) */
    int fd = ARGS(0), fl = ARGS(3); SOCKET s = sock_of(fd);
    if (s == INVALID_SOCKET) { fail(c, E_BADF); return; }
    if ((fl & 0x80) && !socks[fd].nbio && would_block(s, 0)) { fail(c, E_WOULDBLOCK); return; }
    int r = recv(s, (char *)gp(ARG32(1)), (int)ARG32(2), win_flags(fl));
    if (r > 0) for (uint64_t a = ARG32(1) & ~0xFFFu; a < (uint64_t)ARG32(1) + r; a += 0x1000) { volatile uint8_t *b = gp(a < ARG32(1) ? ARG32(1) : (uint32_t)a); *b = *b; }   /* kernel writes: marked for the renderer (as fread in host.c) */
    xfer_result(c, fd, "recv", r, ARG32(2));
}
void net_recvfrom(ctx_t *c) {   /* (s, buf, len, flags, addr, paddrlen) */
    int fd = ARGS(0), fl = ARGS(3); SOCKET s = sock_of(fd);
    if (s == INVALID_SOCKET) { fail(c, E_BADF); return; }
    if ((fl & 0x80) && !socks[fd].nbio && would_block(s, 0)) { fail(c, E_WOULDBLOCK); return; }
    struct sockaddr_in a = {0}; int l = sizeof a;
    if (socks[fd].type == 6) {   /* from the P2P port: {addr (raw), port (be), source vport (be)} + data */
        static __thread uint8_t *b; if (!b) b = malloc(65536 + 8);
        int r = recvfrom(s, (char *)b, 65536 + 8, win_flags(fl) & ~(MSG_WAITALL | MSG_PEEK), NULL, NULL);
        if (r == SOCKET_ERROR || r < 8) { xfer_result(c, fd, "recvfrom", r == SOCKET_ERROR ? r : 0, ARG32(2)); return; }
        int len = r - 8; if (len > (int)ARG32(2)) len = (int)ARG32(2);
        memcpy(gp(ARG32(1)), b + 8, len);
        memcpy(&a.sin_addr.s_addr, b, 4); memcpy(&a.sin_port, b + 4, 2);
        addr_out(ARG32(4), ARG32(5), &a);
        if (ARG32(4)) st_u16(ARG32(4) + 8, (uint16_t)(b[6] << 8 | b[7]));
        if (net_log > 1) printf("[net] recvfrom(%d) = %d from %s vport %u (P2P)\n", fd, len, fmt_addr(&a), b[6] << 8 | b[7]);
        RET(len); return;
    }
    int r = recvfrom(s, (char *)gp(ARG32(1)), (int)ARG32(2), win_flags(fl) & ~MSG_WAITALL, (struct sockaddr *)&a, &l);
    if (r != SOCKET_ERROR) addr_out(ARG32(4), ARG32(5), &a);
    if (r > 0) for (uint64_t p = ARG32(1) & ~0xFFFu; p < (uint64_t)ARG32(1) + r; p += 0x1000) { volatile uint8_t *b = gp(p < ARG32(1) ? ARG32(1) : (uint32_t)p); *b = *b; }   /* kernel writes (as in net_recv) */
    xfer_result(c, fd, "recvfrom", r, ARG32(2));
}

/* socketpoll(struct pollfd {s32 fd; s16 events; s16 revents} *fds, nfds, timeout ms), BSD poll bits:
 * IN 1, PRI 2, OUT 4, ERR 8, HUP 0x10, NVAL 0x20, RDNORM 0x40 */
void net_socketpoll(ctx_t *c) {
    uint32_t fds = ARG32(0); int n = ARGS(1), ms = ARGS(2);
    if (n < 0 || n > 64) { fail(c, E_INVAL); return; }
    fd_set rd, wr, ex;
    unsigned char mine[MAX_SOCK] = {0};
    for (int i = 0; i < n; i++) { int fd = (int32_t)ld_u32(fds + i * 8); if (fd > 0 && fd < MAX_SOCK) mine[fd] = 1; }
    ULONGLONG start = GetTickCount64();
    for (;;) {   /* while connects are pending, in slices of 250 ms with redials between (redial_overdue) */
        int pend = redial_overdue(mine);
        FD_ZERO(&rd); FD_ZERO(&wr); FD_ZERO(&ex);
        int any = 0, ready = 0;
        for (int i = 0; i < n; i++) {
            int fd = (int32_t)ld_u32(fds + i * 8); uint16_t ev = ld_u16(fds + i * 8 + 4);
            st_u16(fds + i * 8 + 6, 0);
            if (fd < 0) continue;
            SOCKET s = sock_of(fd);
            if (s == INVALID_SOCKET) { st_u16(fds + i * 8 + 6, 0x20); ready++; continue; }
            if (ev & 0x43) FD_SET(s, &rd);
            if (ev & 0x4) FD_SET(s, &wr);
            FD_SET(s, &ex); any = 1;
        }
        if (ready || !any) { if (!any && !ready && ms > 0) Sleep(ms); RET(ready); return; }
        int left = ms < 0 ? -1 : (int)(ms - (int)(GetTickCount64() - start)); if (ms >= 0 && left < 0) left = 0;
        int wait = pend && (left < 0 || left > 250) ? 250 : left;
        struct timeval tv = {wait / 1000, (wait % 1000) * 1000};
        int r = select(0, &rd, &wr, &ex, wait < 0 ? NULL : &tv);
        if (r == SOCKET_ERROR) { fail(c, wsa_errno()); return; }
        if (r > 0 || !pend || (ms >= 0 && GetTickCount64() - start >= (ULONGLONG)ms)) break;
    }
    int ready = 0;
    for (int i = 0; i < n; i++) {
        int fd = (int32_t)ld_u32(fds + i * 8); uint16_t ev = ld_u16(fds + i * 8 + 4), re = 0;
        SOCKET s = sock_of(fd);
        if (fd < 0 || s == INVALID_SOCKET) continue;
        if (FD_ISSET(s, &rd)) re |= ev & 0x43;
        if (FD_ISSET(s, &wr)) re |= ev & 0x4;
        if (FD_ISSET(s, &ex)) re |= 0x8;
        if (re & 0xC) socks[fd].pend = 0;
        st_u16(fds + i * 8 + 6, re);
        if (re) ready++;
    }
    RET(ready);
}
/* socketselect(nfds, readfds, writefds, exceptfds, struct timeval {s64 sec; s64 usec} *timeout); fd_set = 32 be32
 * words, socket n at word n / 32, bit n % 32 */
void net_socketselect(ctx_t *c) {
    int n = ARGS(0); uint32_t gs[3] = {ARG32(1), ARG32(2), ARG32(3)}, to = ARG32(4);
    if (n > MAX_SOCK) n = MAX_SOCK;
    fd_set f[3];
    int64_t ms = -1;   /* the timeout, -1 = none */
    if (to) { int64_t sec = (int64_t)ld_u64(to), us = (int64_t)ld_u64(to + 8); ms = sec * 1000 + us / 1000; if (ms < 0) ms = 0; }
    unsigned char mine[MAX_SOCK] = {0};
    for (int k = 0; k < 3; k++) if (gs[k]) for (int fd = 1; fd < n; fd++) if (ld_u32(gs[k] + fd / 32 * 4) >> (fd % 32) & 1) mine[fd] = 1;
    ULONGLONG start = GetTickCount64();
    for (;;) {   /* while connects are pending, in slices of 250 ms with redials between (as socketpoll) */
        int pend = redial_overdue(mine), any = 0;
        for (int k = 0; k < 3; k++) {
            FD_ZERO(&f[k]);
            if (gs[k]) for (int fd = 0; fd < n; fd++)
                if (ld_u32(gs[k] + fd / 32 * 4) >> (fd % 32) & 1) { SOCKET s = sock_of(fd); if (s != INVALID_SOCKET) { FD_SET(s, &f[k]); any = 1; } }
        }
        if (!any) { if (ms > 0) Sleep((DWORD)ms); break; }
        int64_t left = ms < 0 ? -1 : ms - (int64_t)(GetTickCount64() - start); if (ms >= 0 && left < 0) left = 0;
        int64_t wait = pend && (left < 0 || left > 250) ? 250 : left;
        struct timeval tv = {(long)(wait / 1000), (long)(wait % 1000 * 1000)};
        int r = select(0, gs[0] ? &f[0] : NULL, gs[1] ? &f[1] : NULL, gs[2] ? &f[2] : NULL, wait < 0 ? NULL : &tv);
        if (r == SOCKET_ERROR) { fail(c, wsa_errno()); return; }
        if (r > 0 || !pend || (ms >= 0 && GetTickCount64() - start >= (ULONGLONG)ms)) break;
    }
    int r = 0;
    for (int k = 0; k < 3; k++) if (gs[k]) {
        for (int w = 0; w < (n + 31) / 32; w++) {
            uint32_t bits = 0;
            for (int b = 0; b < 32; b++) { int fd = w * 32 + b; if (fd >= n) break;
                SOCKET s = sock_of(fd);
                if (s != INVALID_SOCKET && (ld_u32(gs[k] + w * 4) >> b & 1) && FD_ISSET(s, &f[k])) { bits |= 1u << b; r++; if (k) socks[fd].pend = 0; } }
            st_u32(gs[k] + w * 4, bits);
        }
    }
    RET(r);
}

/* ------------------------------------------------------------------ a plain HTTP GET (the launcher's ticket service) */
int net_http_get(const char *hostport, const char *path, uint8_t **body) {
    char host[64] = "127.0.0.1"; int port = 80;
    const char *colon = strrchr(hostport, ':');
    if (colon) { size_t l = colon - hostport; if (l && l < sizeof host) { memcpy(host, hostport, l); host[l] = 0; } port = atoi(colon + 1); }
    else snprintf(host, sizeof host, "%s", hostport);
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return -1;
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons((u_short)port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1 || connect(s, (struct sockaddr *)&a, sizeof a) != 0) { closesocket(s); return -1; }
    DWORD to = 30000; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&to, 4);
    char req[3000]; int n = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n", path, host);
    send(s, req, n, 0);
    int cap = 1 << 20, got = 0; char *buf = malloc(cap);
    for (int r; got < cap && (r = recv(s, buf + got, cap - got, 0)) > 0; ) got += r;
    closesocket(s);
    int len = -1;
    for (int i = 0; i + 3 < got; i++) if (!memcmp(buf + i, "\r\n\r\n", 4)) {
        if (got > 12 && !memcmp(buf + 9, "200", 3)) { len = got - i - 4; *body = malloc(len ? len : 1); memcpy(*body, buf + i + 4, len); }
        break;
    }
    free(buf);
    return len;
}
