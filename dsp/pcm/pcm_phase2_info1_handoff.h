// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_PHASE2_INFO1_HANDOFF_H
#define PCM_PHASE2_INFO1_HANDOFF_H

#include "pcm_info1.h"
#include "pcm_phase2.h"

/* Extract either endpoint's selected INFO1 decision at its role-safe Phase 3
 * boundary. Rejected calls leave selection unchanged. */
enum pcm_info1_result pcm_phase2_info1_handoff(
	const struct pcm_phase2 *phase2,
	struct pcm_info1_selection *selection);

#endif /* PCM_PHASE2_INFO1_HANDOFF_H */
