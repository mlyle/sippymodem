// SPDX-License-Identifier: BSD-3-Clause
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sippymodem_fd.h"

#ifdef _WIN32
#include <errno.h>

/* Native pppd fd pumping is a POSIX adapter; Windows uses literal byte I/O. */
struct sippymodem_fd *sippymodem_fd_create(struct sippymodem_line *line,
    uint64_t generation, int *peer_fd)
{
    (void)line;
    (void)generation;
    if (peer_fd != NULL)
        *peer_fd = -1;
    errno = peer_fd != NULL ? ENOSYS : EINVAL;
    return NULL;
}
enum sippymodem_result sippymodem_fd_process(struct sippymodem_fd *adapter)
{
    (void)adapter;
    return SIPPYMODEM_INVALID;
}
int sippymodem_fd_poll_fd(const struct sippymodem_fd *adapter)
{
    (void)adapter;
    return -1;
}
void sippymodem_fd_destroy(struct sippymodem_fd *adapter)
{
    (void)adapter;
}
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <time.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/event.h>
#endif

enum { ADAPTER_CAPACITY = 4096 };
struct sippymodem_fd {
	struct sippymodem_line *line;
	uint64_t generation;
	int fd;
#ifdef __APPLE__
	int close_queue;
#endif
	unsigned char tx[ADAPTER_CAPACITY], rx[ADAPTER_CAPACITY];
	size_t tx_head, tx_count, rx_head, rx_count;
	bool closed;
};

static int make_pair(int sockets[2])
{
#if defined(__linux__) && !defined(FD_ADAPTER_PORTABLE_PAIR)
	return socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets);
#else
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
		return -1;
	for (unsigned i = 0; i < 2; ++i) {
		int flags = fcntl(sockets[i], F_GETFL);
		int descriptor_flags = fcntl(sockets[i], F_GETFD);
		if (flags < 0 || descriptor_flags < 0 ||
		    fcntl(sockets[i], F_SETFL, flags | O_NONBLOCK) < 0 ||
		    fcntl(sockets[i], F_SETFD, descriptor_flags | FD_CLOEXEC) < 0)
			goto fail;
#ifdef SO_NOSIGPIPE
		int one = 1;
		if (setsockopt(sockets[i], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) != 0)
			goto fail;
#endif
	}
	return 0;
fail: {
	int error = errno;
	close(sockets[0]);
	close(sockets[1]);
	errno = error;
	return -1;
}
#endif
}

static void close_adapter(struct sippymodem_fd *adapter)
{
	if (adapter->fd >= 0)
		close(adapter->fd);
	adapter->fd = -1;
#ifdef __APPLE__
	if (adapter->close_queue >= 0)
		close(adapter->close_queue);
	adapter->close_queue = -1;
#endif
}

