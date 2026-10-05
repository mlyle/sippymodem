// SPDX-License-Identifier: BSD-3-Clause
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sippymodem_fd.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

enum { ADAPTER_CAPACITY = 4096 };
struct sippymodem_fd {
	struct sippymodem_line *line;
	uint64_t generation;
	int fd;
	unsigned char tx[ADAPTER_CAPACITY], rx[ADAPTER_CAPACITY];
	size_t tx_head, tx_count, rx_head, rx_count;
	bool closed;
};

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
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets) != 0) {
		free(adapter);
		return NULL;
	}
	adapter->line = line;
	adapter->generation = generation;
	adapter->fd = sockets[0];
	*peer_fd = sockets[1];
	return adapter;
}

static enum sippymodem_result peer_closed(struct sippymodem_fd *adapter)
{
	adapter->closed = true;
	if (adapter->fd >= 0) {
		close(adapter->fd);
		adapter->fd = -1;
	}
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
		if (adapter->fd >= 0) {
			close(adapter->fd);
			adapter->fd = -1;
		}
		adapter->closed = true;
		return SIPPYMODEM_STALE;
	}
	if (adapter->closed)
		return SIPPYMODEM_CLOSED;
	bool progress = false;
	/* Observe write-side closure without consuming paused peer bytes. */
	if (status.ready && status.dte_mode == SIPPYMODEM_COMMAND) {
		struct pollfd peer = { .fd = adapter->fd, .events = POLLRDHUP };
		int polled = poll(&peer, 1, 0);
		if ((polled > 0 && (peer.revents & (POLLRDHUP | POLLHUP | POLLERR | POLLNVAL))) ||
		    (polled < 0 && errno != EINTR))
			return peer_closed(adapter);
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
		ssize_t sent = send(adapter->fd, adapter->rx + adapter->rx_head,
			adapter->rx_count, MSG_DONTWAIT | MSG_NOSIGNAL);
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
	if (adapter->fd >= 0)
		close(adapter->fd);
	free(adapter);
}
