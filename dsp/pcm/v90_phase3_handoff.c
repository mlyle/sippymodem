// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase3_handoff.h"

#include "v90_phase2.h"
#include "v90_phase2_info1_handoff.h"

#include <stddef.h>

enum v90_phase3_entry_result v90_phase3_entry_from_phase2(
	struct v90_phase3_entry *entry,
	const struct v90_phase2 *phase2, enum g711_law media_law)
{
	struct pcm_info1_selection selection;
	struct v90_info0d digital_info0;

	if (entry == NULL || !v90_phase2_is_valid(phase2) ||
	    (media_law != G711_LAW_PCMU && media_law != G711_LAW_PCMA))
		return V90_PHASE3_ENTRY_INVALID_ARGUMENT;
	if (!v90_phase2_is_ready_for_phase3(phase2))
		return V90_PHASE3_ENTRY_INVALID_STATE;
	if (v90_phase2_info1_handoff(phase2, &selection) != PCM_INFO1_OK)
		return V90_PHASE3_ENTRY_INTERNAL_ERROR;
	if (phase2->local_role == V90_DIGITAL_MODEM)
		digital_info0 = phase2->local_info0.digital;
	else if (v90_phase2_peer_info0d(phase2, &digital_info0) !=
		 V90_PHASE2_OK)
		return V90_PHASE3_ENTRY_INTERNAL_ERROR;
	return v90_phase3_entry_init(
		entry, phase2->local_role, media_law,
		&digital_info0, &selection);
}
