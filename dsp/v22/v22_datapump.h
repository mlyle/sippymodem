// SPDX-License-Identifier: BSD-3-Clause

#ifndef V22_DATAPUMP_H
#define V22_DATAPUMP_H

#include <modem_defs.h>

struct dp_operations;
struct dp;

#include <stdbool.h>

enum {
	V22_600_BIT_RATE = 600,
	V22_1200_BIT_RATE = 1200,
	V22BIS_BIT_RATE = 2400,
};

const struct dp_operations *v22_datapump_operations(enum DP_ID id);
bool v22_datapump_has_peer(struct dp *dp);
/* The bounded reply wait starts after our S1, excluding the answer-mark guard. */
bool v22_datapump_can_retry_600(struct dp *dp);

#endif /* V22_DATAPUMP_H */
