// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE4_B1_HANDOFF_H
#define V34_PHASE4_B1_HANDOFF_H

#include "v34_data_stream.h"
#include "v34_phase4_e_sequence.h"

/* Start the local B1 stream only after local E transmission completes. The
 * peer receive path is intentionally independent and may still be in Phase 4.
 * Rejected calls leave every input unchanged. */
enum v34_data_stream_result
v34_phase4_b1_transmit_stream_init(
	struct v34_data_stream *stream,
	const struct v34_phase4_e_sequence *transmitted_e,
	const struct v34_data_stream_configuration *configuration);

#endif
