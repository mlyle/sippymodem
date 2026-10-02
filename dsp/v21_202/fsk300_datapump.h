// SPDX-License-Identifier: BSD-3-Clause

#ifndef FSK300_DATAPUMP_H
#define FSK300_DATAPUMP_H

#include <modem_defs.h>

struct dp_operations;

#include <stdbool.h>

enum {
	FSK300_BIT_RATE = 300,
};

struct dp_operations *fsk300_datapump_operations(enum DP_ID id);

#endif /* FSK300_DATAPUMP_H */
