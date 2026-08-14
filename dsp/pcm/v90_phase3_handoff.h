// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE3_HANDOFF_H
#define V90_PHASE3_HANDOFF_H

#include "v90_phase3_entry.h"

struct pcm_phase2;

/* Extract both role-dependent records from a completed Phase 2 owner and
 * resolve them against the active media law. Rejected calls leave entry
 * unchanged. */
enum v90_phase3_entry_result v90_phase3_entry_from_phase2(
	struct v90_phase3_entry *entry,
	const struct pcm_phase2 *phase2, enum g711_law media_law);

#endif /* V90_PHASE3_HANDOFF_H */
