// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_phase3_handoff.h"

#include "pcm_phase2.h"
#include "pcm_phase2_info1_handoff.h"

#include <stddef.h>

enum pcm_phase3_entry_result pcm_phase3_entry_from_phase2(
	struct pcm_phase3_entry *entry,
	const struct pcm_phase2 *phase2, enum g711_law media_law,
	const struct pcm_phase3_short_training *short_training)
{
	struct pcm_info1_selection selection;
	struct v90_info0d digital_info0;

	if (entry == NULL || !pcm_phase2_is_valid(phase2) ||
	    !g711_law_is_valid(media_law))
		return PCM_PHASE3_ENTRY_INVALID_ARGUMENT;
	if (!pcm_phase2_is_ready_for_phase3(phase2))
		return PCM_PHASE3_ENTRY_INVALID_STATE;
	if (pcm_phase2_info1_handoff(phase2, &selection) != PCM_INFO1_OK)
		return PCM_PHASE3_ENTRY_INTERNAL_ERROR;
	if ((selection.startup.phase2 == PCM_PHASE2_SHORT) !=
	    (short_training != NULL))
		return PCM_PHASE3_ENTRY_INVALID_ARGUMENT;
	if (phase2->local_role == V90_DIGITAL_MODEM)
		digital_info0 = phase2->info0_protocol == PCM_PROTOCOL_V90 ?
				phase2->local_info0.digital :
				phase2->local_info0.v92_digital.v90;
	else if (pcm_phase2_peer_info0d(phase2, &digital_info0) !=
		 PCM_PHASE2_OK)
		return PCM_PHASE3_ENTRY_INTERNAL_ERROR;
	if (selection.startup.phase2 == PCM_PHASE2_SHORT)
		return pcm_phase3_entry_init_short(entry, phase2->local_role,
			media_law, &digital_info0, &selection, short_training);
	return pcm_phase3_entry_init(
		entry, phase2->local_role, media_law,
		&digital_info0, &selection);
}
