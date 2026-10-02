// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_DATAPUMP_H
#define V34_DATAPUMP_H

#include <modem_defs.h>

struct dp_operations;

#include "v34_profile.h"

#include <stdbool.h>

struct dp;
struct pcm_call;
struct training_session;

const struct dp_operations *v34_datapump_operations(enum DP_ID id);

/* Copy a pending fallback retrain before the modem deletes this owner. */
bool v34_datapump_pcm_retrain_handoff(
	struct dp *dp, struct pcm_call *call, struct training_session *training);

#endif /* V34_DATAPUMP_H */
