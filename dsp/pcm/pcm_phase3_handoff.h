// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_PHASE3_HANDOFF_H
#define PCM_PHASE3_HANDOFF_H

#include "pcm_phase3_entry.h"

struct pcm_phase2;

/* Extract both role-dependent records from a ready Phase 2 owner and resolve
 * them against the active media law. Rejected calls leave entry unchanged. */
enum pcm_phase3_entry_result pcm_phase3_entry_from_phase2(
	struct pcm_phase3_entry *entry,
	const struct pcm_phase2 *phase2, enum g711_law media_law);

#endif /* PCM_PHASE3_HANDOFF_H */
