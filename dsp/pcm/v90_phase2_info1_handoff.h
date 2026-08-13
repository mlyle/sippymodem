// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE2_INFO1_HANDOFF_H
#define V90_PHASE2_INFO1_HANDOFF_H

#include "pcm_info1.h"
#include "v90_phase2.h"

/* Extract either endpoint's completed INFO1 decision. Rejected calls leave
 * selection unchanged. */
enum pcm_info1_result v90_phase2_info1_handoff(
	const struct v90_phase2 *phase2,
	struct pcm_info1_selection *selection);

#endif /* V90_PHASE2_INFO1_HANDOFF_H */
