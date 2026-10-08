// SPDX-License-Identifier: BSD-3-Clause
#ifndef MEDIA_WORKERS_H
#define MEDIA_WORKERS_H

#include "host.h"

struct modem_line;
struct media_workers;

struct media_job {
	struct modem_line *line;
	uint64_t now_ns;
	size_t datagrams;
	enum modem_host_result result;
};

/* One owner submits bounded batches. Lines must be distinct and exclusively
 * owned until run returns. No job or borrowed pointer survives that return.
 * threads includes the submitting owner; one uses no additional threads. */
struct media_workers *media_workers_create(unsigned int threads);
void media_workers_destroy(struct media_workers *workers);
bool media_workers_run(struct media_workers *workers, struct media_job *jobs,
	size_t count);

#endif
