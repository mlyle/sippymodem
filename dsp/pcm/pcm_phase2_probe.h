// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_PHASE2_PROBE_H
#define PCM_PHASE2_PROBE_H

#include "pcm_phase2_ranging.h"
#include "phase2_initial_probe.h"
#include "phase2_reciprocal_probe.h"

/* A single PCM Phase 2 probe timeline. Only the active controller and the
 * shared INFO0 transport may consume samples. The completed controllers
 * supply the next stage's role and timing checks. */
struct pcm_phase2_probe {
	struct pcm_phase2_ranging ranging;
	struct phase2_initial_probe initial;
	struct phase2_probe_turnaround turnaround;
	struct phase2_reciprocal_probe reciprocal;
};

static inline bool pcm_phase2_probe_ranging_complete(
	const struct pcm_phase2_probe *probe)
{
	return pcm_phase2_ranging_is_valid(&probe->ranging) &&
	       probe->ranging.control.phase == PHASE2_RANGING_PROBE_READY;
}

#endif
