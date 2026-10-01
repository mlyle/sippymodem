// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE4_DECISION_RECEIVER_H
#define V34_PHASE4_DECISION_RECEIVER_H

#include "v34_adaptive_equalizer.h"
#include "v34_phase3_j_sequence.h"
#include "v34_phase3_s_receiver.h"
#include "v34_phase4_mp_receiver.h"
#include "v34_phase4_j_prime_sequence.h"
#include "v34_training_sequence.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	/* Confirm zero-seeded TRN after J-prime or the answerer's S marker
	 * before making known-sequence equalizer decisions. */
	V34_PHASE4_DECISION_RECEIVER_TRN_CONFIRM_SYMBOLS = 16,
};

enum v34_phase4_decision_receiver_stage {
	V34_PHASE4_DECISION_RECEIVER_CALL_J_PRIME,
	V34_PHASE4_DECISION_RECEIVER_CALL_TRN_CANDIDATE,
	V34_PHASE4_DECISION_RECEIVER_ANSWER_S,
	V34_PHASE4_DECISION_RECEIVER_TRN,
	V34_PHASE4_DECISION_RECEIVER_TRN_OR_MP,
	V34_PHASE4_DECISION_RECEIVER_MP,
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
};

/* Own only the Phase 4 S/TRN/MP/E semantics. Both roles retain at least 512
 * known TRN decisions. Four-point reception advances that known lineage until
 * the physical MP boundary diverges; sixteen-point reception lets the
 * continuous MP decoder self-synchronize. The answering peer first supplies
 * its S/S-bar boundary, confirmed by the peer's TRN scrambler before the
 * handoff. Qualify S recurrence before using it as equalizer feedback:
 * delayed Phase 3 J is not a training target. The physical timing
 * and one adaptive equalizer remain in the aggregate receive owner through
 * B1 and data. A complete MP event pauses this owner for the controller. After
 * a valid MP, the continuous decoder also accepts E: V.34 permits E to end the
 * exchange when the peer's MP-prime was missed. */
struct v34_phase4_decision_receiver {
	struct v34_phase3_s_hunt answer_s;
	struct v34_phase4_j_prime_receiver call_j_prime;
	struct v34_training_sequence expected_trn;
	struct v34_phase4_mp_receiver mp_receiver;
	/* Bounded complex correlation while S awaits peer TRN confirmation. */
	int64_t trn_correlation_in_phase;
	int64_t trn_correlation_quadrature;
	uint64_t trn_observed_energy;
	uint64_t trn_target_energy;
	uint16_t trn_symbols;
	uint8_t trn_candidate_errors;
	enum v34_phase4_decision_receiver_stage stage;
	bool record_pending;
	bool mp_received;
	bool rate_renegotiation;
};

enum v34_phase4_decision_receiver_result
v34_phase4_decision_receiver_init(
	struct v34_phase4_decision_receiver *receiver,
	const struct v34_phase3_j_sequence *j_sequence,
	enum v34_constellation_size constellation);

/* S/S-bar has already been consumed by the shared online detector. Optional
 * TRN and repeated MP self-synchronize the four-point control receiver. */
enum v34_phase4_decision_receiver_result
v34_phase4_decision_receiver_resume_online(
	struct v34_phase4_decision_receiver *receiver,
	enum v34_station_role transmitting_role);

bool v34_phase4_decision_receiver_is_valid(
	const struct v34_phase4_decision_receiver *receiver);
bool v34_phase4_decision_receiver_is_complete(
	const struct v34_phase4_decision_receiver *receiver);

enum v34_phase4_decision_receiver_result
v34_phase4_decision_receiver_continue_mp(
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
	struct v34_receive_action *output,
	struct v34_mp_receive_event *event);

#endif /* V34_PHASE4_DECISION_RECEIVER_H */
