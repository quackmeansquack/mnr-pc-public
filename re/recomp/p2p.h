/* The PS3 P2P port (UDP 3658, RPCS3-compatible framing) and NP signaling (p2p.c). */
#pragma once
#include <winsock2.h>
#include <stdint.h>

/* the game binds a SOCK_DGRAM_P2P socket: <game_sock> is its loopback socket, which receives
 * {addr (raw), port (be16), source vport (be16)} + data for every datagram addressed to <vport> */
int p2p_bind(uint16_t port, uint16_t vport, SOCKET game_sock);
void p2p_unbind(SOCKET game_sock);
/* a datagram from the game's P2P socket: addr is network order, ports host order; returns bytes sent (data) or -1 */
int p2p_sendto(uint16_t src_vport, const uint8_t *data, int len, uint32_t addr, uint16_t port, uint16_t dst_vport);
/* sceNpSignaling: status of a connection (0 inactive, 1 pending, 2 active; -1 unknown id), and terminating one */
int p2p_conn_status(uint32_t conn_id, uint32_t *addr, uint16_t *port);
void p2p_terminate(uint32_t conn_id);
