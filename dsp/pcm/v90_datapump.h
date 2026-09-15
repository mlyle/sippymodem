// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_DATAPUMP_H
#define V90_DATAPUMP_H

#include "training_session.h"
#include "pcm_call.h"

#include <stdbool.h>

struct dp;

struct pcm_v34_fallback_handoff {
	struct v34_phase2_handoff phase2;
	struct pcm_fallback_restart restart;
	struct training_session training;
};

int v90_datapump_register(void);
int v90_datapump_unregister(void);
bool v90_datapump_is_busy(void);

/* Request an online PCM-upstream rate exchange without changing
 * terminal command mode. Ordinary media processing owns Ru/Rd and reconnects
 * the retained link. request_silence requests the intervening timed pause
 * for this exchange only; it does not change the call's profile preference.
 * Full retraining (including ATO1) remains separate.
 * Unsupported protocols, non-online calls and duplicates return false
 * without changing the call or its training session. Serialize with media. */
bool v90_datapump_start_rate_renegotiation(struct dp *dp, bool request_silence);

/* The same retained host-link contract, using V.92 RM/Rf fast exchange and
 * the preceding data modulation instead of Ru/Rd and fresh training. */
bool v90_datapump_start_fast_exchange(struct dp *dp);

/* Copy a completed PCM-to-V.34 fallback while the modem still owns the old
 * datapump. The ordinary datapump switch creates the V.34 owner before
 * deleting this one, so no process-global transfer slot is needed. */
bool v90_datapump_v34_fallback_handoff(
	struct dp *dp, struct pcm_v34_fallback_handoff *handoff);

/* QC signal timeout restarts ordinary V.8 once, retaining the deadline. */
bool v90_datapump_v8_fallback_handoff(
	struct dp *dp, struct training_session *training);

#endif /* V90_DATAPUMP_H */
