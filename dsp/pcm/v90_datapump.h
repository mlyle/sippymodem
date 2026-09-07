// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_DATAPUMP_H
#define V90_DATAPUMP_H

#include "training_session.h"
#include "pcm_call.h"

#include <stdbool.h>

struct dp;

struct v90_v34_fallback_handoff {
	struct v34_phase2_handoff phase2;
	struct v90_fallback_restart restart;
	struct training_session training;
};

int v90_datapump_register(void);
int v90_datapump_unregister(void);
bool v90_datapump_is_busy(void);

/* Copy a completed V.90-to-V.34 fallback while the modem still owns the old
 * datapump. The ordinary datapump switch creates the V.34 owner before
 * deleting this one, so no process-global transfer slot is needed. */
bool v90_datapump_v34_fallback_handoff(
	struct dp *dp, struct v90_v34_fallback_handoff *handoff);

#endif /* V90_DATAPUMP_H */
