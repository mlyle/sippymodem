// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_phase3_profile.h"

#include <stddef.h>

static const struct v34_info1c *digital_v34_info1(
	const struct pcm_info1_selection *selection)
{
	return selection->startup.protocol == PCM_PROTOCOL_V90 ?
		       &selection->digital.record.v90 :
		       &selection->digital.record.v92.v34;
}

enum v34_phase3_profile_result pcm_phase3_conventional_profile(
	const struct pcm_info1_selection *selection,
	struct v34_phase3_profile *profile)
{
	const struct v34_info1c *digital;
	const struct v90_info1a *analog;
	enum v34_symbol_rate symbol_rate;

	if (profile == NULL || !pcm_info1_selection_is_valid(selection) ||
	    selection->upstream != PCM_UPSTREAM_V34)
		return V34_PHASE3_PROFILE_INVALID_ARGUMENT;
	digital = digital_v34_info1(selection);
	analog = &selection->analog.conventional;
	symbol_rate = analog->upstream_symbol_rate;
	return v34_phase3_profile_init(
		profile, V34_ANSWERING_STATION, symbol_rate,
		&digital->answer_transmit[symbol_rate],
		digital->common.minimum_peer_power_reduction_db,
		analog->local_md_length_35_ms_units);
}
