// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase4_b1_handoff.h"

#include <stddef.h>

enum v34_data_stream_result
v34_phase4_b1_transmit_stream_init(
	struct v34_data_stream *stream,
	const struct v34_phase4_e_sequence *transmitted_e,
	const struct v34_data_stream_configuration *configuration)
{
	if (stream == NULL || configuration == NULL ||
	    !v34_phase4_e_sequence_is_complete(transmitted_e) ||
	    configuration->profile.local_role != transmitted_e->role)
		return V34_DATA_STREAM_INVALID_ARGUMENT;
	return v34_data_stream_init(stream, configuration);
}
