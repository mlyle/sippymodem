// SPDX-License-Identifier: BSD-3-Clause
#ifndef MEDIA_WORKERS_H
#define MEDIA_WORKERS_H

#include "host.h"

struct modem_line;
struct media_worker;
struct media_job {
	uint64_t now_ns;
	size_t datagrams;
	enum modem_host_result result;
	bool notified;
};

/* Each started line owns a persistent thread and one copied job. Collect it
 * before accessing the line. No payload or caller job pointers are retained.
 * wake_fd is borrowed until destroy; -1 disables completion notification. */
struct media_worker *media_worker_create(struct modem_line *line, int wake_fd);
bool media_worker_busy(const struct media_worker *worker);
bool media_worker_submit(struct media_worker *worker, uint64_t now_ns);
bool media_worker_collect(struct media_worker *worker, bool wait, struct media_job *job);
/* Lifecycle only: finish outstanding work and join before releasing line. */
void media_worker_destroy(struct media_worker *worker);
#endif
