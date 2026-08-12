// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_TRAINING_RECEIVER_H
#define V34_TRAINING_RECEIVER_H

#include "v34_phase4_line_handoff.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_training_receiver_stage {
	V34_TRAINING_RECEIVER_PHASE3,
	V34_TRAINING_RECEIVER_PHASE4,
};

enum v34_training_receiver_result {
	V34_TRAINING_RECEIVER_OK,
	V34_TRAINING_RECEIVER_TIMING_ACQUIRED,
	V34_TRAINING_RECEIVER_S_DETECTED,
	V34_TRAINING_RECEIVER_MD_COMPLETE,
	V34_TRAINING_RECEIVER_REPEATED_S_DETECTED,
	V34_TRAINING_RECEIVER_PP_COMPLETE,
	/* J was detected and the instance now owns a Phase 4 receiver. */
	V34_TRAINING_RECEIVER_J_DETECTED,
	V34_TRAINING_RECEIVER_SYMBOL,
	V34_TRAINING_RECEIVER_RECORD_READY,
	V34_TRAINING_RECEIVER_E_COMPLETE,
	V34_TRAINING_RECEIVER_COMPLETE,
	V34_TRAINING_RECEIVER_FAILED,
	V34_TRAINING_RECEIVER_INVALID_ARGUMENT,
	V34_TRAINING_RECEIVER_INVALID_STATE,
	V34_TRAINING_RECEIVER_NUMERIC_FAILURE,
	V34_TRAINING_RECEIVER_OUT_OF_RANGE,
	V34_TRAINING_RECEIVER_SEQUENCE_MISMATCH,
};

struct v34_training_receiver_configuration {
	struct v34_phase3_line_receiver_configuration phase3;
	double phase4_equalizer_adaptation;
};

union v34_training_receiver_state {
	struct v34_phase3_line_receiver phase3;
	struct v34_phase4_line_receiver phase4;
};

/* Own the peer's continuous Phase 3 and Phase 4 sample path. Mutable state is
 * confined to one instance; sample and control calls on the same instance
 * must be serialized by its caller. MP acceptance policy remains external. */
struct v34_training_receiver {
	union v34_training_receiver_state state;
	struct v34_training_receiver_configuration configuration;
	enum v34_training_receiver_stage stage;
	bool failed;
};

bool v34_training_receiver_configuration_is_valid(
	const struct v34_training_receiver_configuration *configuration);
bool v34_training_receiver_is_valid(
	const struct v34_training_receiver *receiver);
bool v34_training_receiver_has_failed(
	const struct v34_training_receiver *receiver);
bool v34_training_receiver_is_complete(
	const struct v34_training_receiver *receiver);

enum v34_training_receiver_result v34_training_receiver_init(
	struct v34_training_receiver *receiver,
	const struct v34_phase3_profile *transmitting_profile,
	uint32_t sample_rate,
	const struct v34_training_receiver_configuration *configuration);

/* Consume one line sample. `event` changes only for RECORD_READY. J detection
 * commits the Phase 3-to-Phase 4 handoff at that exact sample boundary. */
enum v34_training_receiver_result v34_training_receiver_sample(
	struct v34_training_receiver *receiver, int16_t sample,
	struct v34_mp_receive_event *event);

/* Resolve the pending Phase 4 record without embedding negotiation policy in
 * the DSP owner. Calls in Phase 3 or without a pending record are rejected. */
enum v34_training_receiver_result v34_training_receiver_continue_mp(
	struct v34_training_receiver *receiver);
enum v34_training_receiver_result v34_training_receiver_start_e(
	struct v34_training_receiver *receiver);

/* Copy the completed E-to-B1 boundary into the steady-state data receiver. */
enum v34_training_receiver_result v34_training_receiver_start_data(
	const struct v34_training_receiver *receiver,
	struct v34_data_line_receiver *data_receiver,
	const struct v34_data_line_receiver_configuration *configuration);

const char *v34_training_receiver_result_name(
	enum v34_training_receiver_result result);

#endif /* V34_TRAINING_RECEIVER_H */
