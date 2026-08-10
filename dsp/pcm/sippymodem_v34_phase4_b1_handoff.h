// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_PHASE4_B1_HANDOFF_H
#define SIPPYMODEM_V34_PHASE4_B1_HANDOFF_H

#include "sippymodem_v34_data_stream.h"
#include "sippymodem_v34_phase4_e_sequence.h"

/* Start the local B1 stream only after local E transmission completes. The
 * peer receive path is intentionally independent and may still be in Phase 4.
 * Rejected calls leave every input unchanged. */
enum sippymodem_v34_data_stream_result
sippymodem_v34_phase4_b1_transmit_stream_init(
	struct sippymodem_v34_data_stream *stream,
	const struct sippymodem_v34_phase4_e_sequence *transmitted_e,
	const struct sippymodem_v34_data_stream_configuration *configuration);

#endif
