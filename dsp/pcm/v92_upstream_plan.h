// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_UPSTREAM_PLAN_H
#define V92_UPSTREAM_PLAN_H

#include "v92_pcm_frame.h"
#include "v92_pcm_phase4_trn2u_line_rx.h"

#include <math.h>

/* Local receive coordinates, never a CPd wire field. Zero initialization
 * retains the trained equalizer/filter-output path. A channel-compensated
 * mapping instead yields delayed raw u(n), multiplied by gain here. Delay
 * is relative to the peer's first B1u input, not our delayed E2u decision. */
struct v92_upstream_plan {
	double gain;
	uint16_t delay;
	enum v92_pcm_frame_observation observation;
};

static inline bool v92_upstream_plan_is_valid(const struct v92_upstream_plan *plan)
{
	return plan != NULL &&
		((plan->observation == V92_PCM_FRAME_FILTER_OUTPUT && plan->gain == 0.0 && plan->delay == 0u) ||
		 (plan->observation == V92_PCM_FRAME_CONSTELLATION && isfinite(plan->gain) &&
		  plan->gain >= 1.0 / 32768.0 && plan->gain <= 32768.0 &&
		  plan->delay >= V92_PCM_PHASE4_TRN2U_LINE_DECISION_DELAY && plan->delay < V92_B1U_SYMBOLS));
}

#endif /* V92_UPSTREAM_PLAN_H */
