// SPDX-License-Identifier: BSD-3-Clause
#ifndef DS0_DATAPUMP_H
#define DS0_DATAPUMP_H
#include "modem_dp.h"
#include "ds0_probe.h"
const struct dp_operations *ds0_datapump_operations(enum DP_ID);
/* Copy the qualifying codewords; ownership is transferred through V.8. */
struct dp *ds0_datapump_create_handoff(struct modem *, int caller, const struct ds0_probe *);
#endif
