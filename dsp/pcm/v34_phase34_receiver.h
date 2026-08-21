// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE34_RECEIVER_H
#define V34_PHASE34_RECEIVER_H

#include "v34_adaptive_equalizer.h"
#include "v34_phase3_decision_receiver.h"
#include "v34_phase3_profile.h"
#include "v34_phase4_decision_receiver.h"
#include "v34_receive_timing.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_phase34_receiver_stage {
	V34_PHASE34_RECEIVER_PHASE3,
	V34_PHASE34_RECEIVER_PHASE4,
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
};

/* Own one continuous fixed-point receive lineage from the peer's Phase 3 S
 * through Phase 4 E. Timing and adaptive equalization are never handed off or
 * restarted at J; only the semantic owner changes. */
struct v34_phase34_receiver {
	struct v34_receive_timing timing;
	struct v34_adaptive_equalizer equalizer;
	union v34_phase34_receiver_semantic_state semantic;
	struct v34_phase3_decision_handoff phase3_handoff;
	struct v34_phase3_profile transmitting_profile;
	uint32_t tracking_symbols;
	uint16_t decoder_power_q15;
	enum v34_constellation_size phase4_constellation;
	enum v34_phase34_receiver_stage stage;
	bool signal_started;
	bool tracking_started;
	bool failed;
	bool initialized;
};

enum v34_phase34_receiver_result v34_phase34_receiver_init(
	struct v34_phase34_receiver *receiver,
	const struct v34_phase3_profile *transmitting_profile,
	enum v34_constellation_size phase4_constellation);
bool v34_phase34_receiver_is_valid(
	const struct v34_phase34_receiver *receiver);
bool v34_phase34_receiver_has_failed(
	const struct v34_phase34_receiver *receiver);
bool v34_phase34_receiver_is_complete(
	const struct v34_phase34_receiver *receiver);

/* Consume one signed 8-kHz line sample. A terminal semantic or numeric error
 * consumes that physical sample and latches failure. `event` changes only for
 * RECORD_READY; rejected, paused, complete, and failed calls consume nothing. */
enum v34_phase34_receiver_result v34_phase34_receiver_sample(
	struct v34_phase34_receiver *receiver, int16_t sample,
	struct v34_mp_receive_event *event);

/* Resolve the pending MP record without embedding negotiation policy in the
 * receive owner. */
enum v34_phase34_receiver_result v34_phase34_receiver_continue_mp(
	struct v34_phase34_receiver *receiver);
enum v34_phase34_receiver_result v34_phase34_receiver_start_e(
	struct v34_phase34_receiver *receiver);
enum v34_phase34_receiver_result v34_phase34_receiver_expect_e(
	struct v34_phase34_receiver *receiver);

/* Copy the detected Phase 3 boundary after the receiver has entered Phase 4. */
enum v34_phase34_receiver_result v34_phase34_receiver_phase3_handoff(
	const struct v34_phase34_receiver *receiver,
	struct v34_phase3_decision_handoff *handoff);

#endif /* V34_PHASE34_RECEIVER_H */
