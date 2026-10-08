// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_startup.h"

#include <stddef.h>

bool pcm_startup_selection_is_valid(
	const struct pcm_startup_selection *selection)
{
	if (selection == NULL ||
	    (selection->protocol != PCM_PROTOCOL_V90 &&
	     selection->protocol != PCM_PROTOCOL_V92) ||
	    (selection->phase2 != PCM_PHASE2_FULL &&
	     selection->phase2 != PCM_PHASE2_SHORT))
		return false;
	return selection->protocol == PCM_PROTOCOL_V92 ||
	       selection->phase2 == PCM_PHASE2_FULL;
}

enum pcm_startup_result pcm_startup_select(
	const struct v92_info0a *analog,
	const struct v92_info0d *digital,
	struct pcm_startup_selection *selection)
{
	struct pcm_startup_selection selected;

	if (analog == NULL || digital == NULL || selection == NULL)
		return PCM_STARTUP_INVALID_ARGUMENT;
	if (!v92_info0a_is_valid(analog) || !v92_info0d_is_valid(digital))
		return PCM_STARTUP_INVALID_CAPABILITIES;
	if (!analog->v92_capable || !digital->v92_capable) {
		selected = (struct pcm_startup_selection){
			.protocol = PCM_PROTOCOL_V90,
			.phase2 = PCM_PHASE2_FULL,
		};
	} else {
		selected = (struct pcm_startup_selection){
			.protocol = PCM_PROTOCOL_V92,
			.phase2 = analog->short_phase2_requested &&
					  digital->short_phase2_requested ?
					  PCM_PHASE2_SHORT : PCM_PHASE2_FULL,
		};
	}
	*selection = selected;
	return PCM_STARTUP_OK;
}
