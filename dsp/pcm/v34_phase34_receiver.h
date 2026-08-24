// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE34_RECEIVER_H
#define V34_PHASE34_RECEIVER_H

#include "v34_adaptive_equalizer.h"
#include "v34_data_decision_receiver.h"
#include "v34_phase3_decision_receiver.h"
#include "v34_phase3_profile.h"
#include "v34_phase4_decision_receiver.h"
#include "v34_receive_timing.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_phase34_receiver_stage {
	V34_PHASE34_RECEIVER_PHASE3,
	V34_PHASE34_RECEIVER_PHASE4,
	V34_PHASE34_RECEIVER_DATA,
};

enum {
	V34_PHASE34_SIGNAL_ACQUISITION_SAMPLES = 32,
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
	V34_PHASE34_RECEIVER_SEQUENCE_MISMATCH,
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

/* Own one continuous fixed-point receive lineage from the peer's Phase 3 S
 * through mapped data. Timing and adaptive equalization are never handed off
 * or restarted at J or E; only the semantic owner changes. */
struct v34_phase34_receiver {
	struct v34_receive_timing timing;
	struct v34_adaptive_equalizer equalizer;
	union v34_phase34_receiver_semantic_state semantic;
	struct v34_phase3_decision_handoff phase3_handoff;
	struct v34_q14_coefficient
		precoder_coefficients[V34_PRECODER_TAP_COUNT];
	int16_t signal_acquisition_samples[
		V34_PHASE34_SIGNAL_ACQUISITION_SAMPLES];
	struct v34_phase3_profile transmitting_profile;
	uint64_t signal_acquisition_energy;
	uint32_t tracking_symbols;
	uint16_t decoder_power_q15;
	enum v34_constellation_size phase4_constellation;
	enum v34_phase34_receiver_stage stage;
	uint8_t signal_acquisition_count;
	uint8_t signal_acquisition_write;
	bool signal_started;
	bool tracking_started;
	bool feedback_training_complete;
	bool precoder_selected;
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

/* Resolve the pending MP record without embedding negotiation policy in the
 * receive owner. */
enum v34_phase34_receiver_result v34_phase34_receiver_continue_mp(
	struct v34_phase34_receiver *receiver);
enum v34_phase34_receiver_result v34_phase34_receiver_start_e(
	struct v34_phase34_receiver *receiver);
enum v34_phase34_receiver_result v34_phase34_receiver_expect_e(
	struct v34_phase34_receiver *receiver);

/* End decision-feedback training at the local MP boundary. A Type 1 MP owns
 * the returned learned coefficients; Type 0 selects a zero-coefficient
 * receive path while still freezing the learned filter and clearing its B1
 * history. */
enum v34_phase34_receiver_result
v34_phase34_receiver_finish_feedback_training(
	struct v34_phase34_receiver *receiver, bool select_precoder,
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

/* Copy the detected Phase 3 boundary after the receiver has left Phase 3. */
enum v34_phase34_receiver_result v34_phase34_receiver_phase3_handoff(
	const struct v34_phase34_receiver *receiver,
	struct v34_phase3_decision_handoff *handoff);

#endif /* V34_PHASE34_RECEIVER_H */
