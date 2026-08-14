// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_phase2_info1_handoff.h"

#include <stddef.h>

enum pcm_info1_result pcm_phase2_info1_handoff(
	const struct pcm_phase2 *phase2,
	struct pcm_info1_selection *selection)
{
	struct pcm_info1_selection selected;

	if (phase2 == NULL || selection == NULL)
		return PCM_INFO1_INVALID_ARGUMENT;
	if (!pcm_phase2_is_ready_for_phase3(phase2))
		return PCM_INFO1_INVALID_RECORD;
	selected = (struct pcm_info1_selection){
		.startup = phase2->startup,
		.digital = phase2->state.info1.digital,
		.analog = phase2->state.info1.analog.record,
		.upstream = phase2->state.info1.analog.upstream,
	};
	if (!pcm_info1_selection_is_valid(&selected))
		return PCM_INFO1_INVALID_RECORD;
	*selection = selected;
	return PCM_INFO1_OK;
}
