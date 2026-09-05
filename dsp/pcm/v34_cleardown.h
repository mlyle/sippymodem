// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_CLEARDOWN_H
#define V34_CLEARDOWN_H

#include "v34_line_receiver.h"
#include "v34_phase3_s_receiver.h"
#include "v34_phase4_mp.h"
#include "v34_phase4_mp_receiver.h"
#include "v34_training_line.h"

enum v34_cleardown_result {
	V34_CLEARDOWN_OK,
	V34_CLEARDOWN_COMPLETE,
	V34_CLEARDOWN_INVALID_ARGUMENT,
	V34_CLEARDOWN_INVALID_STATE,
	V34_CLEARDOWN_NUMERIC_FAILURE,
};

enum v34_cleardown_stage {
	V34_CLEARDOWN_S,
	V34_CLEARDOWN_MP,
	V34_CLEARDOWN_TAIL,
	V34_CLEARDOWN_FINISHED,
};

/* V.34 11.7 initiation over the already-trained line. Data ownership ends at
 * entry, but carrier, pulse history, timing, and equalizer remain continuous.
 * A local zero-rate MP is acknowledged only after a valid peer MP; completion
 * requires reciprocal MP-prime and the final transmit filter tail. The modem
 * lifecycle owns the deadline for a peer that never acknowledges. */
struct v34_cleardown {
	struct v34_training_line transmit_line;
	struct v34_line_receiver receive_line;
	struct v34_phase3_s_sequence transmit_s;
	struct v34_phase3_s_receiver receive_s;
	struct v34_phase4_mp transmit_mp;
	struct v34_phase4_mp_receiver receive_mp;
	struct v34_mp_parameters parameters;
	struct v34_mp_parameters peer_parameters;
	uint16_t tail_samples;
	enum v34_cleardown_stage stage;
	bool peer_mp_received;
	bool mp_prime_sent;
	bool failed;
};

enum v34_cleardown_result v34_cleardown_init(
	struct v34_cleardown *cleardown,
	const struct v34_training_line *transmit_line,
	const struct v34_line_receiver *receive_line,
	const struct v34_mp_parameters *parameters);
bool v34_cleardown_is_valid(const struct v34_cleardown *cleardown);

/* Consume and emit one simultaneous 8-kHz sample. Rejected calls consume
 * nothing. COMPLETE emits silence after every final MP-prime sample drained. */
enum v34_cleardown_result v34_cleardown_sample(
	struct v34_cleardown *cleardown, int16_t received,
	int16_t *transmitted);

#endif /* V34_CLEARDOWN_H */
