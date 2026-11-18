// SPDX-License-Identifier: BSD-3-Clause

#ifndef PHASE2_PROBE_H
#define PHASE2_PROBE_H

#include "phase2_info0_exchange.h"
#include "v34_phase2_probe_acquisition.h"
#include "v34_phase2_probe_sequence.h"

/* Both probe turns use the same DSP path and failure contract. Their owners
 * retain the role-specific sequencing, acknowledgement, and recovery rules. */
enum phase2_probe_failure {
	PHASE2_PROBE_NO_FAILURE,
	PHASE2_PROBE_REMOTE_NOT_FOUND,
	PHASE2_PROBE_LOCAL_TRANSMIT_LIMIT,
};

union phase2_probe_path {
	struct v34_phase2_probe_acquisition receive;
	struct v34_phase2_probe_sequence transmit;
};

/* The probe owner chooses when to listen. Acknowledgment can end the
 * sequence only after the first L2 sample, including after the wire limit. */
static inline bool phase2_probe_transmit_sample(
	struct v34_phase2_probe_sequence *sequence,
	struct phase2_info0_exchange *exchange, bool listen,
	int16_t input, int16_t *output, struct v34_phase2_tone_event *tone,
	struct v34_phase2_probe_sequence_event *event)
{
	if (listen && !phase2_info0_exchange_observe_tone(exchange, input, tone))
		return false;
	bool finish = tone->kind == V34_PHASE2_TONE_STARTED &&
		sequence->samples_emitted > V34_PHASE2_PROBE_L1_SAMPLES;
	return v34_phase2_probe_sequence_sample(sequence, finish, output, event) ==
	       V34_PHASE2_PROBE_SEQUENCE_OK;
}

#endif
