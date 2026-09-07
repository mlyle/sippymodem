// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_DATAPUMP_H
#define V34_DATAPUMP_H

#include "v34_profile.h"

#include <stdbool.h>

struct dp;
struct pcm_call;
struct training_session;

int v34_datapump_register(void);
int v34_datapump_unregister(void);
bool v34_datapump_is_busy(void);

/* Copy a pending fallback retrain before the modem deletes this owner. */
bool v34_datapump_pcm_retrain_handoff(
	struct dp *dp, struct pcm_call *call, struct training_session *training);

#endif /* V34_DATAPUMP_H */
