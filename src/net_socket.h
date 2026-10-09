// SPDX-License-Identifier: BSD-3-Clause
#ifndef NET_SOCKET_H
#define NET_SOCKET_H

#include <stdbool.h>
#include <stddef.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET net_socket;
typedef int net_socklen;
#define NET_INVALID_SOCKET INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
typedef int net_socket;
typedef socklen_t net_socklen;
#define NET_INVALID_SOCKET (-1)
#endif

/* Each successful open owns a nonblocking, noninheritable UDP socket and,
 * on Windows, one Winsock startup reference. Close releases both. */
net_socket net_udp_open(void);
void net_socket_close(net_socket socket);
int net_socket_error(void);
bool net_socket_interrupted(int error);
bool net_socket_would_block(int error);
bool net_socket_readable(net_socket socket);
ptrdiff_t net_socket_receive(net_socket socket, void *data, size_t capacity,
	struct sockaddr_in *source, bool *truncated);
ptrdiff_t net_socket_send(net_socket socket, const void *data, size_t size,
	const struct sockaddr_in *destination);

#endif
