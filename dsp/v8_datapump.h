// SPDX-License-Identifier: BSD-3-Clause

#ifndef V8_DATAPUMP_H
#define V8_DATAPUMP_H

#include "modem.h"
#include "training_session.h"
#include "v8_menu.h"
#include "v92_qc1.h"

#include <stdbool.h>

struct dp;
struct single_tone_detector;

/* Copied before the V.8 owner is deleted. QC transfers after the final
 * QC1/QCA1 sample; the fragment tail was already emitted as silence. */
struct v8_pcm_handoff {
	struct training_session training;
	struct v92_qc1_message local;
	struct v92_qc1_message peer;
	int16_t receive_tail[MODEM_FRAG - 1];
	unsigned int tail_count;
	bool quick_connect;
};

bool v8_datapump_pcm_handoff(const struct dp *dp, struct v8_pcm_handoff *handoff);
/* Transfer the independently owned trained legacy probe once. The caller
 * owns deletion after this succeeds; deleting V.8 releases unselected probes. */
struct dp *v8_datapump_take_legacy(struct dp *dp);
bool v8_datapump_answer_tone_complete(const struct dp *dp);

/* Transfer the qualified 2100-Hz answer to a caller, or the 1800-Hz
 * calling carrier to an answerer, before deleting the negotiation owner. */
bool v8_datapump_v32_handoff(const struct dp *dp, struct single_tone_detector *carrier);

/* Returns the classic V.8 mode carried with the requested datapump. */
static inline enum v8_modulation
v8_datapump_modulation(enum DP_ID id)
{
	switch (id) {
	case DP_V21:
		return V8_MODULATION_V21;
	case DP_V22:
	case DP_V22BIS:
		return V8_MODULATION_V22;
	case DP_V23:
		return V8_MODULATION_V23_DUPLEX;
	case DP_V32:
	case DP_V32BIS:
		return V8_MODULATION_V32;
	case DP_V34:
	case DP_V90:
	case DP_V91:
	case DP_V92:
		return V8_MODULATION_V34_DUPLEX;
	default:
		return V8_MODULATION_NONE;
	}
}

static inline bool v8_datapump_uses_pcm(enum DP_ID id)
{
	return id == DP_V90 || id == DP_V91 || id == DP_V92;
}

const struct dp_operations *v8_datapump_operations(enum DP_ID id);
/* True only while a completed PCM-mode V.8 exchange is handing this modem
 * to conventional V.34 because the joint menu selected no PCM roles. */
bool v8_datapump_selected_v34_fallback(const struct dp *dp);

#endif