struct sippymodem_fd *sippymodem_fd_create(struct sippymodem_line *line,
	uint64_t generation, int *peer_fd)
{
	if (peer_fd == NULL)
		return NULL;
	*peer_fd = -1;
	struct sippymodem_status status;
	if (sippymodem_get_status(line, &status) != SIPPYMODEM_OK ||
	    status.generation != generation || !status.ready || status.dte_mode != SIPPYMODEM_DATA) {
		errno = EINVAL;
		return NULL;
	}
	struct sippymodem_fd *adapter = calloc(1, sizeof(*adapter));
	if (adapter == NULL)
		return NULL;
	int sockets[2];
	if (make_pair(sockets) != 0) {
		free(adapter);
		return NULL;
	}
	adapter->fd = sockets[0];
#ifdef __APPLE__
	adapter->close_queue = kqueue();
	struct kevent change;
	EV_SET(&change, adapter->fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
	if (adapter->close_queue < 0 ||
	    fcntl(adapter->close_queue, F_SETFD, FD_CLOEXEC) < 0 ||
	    kevent(adapter->close_queue, &change, 1, NULL, 0, NULL) < 0) {
		int error = errno;
		close_adapter(adapter);
		close(sockets[1]);
		free(adapter);
		errno = error;
		return NULL;
	}
#endif
	adapter->line = line;
	adapter->generation = generation;
	*peer_fd = sockets[1];
	return adapter;
}

static enum sippymodem_result peer_closed(struct sippymodem_fd *adapter)
{
	adapter->closed = true;
	close_adapter(adapter);
	(void)sippymodem_hangup(adapter->line, adapter->generation);
	return SIPPYMODEM_CLOSED;
}

enum sippymodem_result sippymodem_fd_process(struct sippymodem_fd *adapter)
{
	if (adapter == NULL)
		return SIPPYMODEM_INVALID;
	struct sippymodem_status status;
	if (sippymodem_get_status(adapter->line, &status) != SIPPYMODEM_OK)
		return SIPPYMODEM_INVALID;
	if (status.generation != adapter->generation) {
		/* The adapter belongs to the retired call, never its replacement. */
		close_adapter(adapter);
		adapter->closed = true;
		return SIPPYMODEM_STALE;
	}
	if (adapter->closed)
		return SIPPYMODEM_CLOSED;
	bool progress = false;
	/* Observe write-side closure without consuming paused peer bytes. */
	if (status.ready && status.dte_mode == SIPPYMODEM_COMMAND) {
#ifdef __APPLE__
		struct kevent event;
		const struct timespec no_wait = { 0 };
		int polled = kevent(adapter->close_queue, NULL, 0, &event, 1, &no_wait);
		/* EV_EOF also reports half-close behind queued, paused payload. */
		if ((polled > 0 && (event.flags & (EV_EOF | EV_ERROR))) ||
		    (polled < 0 && errno != EINTR))
			return peer_closed(adapter);
#else
		struct pollfd peer = { .fd = adapter->fd, .events = POLLRDHUP };
		int polled = poll(&peer, 1, 0);
		if ((polled > 0 && (peer.revents & (POLLRDHUP | POLLHUP | POLLERR | POLLNVAL))) ||
		    (polled < 0 && errno != EINTR))
			return peer_closed(adapter);
#endif
	}
	/* Do not remove DTE bytes while the caller is explicitly in command mode. */
	if (status.ready && status.dte_mode == SIPPYMODEM_DATA) {
		if (adapter->tx_count == 0 && status.tx_free != 0) {
			size_t capacity = status.tx_free;
			if (capacity > sizeof(adapter->tx))
				capacity = sizeof(adapter->tx);
			ssize_t received = recv(adapter->fd, adapter->tx, capacity, MSG_DONTWAIT);
			if (received == 0)
				return peer_closed(adapter);
			if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
				return peer_closed(adapter);
			if (received > 0) {
				adapter->tx_head = 0;
				adapter->tx_count = (size_t)received;
				progress = true;
			}
		}
		if (adapter->tx_count != 0) {
			size_t accepted;
			enum sippymodem_result result = sippymodem_send(adapter->line,
				adapter->tx + adapter->tx_head, adapter->tx_count, &accepted);
			if (result != SIPPYMODEM_OK && result != SIPPYMODEM_AGAIN)
				return result;
			adapter->tx_head += accepted;
			adapter->tx_count -= accepted;
			progress |= accepted != 0;
		}
	}
	if (adapter->rx_count == 0) {
		size_t received;
		enum sippymodem_result result = sippymodem_receive(adapter->line,
			adapter->rx, sizeof(adapter->rx), &received);
		if (result != SIPPYMODEM_OK && result != SIPPYMODEM_AGAIN && result != SIPPYMODEM_CLOSED)
			return result;
		adapter->rx_head = 0;
		adapter->rx_count = received;
		progress |= received != 0;
		if (result == SIPPYMODEM_CLOSED && received == 0)
			return peer_closed(adapter);
	}
	if (adapter->rx_count != 0) {
		int flags = MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
		flags |= MSG_NOSIGNAL;
#endif
		ssize_t sent = send(adapter->fd, adapter->rx + adapter->rx_head,
			adapter->rx_count, flags);
		if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
			return peer_closed(adapter);
		if (sent > 0) {
			adapter->rx_head += (size_t)sent;
			adapter->rx_count -= (size_t)sent;
			progress = true;
		}
	}
	return progress ? SIPPYMODEM_OK : SIPPYMODEM_AGAIN;
}

int sippymodem_fd_poll_fd(const struct sippymodem_fd *adapter)
{
	return adapter != NULL && !adapter->closed ? adapter->fd : -1;
}

void sippymodem_fd_destroy(struct sippymodem_fd *adapter)
{
	if (adapter == NULL)
		return;
	close_adapter(adapter);
	free(adapter);
}

#endif
