// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase2_info1_handoff.h"

#include <stddef.h>

static const struct v34_info1c *digital_info1(
	const struct v90_phase2 *phase2)
{
	return &phase2->state.info1.digital.record.v90;
}

static const struct v90_info1a *analog_info1(
	const struct v90_phase2 *phase2)
{
	return &phase2->state.info1.analog.record.conventional;
}

enum pcm_info1_result v90_phase2_info1_handoff(
	const struct v90_phase2 *phase2,
	struct pcm_info1_selection *selection)
{
	struct pcm_info1_selection selected = {
		.startup = {
			.protocol = PCM_PROTOCOL_V90,
			.phase2 = PCM_PHASE2_FULL,
		},
		.upstream = PCM_UPSTREAM_V34,
	};

	if (phase2 == NULL || selection == NULL)
		return PCM_INFO1_INVALID_ARGUMENT;
	if (!v90_phase2_is_ready_for_phase3(phase2))
		return PCM_INFO1_INVALID_RECORD;
	if (pcm_info1d_init_v90(
		    &selected.digital, digital_info1(phase2)) != PCM_INFO1_OK)
		return PCM_INFO1_INVALID_RECORD;
	selected.analog.conventional = *analog_info1(phase2);
	if (!pcm_info1_selection_is_valid(&selected))
		return PCM_INFO1_INVALID_RECORD;
	*selection = selected;
	return PCM_INFO1_OK;
}
