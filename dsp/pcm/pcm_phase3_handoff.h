// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_PHASE3_HANDOFF_H
#define PCM_PHASE3_HANDOFF_H

#include "pcm_phase3_entry.h"

struct pcm_phase2;

/* Resolve the actual INFO0/INFO1 decision against the active media law.
 * Short startup requires explicit retained training policy; full startup
 * requires NULL because its received INFO1d already owns those settings.
 * Rejected calls leave entry unchanged. */
enum pcm_phase3_entry_result pcm_phase3_entry_from_phase2(
	struct pcm_phase3_entry *entry,
	const struct pcm_phase2 *phase2, enum g711_law media_law,
	const struct pcm_phase3_short_training *short_training);

#endif /* PCM_PHASE3_HANDOFF_H */
