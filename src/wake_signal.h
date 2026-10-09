// SPDX-License-Identifier: BSD-3-Clause
#ifndef WAKE_SIGNAL_H
#define WAKE_SIGNAL_H

#include <stdbool.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define WAKE_SIGNAL_INITIALIZER { 0 }
#else
#define WAKE_SIGNAL_INITIALIZER { -1, -1 }
#endif

/* Counted, nonblocking notifications. Join all writers before closing.
 * At most 192 wakes are outstanding (128 events + 32 terminal + 32 workers).
 * Wait handles/descriptors are borrowed for observation, never consumption. */
struct wake_signal {
#ifdef _WIN32
	HANDLE event;
	SRWLOCK mutex;
	unsigned pending;
#else
	int read_fd, write_fd;
#endif
};
/* Initialize with WAKE_SIGNAL_INITIALIZER; open rejects an already open owner. */
bool wake_signal_open(struct wake_signal *signal);
bool wake_signal_is_open(const struct wake_signal *signal);
void wake_signal_close(struct wake_signal *signal);
bool wake_signal_send(struct wake_signal *signal);
bool wake_signal_take(struct wake_signal *signal);
int wake_signal_fd(const struct wake_signal *signal);
void *wake_signal_handle(const struct wake_signal *signal);

#endif
