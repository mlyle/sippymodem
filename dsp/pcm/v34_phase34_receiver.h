// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE34_RECEIVER_H
#define V34_PHASE34_RECEIVER_H

#include "v34_data_decision_receiver.h"
#include "v34_line_receiver.h"
#include "v34_phase3_decision_receiver.h"
#include "v34_phase3_profile.h"
#include "v34_phase4_decision_receiver.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_phase34_receiver_stage {
	V34_PHASE34_RECEIVER_PHASE3,
	V34_PHASE34_RECEIVER_PHASE4,
	V34_PHASE34_RECEIVER_DATA,
};

enum v34_phase34_receiver_result {
	V34_PHASE34_RECEIVER_OK,
	V34_PHASE34_RECEIVER_TIMING_ACQUIRED,
	V34_PHASE34_RECEIVER_PHASE3_S_DETECTED,
	V34_PHASE34_RECEIVER_MD_COMPLETE,
	V34_PHASE34_RECEIVER_REPEATED_S_DETECTED,
	V34_PHASE34_RECEIVER_PP_COMPLETE,
	V34_PHASE34_RECEIVER_J_DETECTED,
	V34_PHASE34_RECEIVER_PHASE4_S_DETECTED,
	V34_PHASE34_RECEIVER_TRN_COMPLETE,
	V34_PHASE34_RECEIVER_RECORD_READY,
	V34_PHASE34_RECEIVER_E_COMPLETE,
	V34_PHASE34_RECEIVER_INTERVAL_READY,
	V34_PHASE34_RECEIVER_MAPPING_FRAME_READY,
	V34_PHASE34_RECEIVER_B1_IN_PROGRESS,
	V34_PHASE34_RECEIVER_B1_COMPLETE,
	V34_PHASE34_RECEIVER_COMPLETE,
	V34_PHASE34_RECEIVER_FAILED,
	V34_PHASE34_RECEIVER_INVALID_ARGUMENT,
	V34_PHASE34_RECEIVER_INVALID_STATE,
	V34_PHASE34_RECEIVER_NUMERIC_FAILURE,
	V34_PHASE34_RECEIVER_OUT_OF_RANGE,
};

union v34_phase34_receiver_semantic_state {
	struct v34_phase3_decision_receiver phase3;
	struct v34_phase4_decision_receiver phase4;
	struct v34_data_decision_receiver data;
};

struct v34_phase34_receive_output {
	struct v34_mp_receive_event mp;
	struct v34_survivor_decision current;
	struct v34_received_mapping_frame received;
};

/* Own the V.34 semantic receive lineage from the peer's Phase 3 S through
 * mapped data. At local J the caller starts a distinct Phase 4 S semantic
 * window over the same line receiver; every later boundary preserves that
 * physical lineage too. */
struct v34_phase34_receiver {
	struct v34_line_receiver line;
	union v34_phase34_receiver_semantic_state semantic;
	struct v34_phase3_decision_handoff phase3_handoff;
	struct v34_q14_coefficient
		precoder_coefficients[V34_PRECODER_TAP_COUNT];
	struct v34_phase3_profile transmitting_profile;
	uint32_t tracking_symbols;
	enum v34_constellation_size phase4_constellation;
	enum v34_phase34_receiver_stage stage;
	bool tracking_started;
	bool phase4_s_rearmed;
	bool feedback_training_complete;
	bool data_erasure;
	bool failed;
	bool initialized;
};

enum v34_phase34_receiver_result v34_phase34_receiver_init(
	struct v34_phase34_receiver *receiver,
	const struct v34_phase3_profile *transmitting_profile,
	enum v34_constellation_size phase4_constellation,
	int16_t initial_receive_gain);
bool v34_phase34_receiver_is_valid(
	const struct v34_phase34_receiver *receiver);
bool v34_phase34_receiver_has_failed(
	const struct v34_phase34_receiver *receiver);
bool v34_phase34_receiver_is_complete(
	const struct v34_phase34_receiver *receiver);

/* Consume one signed 8-kHz line sample. A terminal semantic or numeric error
 * consumes that physical sample and latches failure. `output` changes only
 * for RECORD_READY or a completed data interval; rejected, paused, complete,
 * and failed calls consume nothing. */
enum v34_phase34_receiver_result v34_phase34_receiver_sample(
	struct v34_phase34_receiver *receiver, int16_t sample,
	struct v34_phase34_receive_output *output);

/* Begin the caller's distinct Phase 4 S/S-bar semantic window while retaining
 * the learned timing and equalizer. No received sample is consumed by this
 * transition. */
enum v34_phase34_receiver_result v34_phase34_receiver_rearm_phase4_s(
	struct v34_phase34_receiver *receiver);

/* Resolve the pending MP record without embedding negotiation policy in the
 * receive owner. */
enum v34_phase34_receiver_result v34_phase34_receiver_continue_mp(
	struct v34_phase34_receiver *receiver);
enum v34_phase34_receiver_result v34_phase34_receiver_expect_e(
	struct v34_phase34_receiver *receiver);

/* End decision-feedback training at the local MP boundary. When the record
 * permits precoding, select the learned coefficients only if the measured
 * feedback residual strictly improves on the ordinary decision error. A
 * rejected or disallowed precoder returns zero coefficients while still
 * freezing the learned filter and clearing its B1 history. */
enum v34_phase34_receiver_result
v34_phase34_receiver_finish_feedback_training(
	struct v34_phase34_receiver *receiver, bool allow_precoder,
	struct v34_q14_coefficient
		coefficients[V34_PRECODER_TAP_COUNT]);

/* Replace the completed Phase 4 semantic owner with a fresh B1/data decoder
 * while retaining the exact timing and adaptive-equalizer sample boundary. */
enum v34_phase34_receiver_result v34_phase34_receiver_start_data(
	struct v34_phase34_receiver *receiver,
	const struct v34_power_normalizer *normalizer,
	const struct v34_frame_geometry *geometry, bool expanded_shaping,
	const struct v34_q14_coefficient
		coefficients[V34_PRECODER_TAP_COUNT],
	enum v34_trellis_selection selection);

/* Freeze or resume the trained data-mode sample clock at an online quality
 * boundary.  No line sample is consumed. */
enum v34_phase34_receiver_result
v34_phase34_receiver_set_data_timing_frozen(
	struct v34_phase34_receiver *receiver, bool frozen);

/* Suppress semantic data decisions across a known line erasure while keeping
 * the physical timing, carrier, and equalizer input clocks continuous. */
enum v34_phase34_receiver_result
v34_phase34_receiver_set_data_erasure(
	struct v34_phase34_receiver *receiver, bool erasure);

#endif /* V34_PHASE34_RECEIVER_H */
