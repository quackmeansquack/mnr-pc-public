/* sys_net sockets on Winsock, for the game's online services (net.c). Off unless MNR_NET=1: the game then sees
 * no network, as before. */
#pragma once
#include "runtime.h"

extern int net_enabled;                      /* MNR_NET=1 */
void net_setup(uint32_t (*alloc)(uint32_t));  /* at startup (guest buffers come from alloc) */
const char *net_local_ip(void);               /* this PC's IPv4 address as text (cellNetCtlGetInfo) */

void net_errno_loc(ctx_t *c);
void net_h_errno_loc(ctx_t *c);
void net_inet_aton(ctx_t *c);
void net_inet_addr(ctx_t *c);
void net_inet_ntoa(ctx_t *c);
void net_gethostbyname(ctx_t *c);
void net_socket(ctx_t *c);
void net_socketclose(ctx_t *c);
void net_shutdown(ctx_t *c);
void net_setsockopt(ctx_t *c);
void net_bind(ctx_t *c);
void net_listen(ctx_t *c);
void net_accept(ctx_t *c);
void net_connect(ctx_t *c);
void net_getpeername(ctx_t *c);
void net_send(ctx_t *c);
void net_sendto(ctx_t *c);
void net_recv(ctx_t *c);
void net_recvfrom(ctx_t *c);
void net_socketpoll(ctx_t *c);
void net_socketselect(ctx_t *c);
int net_http_get(const char *hostport, const char *path, uint8_t **body);   /* plain HTTP/1.0 GET: body length (malloc'd), or -1 */
