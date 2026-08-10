// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v34_phase4_b1_handoff.h"

#include <stddef.h>

enum sippymodem_v34_data_stream_result
sippymodem_v34_phase4_b1_transmit_stream_init(
	struct sippymodem_v34_data_stream *stream,
	const struct sippymodem_v34_phase4_e_sequence *transmitted_e,
	const struct sippymodem_v34_data_stream_configuration *configuration)
{
	if (stream == NULL || configuration == NULL ||
	    !sippymodem_v34_phase4_e_sequence_is_complete(transmitted_e) ||
	    configuration->profile.local_role != transmitted_e->role)
		return SIPPYMODEM_V34_DATA_STREAM_INVALID_ARGUMENT;
	return sippymodem_v34_data_stream_init(stream, configuration);
}
