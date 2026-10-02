// SPDX-License-Identifier: BSD-3-Clause

#ifndef BAUDOT_DATAPUMP_H
#define BAUDOT_DATAPUMP_H

#include <modem_defs.h>

struct dp_operations;

#include <stdbool.h>

struct dp_operations *baudot_datapump_operations(enum DP_ID id);

#endif /* BAUDOT_DATAPUMP_H */
