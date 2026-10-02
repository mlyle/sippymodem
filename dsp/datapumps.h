// SPDX-License-Identifier: BSD-3-Clause

#ifndef DATAPUMPS_H
#define DATAPUMPS_H

#include <modem_defs.h>

struct dp_operations;
struct dp_operations *call_progress_datapump_operations(enum DP_ID id);

#endif
