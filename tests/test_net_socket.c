// SPDX-License-Identifier: BSD-3-Clause
#define _POSIX_C_SOURCE 200809L
#include "net_socket.h"
#include "test.h"

#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <time.h>
#endif

static bool await_readable(net_socket socket)
{
	for (unsigned attempt = 0; attempt < 1000; ++attempt) {
		if (net_socket_readable(socket))
			return true;
#ifdef _WIN32
		Sleep(1);
#else
		struct timespec pause = { .tv_nsec = 1000000 };
		nanosleep(&pause, NULL);
#endif
	}
	return false;
}

static bool test_owned_udp(void)
{
	net_socket sender = net_udp_open(), receiver = net_udp_open();
	CHECK(sender != NET_INVALID_SOCKET && receiver != NET_INVALID_SOCKET,
	      "UDP startup failed: %d", net_socket_error());
#ifndef _WIN32
	CHECK((fcntl(receiver, F_GETFL) & O_NONBLOCK) &&
	      (fcntl(receiver, F_GETFD) & FD_CLOEXEC), "socket flags differ");
#else
	DWORD flags = HANDLE_FLAG_INHERIT;
	CHECK(GetHandleInformation((HANDLE)receiver, &flags) &&
	      !(flags & HANDLE_FLAG_INHERIT), "socket is inheritable");
#endif
	struct sockaddr_in address = { .sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	net_socklen length = sizeof(address);
	CHECK(bind(receiver, (struct sockaddr *)&address, sizeof(address)) == 0 &&
	      getsockname(receiver, (struct sockaddr *)&address, &length) == 0 &&
	      address.sin_port != 0, "loopback bind failed");
	unsigned char data[257], received[16];
	for (size_t i = 0; i < sizeof(data); ++i)
		data[i] = (unsigned char)i;
	struct sockaddr_in source;
	bool truncated = true;
	CHECK(!net_socket_readable(receiver) &&
	      net_socket_receive(receiver, received, sizeof(received), &source, &truncated) < 0 &&
	      !truncated && net_socket_would_block(net_socket_error()),
	      "empty receive blocked or changed its error domain");
	CHECK(net_socket_send(sender, data, sizeof(received), &address) == sizeof(received) &&
	      await_readable(receiver) &&
	      net_socket_receive(receiver, received, sizeof(received), &source, &truncated) == sizeof(received) &&
	      !truncated && source.sin_addr.s_addr == htonl(INADDR_LOOPBACK) &&
	      memcmp(data, received, sizeof(received)) == 0, "datagram round trip failed");
	CHECK(net_socket_send(sender, data, sizeof(data), &address) == sizeof(data) &&
	      await_readable(receiver) &&
	      net_socket_receive(receiver, received, sizeof(received), &source, &truncated) >= (ptrdiff_t)sizeof(received) &&
	      truncated && !net_socket_readable(receiver), "oversized datagram was not consumed and marked");
	CHECK(net_socket_send(sender, data, 1, &address) == 1 &&
	      await_readable(receiver) &&
	      net_socket_receive(receiver, received, sizeof(received), NULL, &truncated) == 1 &&
	      !truncated && received[0] == data[0], "source-free drain failed");
	CHECK(!net_socket_readable(NET_INVALID_SOCKET) &&
	      net_socket_send(NET_INVALID_SOCKET, data, 1, &address) < 0 &&
	      net_socket_error() != 0, "invalid socket accepted");
	net_socket_close(sender);
	net_socket_close(receiver);
	net_socket_close(NET_INVALID_SOCKET);
	return true;
}

int main(void)
{
	for (unsigned pass = 0; pass < 20; ++pass)
		if (!test_owned_udp())
			return 1;
	return 0;
}
