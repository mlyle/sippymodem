// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase4_b1_handoff.h"

enum v34_data_stream_result
v34_phase4_caller_b1_stream_init(
	struct v34_data_stream *stream,
	const struct v34_caller_training *training,
	const struct v34_data_stream_configuration *configuration)
{
	if (!v34_caller_training_phase4_e_complete(training))
		return V34_DATA_STREAM_INVALID_ARGUMENT;
	return v34_data_stream_continue_b1(
		stream, &training->line, configuration);
}
