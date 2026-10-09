// SPDX-License-Identifier: BSD-3-Clause
#include "net_socket.h"

#include <errno.h>
#include <limits.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

int net_socket_error(void)
{
#ifdef _WIN32
	return WSAGetLastError();
#else
	return errno;
#endif
}

bool net_socket_interrupted(int error)
{
#ifdef _WIN32
	return error == WSAEINTR;
#else
	return error == EINTR;
#endif
}

bool net_socket_would_block(int error)
{
#ifdef _WIN32
	return error == WSAEWOULDBLOCK;
#else
	return error == EAGAIN || error == EWOULDBLOCK;
#endif
}

net_socket net_udp_open(void)
{
#ifdef _WIN32
	WSADATA data;
	int error = WSAStartup(MAKEWORD(2, 2), &data);
	if (error != 0) {
		WSASetLastError(error);
		return NET_INVALID_SOCKET;
	}
	if (data.wVersion != MAKEWORD(2, 2)) {
		WSACleanup();
		WSASetLastError(WSAVERNOTSUPPORTED);
		return NET_INVALID_SOCKET;
	}
	net_socket socket_fd = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP,
		NULL, 0, WSA_FLAG_NO_HANDLE_INHERIT);
	if (socket_fd == NET_INVALID_SOCKET)
		goto fail;
	u_long nonblocking = 1;
	if (ioctlsocket(socket_fd, FIONBIO, &nonblocking) != 0)
		goto fail;
	return socket_fd;
fail:
	error = WSAGetLastError();
	if (socket_fd != NET_INVALID_SOCKET)
		closesocket(socket_fd);
	WSACleanup();
	WSASetLastError(error);
	return NET_INVALID_SOCKET;
#else
	int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (socket_fd < 0)
		return NET_INVALID_SOCKET;
	int flags = fcntl(socket_fd, F_GETFL);
	int descriptor_flags = fcntl(socket_fd, F_GETFD);
	if (flags >= 0 && descriptor_flags >= 0 &&
	    fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) == 0 &&
	    fcntl(socket_fd, F_SETFD, descriptor_flags | FD_CLOEXEC) == 0)
		return socket_fd;
	int error = errno;
	close(socket_fd);
	errno = error;
	return NET_INVALID_SOCKET;
#endif
}

void net_socket_close(net_socket socket_fd)
{
	if (socket_fd == NET_INVALID_SOCKET)
		return;
#ifdef _WIN32
	closesocket(socket_fd);
	WSACleanup();
#else
	close(socket_fd);
#endif
}

bool net_socket_readable(net_socket socket_fd)
{
	if (socket_fd == NET_INVALID_SOCKET)
		return false;
	struct pollfd descriptor = { .fd = socket_fd, .events = POLLIN };
#ifdef _WIN32
	return WSAPoll(&descriptor, 1, 0) > 0;
#else
	return poll(&descriptor, 1, 0) > 0;
#endif
}

ptrdiff_t net_socket_receive(net_socket socket_fd, void *data, size_t capacity,
	struct sockaddr_in *source, bool *truncated)
{
	*truncated = false;
#ifdef _WIN32
	if (capacity > INT_MAX) {
		WSASetLastError(WSAEMSGSIZE);
		return -1;
	}
	int length = sizeof(*source);
	int result = source == NULL ? recv(socket_fd, data, (int)capacity, 0) :
		recvfrom(socket_fd, data, (int)capacity, 0, (struct sockaddr *)source, &length);
	if (result == SOCKET_ERROR && WSAGetLastError() == WSAEMSGSIZE) {
		/* The datagram was consumed; preserve oversized-packet accounting. */
		*truncated = true;
		return (ptrdiff_t)capacity;
	}
	return result;
#else
	if (source == NULL)
		return recv(socket_fd, data, capacity, 0);
	struct iovec vector = { .iov_base = data, .iov_len = capacity };
	struct msghdr message = {
		.msg_name = source, .msg_namelen = sizeof(*source),
		.msg_iov = &vector, .msg_iovlen = 1,
	};
	/* Nonblocking mode belongs to the owner; MSG_TRUNC is an output flag. */
	ssize_t result = recvmsg(socket_fd, &message, 0);
	*truncated = result >= 0 && ((message.msg_flags & MSG_TRUNC) || (size_t)result > capacity);
	return result;
#endif
}

ptrdiff_t net_socket_send(net_socket socket_fd, const void *data, size_t size,
	const struct sockaddr_in *destination)
{
#ifdef _WIN32
	if (size > INT_MAX) {
		WSASetLastError(WSAEMSGSIZE);
		return -1;
	}
	return sendto(socket_fd, data, (int)size, 0,
		(const struct sockaddr *)destination, sizeof(*destination));
#else
	return sendto(socket_fd, data, size, 0,
		(const struct sockaddr *)destination, sizeof(*destination));
#endif
}
