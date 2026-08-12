// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE4_B1_HANDOFF_H
#define V34_PHASE4_B1_HANDOFF_H

#include "v34_caller_training.h"
#include "v34_data_stream.h"

/* Continue the calling station's live line into B1 only after its local E
 * transmission completes. The peer receive path is intentionally independent
 * and may still be in Phase 4. Rejected calls leave every input unchanged. */
enum v34_data_stream_result
v34_phase4_caller_b1_stream_init(
	struct v34_data_stream *stream,
	const struct v34_caller_training *training,
	const struct v34_data_stream_configuration *configuration);

#endif
