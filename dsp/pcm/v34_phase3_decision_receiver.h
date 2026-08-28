// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE3_DECISION_RECEIVER_H
#define V34_PHASE3_DECISION_RECEIVER_H

#include "v34_adaptive_equalizer.h"
#include "v34_phase3_j_receiver.h"
#include "v34_phase3_pp_sequence.h"
#include "v34_phase3_s_receiver.h"
#include "v34_station.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	/* 127 * 35 ms at the largest 24000/7-symbol/s rate. */
	V34_PHASE3_DECISION_RECEIVER_MAX_MD_SYMBOLS = 15240,
};

enum v34_phase3_decision_receiver_stage {
	V34_PHASE3_DECISION_RECEIVER_S,
	V34_PHASE3_DECISION_RECEIVER_MD,
	V34_PHASE3_DECISION_RECEIVER_REPEATED_S,
	V34_PHASE3_DECISION_RECEIVER_PP,
	V34_PHASE3_DECISION_RECEIVER_J,
	V34_PHASE3_DECISION_RECEIVER_COMPLETE,
};

enum v34_phase3_decision_receiver_result {
	V34_PHASE3_DECISION_RECEIVER_OK,
	V34_PHASE3_DECISION_RECEIVER_S_DETECTED,
	V34_PHASE3_DECISION_RECEIVER_MD_COMPLETE,
	V34_PHASE3_DECISION_RECEIVER_REPEATED_S_DETECTED,
	V34_PHASE3_DECISION_RECEIVER_PP_COMPLETE,
	V34_PHASE3_DECISION_RECEIVER_J_DETECTED,
	V34_PHASE3_DECISION_RECEIVER_ALREADY_COMPLETE,
	V34_PHASE3_DECISION_RECEIVER_INVALID_ARGUMENT,
	V34_PHASE3_DECISION_RECEIVER_INVALID_STATE,
	V34_PHASE3_DECISION_RECEIVER_OUT_OF_RANGE,
};

/* Every consumed observation has one explicit physical-owner operation. */
enum v34_phase3_decision_action {
	V34_PHASE3_DECISION_COMMIT,
	V34_PHASE3_DECISION_DISCARD,
	/* Discard the completed S-bar decision, then start full tracking. */
	V34_PHASE3_DECISION_BEGIN_TRACKING,
	/* Discard the final MD decision, then restart before repeated S. */
	V34_PHASE3_DECISION_RESTART_RECEIVE_PATH,
};

struct v34_phase3_decision_output {
	struct v34_adaptive_equalizer_decision equalizer;
	enum v34_phase3_decision_action action;
};

/* Own only Phase 3 semantic state. The physical sample clock and adaptive
 * equalizer remain in the aggregate receive owner so they can cross Phase 4,
 * B1, and data without a handoff reset. */
struct v34_phase3_decision_receiver {
	struct v34_phase3_s_receiver s;
	struct v34_phase3_pp_sequence pp;
	struct v34_phase3_j_receiver j;
	struct v34_phase3_s_detection s_detection;
	uint32_t md_symbols;
	uint32_t md_symbols_remaining;
	enum v34_station_role transmitting_role;
	enum v34_phase3_decision_receiver_stage stage;
};

struct v34_phase3_decision_handoff {
	struct v34_phase3_s_detection s;
	struct v34_phase3_j_detection j;
};

enum v34_phase3_decision_receiver_result
v34_phase3_decision_receiver_init(
	struct v34_phase3_decision_receiver *receiver,
	enum v34_station_role transmitting_role, uint32_t md_symbols);

/* Start from a bounded suffix when timing acquisition completes during S. */
enum v34_phase3_decision_receiver_result
v34_phase3_decision_receiver_init_suffix(
	struct v34_phase3_decision_receiver *receiver,
	enum v34_station_role transmitting_role, uint32_t md_symbols,
	uint8_t s_symbols);

bool v34_phase3_decision_receiver_is_valid(
	const struct v34_phase3_decision_receiver *receiver);
bool v34_phase3_decision_receiver_is_complete(
	const struct v34_phase3_decision_receiver *receiver);
bool v34_phase3_decision_handoff_is_valid(
	const struct v34_phase3_decision_handoff *handoff);

/* Consume exactly one pending adaptive-equalizer observation. The aggregate
 * supplies its current training power so the source-defined reduction after
 * one-half and one symbol-second remains continuous through a long TRN. The
 * returned action tells the aggregate whether to commit the target, discard an
 * unknown interval, begin PP tracking, or restart after MD. Rejected and
 * completed calls leave owner and output unchanged. */
enum v34_phase3_decision_receiver_result
v34_phase3_decision_receiver_observe(
	struct v34_phase3_decision_receiver *receiver,
	const struct v34_adaptive_equalizer_observation *observation,
	uint16_t decoder_power_q15,
	struct v34_phase3_decision_output *output);

enum v34_phase3_decision_receiver_result
v34_phase3_decision_receiver_handoff(
	const struct v34_phase3_decision_receiver *receiver,
	struct v34_phase3_decision_handoff *handoff);

#endif /* V34_PHASE3_DECISION_RECEIVER_H */
