// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE4_DECISION_RECEIVER_H
#define V34_PHASE4_DECISION_RECEIVER_H

#include "v34_adaptive_equalizer.h"
#include "v34_phase3_j_sequence.h"
#include "v34_phase3_s_receiver.h"
#include "v34_phase4_e_receiver.h"
#include "v34_phase4_j_prime_sequence.h"
#include "v34_phase4_trn.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	/* J and J-prime differ by only two dibits.  Confirm the following
	 * zero-seeded TRN before making known-sequence equalizer decisions. */
	V34_PHASE4_TRN_CONFIRM_SYMBOLS = 16,
};

enum v34_phase4_decision_receiver_stage {
	V34_PHASE4_DECISION_RECEIVER_CALL_J_PRIME,
	V34_PHASE4_DECISION_RECEIVER_CALL_TRN_CANDIDATE,
	V34_PHASE4_DECISION_RECEIVER_ANSWER_S,
	V34_PHASE4_DECISION_RECEIVER_TRN,
	V34_PHASE4_DECISION_RECEIVER_MP,
	V34_PHASE4_DECISION_RECEIVER_E,
	V34_PHASE4_DECISION_RECEIVER_COMPLETE,
};

struct v34_phase4_j_prime_receiver {
	struct v34_scrambler descrambler;
	uint16_t decoded_history;
	uint8_t previous_clockwise_quarter_turns;
	uint8_t qualified_history;
	uint8_t received_symbols;
};

enum v34_phase4_decision_receiver_result {
	V34_PHASE4_DECISION_RECEIVER_OK,
	V34_PHASE4_DECISION_RECEIVER_S_DETECTED,
	V34_PHASE4_DECISION_RECEIVER_TRN_COMPLETE,
	V34_PHASE4_DECISION_RECEIVER_RECORD_READY,
	V34_PHASE4_DECISION_RECEIVER_E_COMPLETE,
	V34_PHASE4_DECISION_RECEIVER_ALREADY_COMPLETE,
	V34_PHASE4_DECISION_RECEIVER_INVALID_ARGUMENT,
	V34_PHASE4_DECISION_RECEIVER_INVALID_STATE,
	V34_PHASE4_DECISION_RECEIVER_SEQUENCE_MISMATCH,
};

enum v34_phase4_decision_action {
	V34_PHASE4_DECISION_COMMIT,
	/* The answering peer's S/S-bar follows an unowned Phase 3 drain. */
	V34_PHASE4_DECISION_DISCARD,
	/* S-bar completed the separately reacquired Phase 4 startup signal. */
	V34_PHASE4_DECISION_BEGIN_TRACKING,
};

struct v34_phase4_decision_output {
	struct v34_adaptive_equalizer_decision equalizer;
	enum v34_phase4_decision_action action;
};

/* Own only the Phase 4 S/TRN/MP/E semantics. Both roles retain at least 512
 * known TRN decisions before the continuous MP decoder takes ownership; the
 * answering peer first supplies its S/S-bar boundary. The physical timing and
 * one adaptive equalizer remain in the aggregate receive owner through B1 and
 * data. A complete MP event pauses this owner until the controller decides
 * whether to continue MP or start E. */
struct v34_phase4_decision_receiver {
	struct v34_phase3_s_receiver answer_s;
	struct v34_phase4_j_prime_receiver call_j_prime;
	struct v34_phase4_trn expected_trn;
	struct v34_phase4_mp_receiver mp_receiver;
	struct v34_phase4_e_receiver e_receiver;
	uint16_t trn_symbols;
	uint8_t trn_candidate_errors;
	enum v34_phase4_decision_receiver_stage stage;
	bool record_pending;
	bool pending_record_is_valid;
};

enum v34_phase4_decision_receiver_result
v34_phase4_decision_receiver_init(
	struct v34_phase4_decision_receiver *receiver,
	const struct v34_phase3_j_sequence *j_sequence,
	enum v34_constellation_size constellation);

bool v34_phase4_decision_receiver_is_valid(
	const struct v34_phase4_decision_receiver *receiver);
bool v34_phase4_decision_receiver_is_complete(
	const struct v34_phase4_decision_receiver *receiver);

enum v34_phase4_decision_receiver_result
v34_phase4_decision_receiver_continue_mp(
	struct v34_phase4_decision_receiver *receiver);
enum v34_phase4_decision_receiver_result
v34_phase4_decision_receiver_start_e(
	struct v34_phase4_decision_receiver *receiver);
enum v34_phase4_decision_receiver_result
v34_phase4_decision_receiver_expect_e(
	struct v34_phase4_decision_receiver *receiver);

/* Consume one pending adaptive-equalizer observation.  The aggregate owns the
 * current physical decoder power and supplies it explicitly; Phase 4 owns the
 * target and full-span adaptation decision.  `event` changes only when a
 * complete MP record is returned. */
enum v34_phase4_decision_receiver_result
v34_phase4_decision_receiver_observe(
	struct v34_phase4_decision_receiver *receiver,
	const struct v34_adaptive_equalizer_observation *observation,
	uint16_t decoder_power_q15,
	struct v34_phase4_decision_output *output,
	struct v34_mp_receive_event *event);

#endif /* V34_PHASE4_DECISION_RECEIVER_H */
