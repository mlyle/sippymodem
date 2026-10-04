// SPDX-License-Identifier: BSD-3-Clause
#ifndef V91_DATAPUMP_H
#define V91_DATAPUMP_H
#include "modem_defs.h"
struct dp_operations;
const struct dp_operations *v91_datapump_operations(enum DP_ID id);
#endif
