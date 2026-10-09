// SPDX-License-Identifier: BSD-3-Clause
#ifndef SIPPYMODEM_FD_H
#define SIPPYMODEM_FD_H

#include "sippymodem.h"

#ifdef __cplusplus
extern "C" {
#endif

/* POSIX adapter. Native Windows create returns NULL/ENOSYS; use byte I/O there. */
struct sippymodem_fd;

/* Create a native socket adapter bound to one call generation. peer_fd is a
 * nonblocking, close-on-exec byte-stream socket for pppd (or another native
 * terminal peer). Ownership of that fd passes to the caller on success. The
 * adapter borrows line; destroy it before the line. Pass the peer fd explicitly
 * through spawn/pass_fds, or clear its close-on-exec flag before exec of pppd.
 * No process signal handlers are changed. Steady-state bytes stay in native code. */
struct sippymodem_fd *sippymodem_fd_create(struct sippymodem_line *line,
	uint64_t generation, int *peer_fd);
/* One nonblocking pass copies at most 4096 bytes each way. The embedding owner
 * calls this before/after sippymodem_process. AGAIN means blocked/no progress,
 * CLOSED means peer/carrier lost. Retained RX is delivered before carrier-loss
 * EOF; a terminal result closes the owned endpoint so the peer sees EOF.
 * STALE never affects a newer call. Bytes still in the peer socket do not
 * contribute to the modem local DTE queue counts. */
enum sippymodem_result sippymodem_fd_process(struct sippymodem_fd *adapter);
/* Borrowed socket for poll readiness; receive/transmit buffers remain owned. */
int sippymodem_fd_poll_fd(const struct sippymodem_fd *adapter);
void sippymodem_fd_destroy(struct sippymodem_fd *adapter);

#ifdef __cplusplus
}
#endif
#endif
