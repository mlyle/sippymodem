// SPDX-License-Identifier: BSD-3-Clause

#ifndef V23_DATAPUMP_H
#define V23_DATAPUMP_H

#include <modem_defs.h>

struct dp_operations;

#include <stdbool.h>

enum {
	V23_FORWARD_BIT_RATE = 1200,
	V23_BACKWARD_BIT_RATE = 75,
};

struct dp_operations *v23_datapump_operations(enum DP_ID id);

#endif /* V23_DATAPUMP_H */
