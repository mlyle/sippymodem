// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase4_decision_receiver.h"

#include "memory_region.h"
#include "v34_constellation.h"

#include <limits.h>
#include <stddef.h>

enum {
	V34_PHASE4_TRN_CONFIRM_MAX_ERRORS = 1,
};

static bool s_is_initial(const struct v34_phase3_s_receiver *receiver)
{
	return receiver->next_decision == 0u &&
	       receiver->buffered_decisions == 0u &&
	       receiver->pending_symbols == 0u &&
	       receiver->post_marker_symbols == 0u &&
	       !receiver->detection_pending && !receiver->complete;
}

static bool trn_is_initial(const struct v34_phase4_trn *trn)
{
	return trn->scrambler.history == 0u &&
	       trn->last_clockwise_quarter_turns == 0u &&
	       !trn->has_last_symbol;
}

static bool j_prime_is_qualified(
	uint16_t decoded_history, uint8_t qualified_history)
{
	uint16_t difference =
		(uint16_t)(decoded_history ^ V34_PHASE4_J_PRIME_PATTERN);
	unsigned int mismatches = 0;
	unsigned int unqualified = 0;

	for (unsigned int symbol = 0;
	     symbol < V34_PHASE4_J_PRIME_SYMBOLS; ++symbol) {
		mismatches += (difference & 3u) != 0u;
		unqualified += (qualified_history & 1u) == 0u;
		difference >>= 2u;
		qualified_history >>= 1u;
	}
	return mismatches <= 1u && unqualified <= 1u;
}

static bool training_decision_is_qualified(
	const struct v34_adaptive_equalizer_observation *observation,
	struct v34_analytic_sample target)
{
	int32_t in_phase_error =
		(int32_t)observation->decision.in_phase - target.in_phase;
	int32_t quadrature_error =
		(int32_t)observation->decision.quadrature - target.quadrature;
	uint64_t error_energy =
		(uint64_t)((int64_t)in_phase_error * in_phase_error) +
		(uint64_t)((int64_t)quadrature_error * quadrature_error);
	uint64_t target_energy =
		(uint64_t)((int64_t)target.in_phase * target.in_phase) +
		(uint64_t)((int64_t)target.quadrature * target.quadrature);

	/* A one-shot boundary needs physical training evidence as well as the
	 * decoded bit pattern.  Permit one poor symbol, matching the one-dibit
	 * correlation tolerance, but do not acquire from carrier-free slices. */
	return error_energy * 2u <= target_energy;
}

static bool call_j_prime_state_is_valid(
	const struct v34_phase4_decision_receiver *receiver)
{
	const struct v34_phase4_j_prime_receiver *j_prime =
		&receiver->call_j_prime;
	bool calling_peer = receiver->mp_receiver.transmitting_role ==
		V34_CALLING_STATION;

	if (!calling_peer)
		return j_prime->descrambler.history == 0u &&
		       j_prime->descrambler.polynomial == V34_POLYNOMIAL_GPC &&
		       j_prime->decoded_history == 0u &&
		       j_prime->previous_clockwise_quarter_turns == 0u &&
		       j_prime->qualified_history == 0u &&
		       j_prime->received_symbols == 0u;
	if (!v34_transmit_scrambler_is_valid(
		    &j_prime->descrambler, V34_CALLING_STATION) ||
	    j_prime->previous_clockwise_quarter_turns > 3u ||
	    j_prime->received_symbols > V34_PHASE4_J_PRIME_SYMBOLS ||
	    (j_prime->received_symbols < V34_PHASE4_J_PRIME_SYMBOLS &&
	     ((j_prime->decoded_history >>
	       (2u * j_prime->received_symbols)) != 0u ||
	      (j_prime->qualified_history >> j_prime->received_symbols) !=
		      0u)))
		return false;
	if (receiver->stage != V34_PHASE4_DECISION_RECEIVER_CALL_J_PRIME &&
	    j_prime->received_symbols != V34_PHASE4_J_PRIME_SYMBOLS)
		return false;
	return true;
}

static bool answer_s_state_is_valid(
	const struct v34_phase4_decision_receiver *receiver)
{
	bool answering_peer = receiver->mp_receiver.transmitting_role ==
		V34_ANSWERING_STATION;

	if (!v34_phase3_s_receiver_is_valid(&receiver->answer_s))
		return false;
	if (!answering_peer)
		return receiver->stage != V34_PHASE4_DECISION_RECEIVER_ANSWER_S &&
		       s_is_initial(&receiver->answer_s);
	return v34_phase3_s_receiver_is_complete(&receiver->answer_s) ==
	       (receiver->stage != V34_PHASE4_DECISION_RECEIVER_ANSWER_S);
}

static bool trn_state_is_valid(
	const struct v34_phase4_decision_receiver *receiver)
{
	bool calling_peer = receiver->mp_receiver.transmitting_role ==
		V34_CALLING_STATION;

	if (!v34_phase4_trn_is_valid(&receiver->expected_trn) ||
	    receiver->expected_trn.role !=
		    receiver->mp_receiver.transmitting_role ||
	    receiver->expected_trn.constellation !=
		    receiver->mp_receiver.constellation ||
	    receiver->trn_symbols > V34_PHASE4_TRN_MIN_SYMBOLS ||
	    receiver->trn_candidate_errors >
		    V34_PHASE4_TRN_CONFIRM_MAX_ERRORS)
		return false;
	if (receiver->stage == V34_PHASE4_DECISION_RECEIVER_CALL_J_PRIME ||
	    receiver->stage == V34_PHASE4_DECISION_RECEIVER_ANSWER_S)
		return receiver->trn_symbols == 0u &&
		       receiver->trn_candidate_errors == 0u &&
		       trn_is_initial(&receiver->expected_trn);
	if (receiver->stage ==
	    V34_PHASE4_DECISION_RECEIVER_CALL_TRN_CANDIDATE) {
		if (!calling_peer ||
		    receiver->trn_candidate_errors > receiver->trn_symbols ||
		    receiver->trn_symbols >=
			    V34_PHASE4_TRN_CONFIRM_SYMBOLS)
			return false;
	} else if (receiver->trn_candidate_errors != 0u) {
		return false;
	}
	if (receiver->expected_trn.has_last_symbol !=
	    (receiver->trn_symbols != 0u))
		return false;
	if (receiver->stage == V34_PHASE4_DECISION_RECEIVER_TRN)
		return receiver->trn_symbols < V34_PHASE4_TRN_MIN_SYMBOLS &&
		       (!calling_peer ||
			receiver->trn_symbols >=
				V34_PHASE4_TRN_CONFIRM_SYMBOLS);
	return receiver->stage ==
		       V34_PHASE4_DECISION_RECEIVER_CALL_TRN_CANDIDATE ||
	       receiver->trn_symbols == V34_PHASE4_TRN_MIN_SYMBOLS;
}

bool v34_phase4_decision_receiver_is_valid(
	const struct v34_phase4_decision_receiver *receiver)
{
	if (receiver == NULL ||
	    receiver->stage < V34_PHASE4_DECISION_RECEIVER_CALL_J_PRIME ||
	    receiver->stage > V34_PHASE4_DECISION_RECEIVER_COMPLETE ||
	    !v34_phase4_mp_receiver_is_valid(&receiver->mp_receiver) ||
	    !v34_station_role_is_valid(
		    receiver->mp_receiver.transmitting_role) ||
	    !answer_s_state_is_valid(receiver) ||
	    !call_j_prime_state_is_valid(receiver) ||
	    !trn_state_is_valid(receiver) ||
	    (receiver->pending_record_is_valid && !receiver->record_pending) ||
	    (receiver->record_pending &&
	     receiver->stage != V34_PHASE4_DECISION_RECEIVER_MP))
		return false;
	if (receiver->stage < V34_PHASE4_DECISION_RECEIVER_E)
		return true;
	if (!v34_phase4_e_receiver_is_valid(&receiver->e_receiver) ||
	    receiver->e_receiver.transmitting_role !=
		    receiver->mp_receiver.transmitting_role ||
	    receiver->e_receiver.constellation !=
		    receiver->mp_receiver.constellation)
		return false;
	return v34_phase4_e_receiver_is_complete(&receiver->e_receiver) ==
	       (receiver->stage == V34_PHASE4_DECISION_RECEIVER_COMPLETE);
}

bool v34_phase4_decision_receiver_is_complete(
	const struct v34_phase4_decision_receiver *receiver)
{
	return v34_phase4_decision_receiver_is_valid(receiver) &&
	       receiver->stage == V34_PHASE4_DECISION_RECEIVER_COMPLETE;
}

enum v34_phase4_decision_receiver_result
v34_phase4_decision_receiver_init(
	struct v34_phase4_decision_receiver *receiver,
	const struct v34_phase3_j_sequence *j_sequence,
	enum v34_constellation_size constellation)
{
	enum v34_station_role transmitting_role =
		j_sequence != NULL ? j_sequence->role :
				      (enum v34_station_role)-1;
	struct v34_phase4_decision_receiver initialized = {
		.stage = transmitting_role == V34_ANSWERING_STATION ?
			 V34_PHASE4_DECISION_RECEIVER_ANSWER_S :
			 V34_PHASE4_DECISION_RECEIVER_CALL_J_PRIME,
	};

	if (receiver == NULL || j_sequence == NULL ||
	    memory_regions_overlap(
		    receiver, sizeof(*receiver), j_sequence,
		    sizeof(*j_sequence)) ||
	    !v34_phase3_j_sequence_at_pattern_boundary(j_sequence) ||
	    !v34_station_role_is_valid(transmitting_role) ||
	    !v34_constellation_size_is_valid(constellation) ||
	    v34_phase3_s_receiver_init_fixed_suffix(
		    &initialized.answer_s,
		    V34_PHASE3_S_RECEIVER_MIN_S_SUFFIX_SYMBOLS) !=
		    V34_PHASE3_S_RECEIVER_OK ||
	    v34_phase4_trn_init(
		    &initialized.expected_trn, transmitting_role,
		    constellation) != V34_PHASE4_TRN_OK ||
	    v34_phase4_mp_receiver_init(
		    &initialized.mp_receiver, transmitting_role,
		    constellation) != V34_PHASE4_MP_RECEIVER_OK)
		return V34_PHASE4_DECISION_RECEIVER_INVALID_ARGUMENT;
	if (transmitting_role == V34_CALLING_STATION) {
		initialized.call_j_prime.descrambler =
			j_sequence->scrambler;
		initialized.call_j_prime.previous_clockwise_quarter_turns =
			j_sequence->previous_clockwise_quarter_turns;
	}
	if (!v34_phase4_decision_receiver_is_valid(&initialized))
		return V34_PHASE4_DECISION_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return V34_PHASE4_DECISION_RECEIVER_OK;
}

static enum v34_phase4_decision_receiver_result clear_record(
	struct v34_phase4_decision_receiver *receiver, bool require_valid,
	bool search)
{
	struct v34_phase4_decision_receiver next;

	if (!v34_phase4_decision_receiver_is_valid(receiver))
		return V34_PHASE4_DECISION_RECEIVER_INVALID_STATE;
	if (receiver->stage != V34_PHASE4_DECISION_RECEIVER_MP ||
	    !receiver->record_pending ||
	    (require_valid && !receiver->pending_record_is_valid))
		return V34_PHASE4_DECISION_RECEIVER_INVALID_STATE;
	next = *receiver;
	if (require_valid) {
		enum v34_phase4_e_receiver_result result = search ?
			v34_phase4_e_receiver_search_from_mp(
				&next.e_receiver, &next.mp_receiver) :
			v34_phase4_e_receiver_init_from_mp(
				&next.e_receiver, &next.mp_receiver);

		if (result != V34_PHASE4_E_RECEIVER_OK)
			return V34_PHASE4_DECISION_RECEIVER_INVALID_STATE;
		next.stage = V34_PHASE4_DECISION_RECEIVER_E;
	}
	next.record_pending = false;
	next.pending_record_is_valid = false;
	if (!v34_phase4_decision_receiver_is_valid(&next))
		return V34_PHASE4_DECISION_RECEIVER_INVALID_STATE;
	*receiver = next;
	return V34_PHASE4_DECISION_RECEIVER_OK;
}

enum v34_phase4_decision_receiver_result
v34_phase4_decision_receiver_continue_mp(
	struct v34_phase4_decision_receiver *receiver)
{
	return clear_record(receiver, false, false);
}

enum v34_phase4_decision_receiver_result
v34_phase4_decision_receiver_start_e(
	struct v34_phase4_decision_receiver *receiver)
{
	return clear_record(receiver, true, false);
}

enum v34_phase4_decision_receiver_result
v34_phase4_decision_receiver_expect_e(
	struct v34_phase4_decision_receiver *receiver)
{
	return clear_record(receiver, true, true);
}

static struct v34_phase4_decision_output discard_output(void)
{
	return (struct v34_phase4_decision_output){
		.action = V34_PHASE4_DECISION_DISCARD,
	};
}

static struct v34_phase4_decision_output begin_tracking_output(void)
{
	return (struct v34_phase4_decision_output){
		.action = V34_PHASE4_DECISION_BEGIN_TRACKING,
	};
}

static struct v34_phase4_decision_output commit_output(
	const struct v34_adaptive_equalizer_observation *observation,
	struct v34_analytic_sample target, uint16_t decoder_power_q15)
{
	return (struct v34_phase4_decision_output){
		.equalizer = {
			.observation = *observation,
			.target = target,
			.decoder_power_q15 = decoder_power_q15,
			.adaptation = V34_ADAPTIVE_EQUALIZER_FULL,
		},
		.action = V34_PHASE4_DECISION_COMMIT,
	};
}

static bool advance_trn_prefix(
	struct v34_phase4_decision_receiver *receiver, uint8_t symbols)
{
	for (uint8_t symbol = 0; symbol < symbols; ++symbol) {
		struct v34_analytic_sample ignored;
		uint8_t ignored_quarter;
		uint8_t ignored_rotation;

		if (v34_phase4_trn_next_fixed(
			    &receiver->expected_trn, &ignored,
			    &ignored_quarter, &ignored_rotation) !=
		    V34_PHASE4_TRN_OK)
			return false;
		receiver->trn_symbols++;
	}
	return true;
}

static enum v34_phase4_decision_receiver_result receive_answer_s(
	struct v34_phase4_decision_receiver *receiver,
	const struct v34_adaptive_equalizer_observation *observation,
	const struct v34_fixed_training_decision *hard,
	struct v34_phase4_decision_output *output)
{
	enum v34_phase3_s_receiver_result result =
		v34_phase3_s_receiver_receive_fixed(
			&receiver->answer_s, hard->quarter_point,
			hard->clockwise_quarter_turns, observation->decision);

	*output = discard_output();
	if (result == V34_PHASE3_S_RECEIVER_OK)
		return V34_PHASE4_DECISION_RECEIVER_OK;
	if (result != V34_PHASE3_S_RECEIVER_DETECTED ||
	    !advance_trn_prefix(
		    receiver, receiver->answer_s.post_marker_symbols))
		return V34_PHASE4_DECISION_RECEIVER_INVALID_STATE;
	receiver->stage = V34_PHASE4_DECISION_RECEIVER_TRN;
	*output = begin_tracking_output();
	return V34_PHASE4_DECISION_RECEIVER_S_DETECTED;
}

static bool advance_call_j_prime(
	struct v34_phase4_decision_receiver *receiver,
	const struct v34_adaptive_equalizer_observation *observation,
	const struct v34_fixed_training_decision *hard,
	bool *detected)
{
	struct v34_phase4_j_prime_receiver *j_prime =
		&receiver->call_j_prime;
	uint8_t decoded;

	if (v34_descramble_differential_dibit(
		    &j_prime->descrambler,
		    j_prime->previous_clockwise_quarter_turns,
		    hard->clockwise_quarter_turns, &decoded) != V34_FRAMING_OK)
		return false;
	j_prime->previous_clockwise_quarter_turns =
		hard->clockwise_quarter_turns;
	j_prime->decoded_history = (uint16_t)(
		(j_prime->decoded_history << 2u) | decoded);
	j_prime->qualified_history = (uint8_t)(
		(j_prime->qualified_history << 1u) |
		training_decision_is_qualified(observation, hard->target));
	if (j_prime->received_symbols < V34_PHASE4_J_PRIME_SYMBOLS)
		j_prime->received_symbols++;
	*detected = j_prime->received_symbols ==
			    V34_PHASE4_J_PRIME_SYMBOLS &&
		    j_prime_is_qualified(
			    j_prime->decoded_history,
			    j_prime->qualified_history);
	return true;
}

static enum v34_phase4_decision_receiver_result receive_call_j_prime(
	struct v34_phase4_decision_receiver *receiver,
	const struct v34_adaptive_equalizer_observation *observation,
	uint16_t decoder_power_q15,
	const struct v34_fixed_training_decision *hard,
	struct v34_phase4_decision_output *output)
{
	bool detected;

	if (!advance_call_j_prime(
		    receiver, observation, hard, &detected))
		return V34_PHASE4_DECISION_RECEIVER_INVALID_STATE;
	*output = commit_output(
		observation, hard->target, decoder_power_q15);
	if (detected)
		receiver->stage =
			V34_PHASE4_DECISION_RECEIVER_CALL_TRN_CANDIDATE;
	return V34_PHASE4_DECISION_RECEIVER_OK;
}

static bool reset_expected_trn(
	struct v34_phase4_decision_receiver *receiver)
{
	struct v34_phase4_trn reset;

	if (v34_phase4_trn_init(
		    &reset, receiver->mp_receiver.transmitting_role,
		    receiver->mp_receiver.constellation) != V34_PHASE4_TRN_OK)
		return false;
	receiver->expected_trn = reset;
	receiver->trn_symbols = 0u;
	receiver->trn_candidate_errors = 0u;
	return true;
}

static enum v34_phase4_decision_receiver_result
receive_call_trn_candidate(
	struct v34_phase4_decision_receiver *receiver,
	const struct v34_adaptive_equalizer_observation *observation,
	uint16_t decoder_power_q15,
	const struct v34_fixed_training_decision *j_hard,
	const struct v34_fixed_training_decision *trn_hard,
	struct v34_phase4_decision_output *output)
{
	struct v34_analytic_sample expected;
	uint8_t expected_quarter;
	uint8_t expected_rotation;
	bool j_prime_detected;

	if (!advance_call_j_prime(
		    receiver, observation, j_hard, &j_prime_detected))
		return V34_PHASE4_DECISION_RECEIVER_INVALID_STATE;
	if (v34_phase4_trn_next_fixed(
		    &receiver->expected_trn, &expected, &expected_quarter,
		    &expected_rotation) != V34_PHASE4_TRN_OK)
		return V34_PHASE4_DECISION_RECEIVER_INVALID_STATE;
	receiver->trn_symbols++;
	if (trn_hard->quarter_point != expected_quarter ||
	    trn_hard->clockwise_quarter_turns != expected_rotation)
		receiver->trn_candidate_errors++;
	*output = commit_output(
		observation, trn_hard->target, decoder_power_q15);
	if (receiver->trn_candidate_errors <=
	    V34_PHASE4_TRN_CONFIRM_MAX_ERRORS) {
		if (receiver->trn_symbols ==
		    V34_PHASE4_TRN_CONFIRM_SYMBOLS) {
			receiver->trn_candidate_errors = 0u;
			receiver->stage = V34_PHASE4_DECISION_RECEIVER_TRN;
		}
		return V34_PHASE4_DECISION_RECEIVER_OK;
	}
	if (!reset_expected_trn(receiver))
		return V34_PHASE4_DECISION_RECEIVER_INVALID_STATE;
	receiver->stage = j_prime_detected ?
		V34_PHASE4_DECISION_RECEIVER_CALL_TRN_CANDIDATE :
		V34_PHASE4_DECISION_RECEIVER_CALL_J_PRIME;
	return V34_PHASE4_DECISION_RECEIVER_OK;
}

static enum v34_phase4_decision_receiver_result receive_trn(
	struct v34_phase4_decision_receiver *receiver,
	const struct v34_adaptive_equalizer_observation *observation,
	uint16_t decoder_power_q15,
	const struct v34_fixed_training_decision *hard,
	struct v34_phase4_decision_output *output)
{
	struct v34_mp_receive_event ignored = { 0 };
	struct v34_analytic_sample target;
	uint8_t quarter;
	uint8_t rotation;

	if (v34_phase4_trn_next_fixed(
		    &receiver->expected_trn, &target, &quarter, &rotation) !=
	    V34_PHASE4_TRN_OK)
		return V34_PHASE4_DECISION_RECEIVER_INVALID_STATE;
	receiver->trn_symbols++;
	*output = commit_output(observation, target, decoder_power_q15);
	if (receiver->trn_symbols != V34_PHASE4_TRN_MIN_SYMBOLS)
		return V34_PHASE4_DECISION_RECEIVER_OK;
	if (v34_phase4_mp_receive_decision(
		    &receiver->mp_receiver, hard->quarter_point,
		    hard->clockwise_quarter_turns, &ignored) !=
		    V34_PHASE4_MP_RECEIVER_OK ||
	    ignored.type != V34_MP_RECEIVE_NO_EVENT)
		return V34_PHASE4_DECISION_RECEIVER_INVALID_STATE;
	receiver->stage = V34_PHASE4_DECISION_RECEIVER_MP;
	return V34_PHASE4_DECISION_RECEIVER_TRN_COMPLETE;
}

static enum v34_phase4_decision_receiver_result receive_mp(
	struct v34_phase4_decision_receiver *receiver,
	const struct v34_adaptive_equalizer_observation *observation,
	uint16_t decoder_power_q15,
	const struct v34_fixed_training_decision *hard,
	struct v34_phase4_decision_output *output,
	struct v34_mp_receive_event *event)
{
	struct v34_mp_receive_event emitted = { 0 };

	if (v34_phase4_mp_receive_decision(
		    &receiver->mp_receiver, hard->quarter_point,
		    hard->clockwise_quarter_turns, &emitted) !=
	    V34_PHASE4_MP_RECEIVER_OK)
		return V34_PHASE4_DECISION_RECEIVER_INVALID_STATE;
	*output = commit_output(observation, hard->target, decoder_power_q15);
	if (emitted.type == V34_MP_RECEIVE_NO_EVENT)
		return V34_PHASE4_DECISION_RECEIVER_OK;
	receiver->record_pending = true;
	receiver->pending_record_is_valid =
		(emitted.type == V34_MP_RECEIVE_TYPE0 ||
		 emitted.type == V34_MP_RECEIVE_TYPE1) &&
		emitted.frame_result == V34_MP_OK;
	*event = emitted;
	return V34_PHASE4_DECISION_RECEIVER_RECORD_READY;
}

static enum v34_phase4_decision_receiver_result receive_e(
	struct v34_phase4_decision_receiver *receiver,
	const struct v34_adaptive_equalizer_observation *observation,
	uint16_t decoder_power_q15,
	const struct v34_fixed_training_decision *hard,
	struct v34_phase4_decision_output *output)
{
	enum v34_phase4_e_receiver_result result =
		v34_phase4_e_receive_decision(
			&receiver->e_receiver, hard->quarter_point,
			hard->clockwise_quarter_turns);

	if (result == V34_PHASE4_E_RECEIVER_SEQUENCE_MISMATCH)
		return V34_PHASE4_DECISION_RECEIVER_SEQUENCE_MISMATCH;
	if (result != V34_PHASE4_E_RECEIVER_OK &&
	    result != V34_PHASE4_E_RECEIVER_COMPLETE)
		return V34_PHASE4_DECISION_RECEIVER_INVALID_STATE;
	*output = commit_output(observation, hard->target, decoder_power_q15);
	if (result == V34_PHASE4_E_RECEIVER_OK)
		return V34_PHASE4_DECISION_RECEIVER_OK;
	receiver->stage = V34_PHASE4_DECISION_RECEIVER_COMPLETE;
	return V34_PHASE4_DECISION_RECEIVER_E_COMPLETE;
}

static bool output_is_valid(
	const struct v34_phase4_decision_output *output,
	const struct v34_adaptive_equalizer_observation *observation,
	uint16_t decoder_power_q15)
{
	if (output->action == V34_PHASE4_DECISION_DISCARD ||
	    output->action == V34_PHASE4_DECISION_BEGIN_TRACKING)
		return output->equalizer.observation.decision.in_phase == 0 &&
		       output->equalizer.observation.decision.quadrature == 0 &&
		       output->equalizer.observation.epoch == 0u &&
		       output->equalizer.observation.lineage == 0u &&
		       output->equalizer.target.in_phase == 0 &&
		       output->equalizer.target.quadrature == 0 &&
		       output->equalizer.decoder_power_q15 == 0u &&
		       output->equalizer.adaptation ==
			       V34_ADAPTIVE_EQUALIZER_SKIP;
	return output->action == V34_PHASE4_DECISION_COMMIT &&
	       output->equalizer.observation.decision.in_phase ==
		       observation->decision.in_phase &&
	       output->equalizer.observation.decision.quadrature ==
		       observation->decision.quadrature &&
	       output->equalizer.observation.epoch == observation->epoch &&
	       output->equalizer.observation.lineage == observation->lineage &&
	       output->equalizer.decoder_power_q15 == decoder_power_q15 &&
	       output->equalizer.adaptation == V34_ADAPTIVE_EQUALIZER_FULL;
}

static bool consumed_result(
	enum v34_phase4_decision_receiver_result result)
{
	return result == V34_PHASE4_DECISION_RECEIVER_OK ||
	       result == V34_PHASE4_DECISION_RECEIVER_S_DETECTED ||
	       result == V34_PHASE4_DECISION_RECEIVER_TRN_COMPLETE ||
	       result == V34_PHASE4_DECISION_RECEIVER_RECORD_READY ||
	       result == V34_PHASE4_DECISION_RECEIVER_E_COMPLETE;
}

enum v34_phase4_decision_receiver_result
v34_phase4_decision_receiver_observe(
	struct v34_phase4_decision_receiver *receiver,
	const struct v34_adaptive_equalizer_observation *observation,
	uint16_t decoder_power_q15,
	struct v34_phase4_decision_output *output,
	struct v34_mp_receive_event *event)
{
	struct v34_phase4_decision_receiver next;
	struct v34_phase4_decision_output generated = { 0 };
	struct v34_mp_receive_event emitted = { 0 };
	struct v34_fixed_training_decision hard;
	struct v34_fixed_training_decision j_hard;
	enum v34_phase4_decision_receiver_result result;
	enum v34_constellation_size constellation;

	if (observation == NULL || output == NULL || event == NULL ||
	    memory_regions_overlap(
		    receiver, receiver == NULL ? 0 : sizeof(*receiver),
		    observation, sizeof(*observation)) ||
	    memory_regions_overlap(
		    receiver, receiver == NULL ? 0 : sizeof(*receiver),
		    output, sizeof(*output)) ||
	    memory_regions_overlap(
		    receiver, receiver == NULL ? 0 : sizeof(*receiver),
		    event, sizeof(*event)) ||
	    memory_regions_overlap(
		    observation, sizeof(*observation), output,
		    sizeof(*output)) ||
	    memory_regions_overlap(
		    observation, sizeof(*observation), event, sizeof(*event)) ||
	    memory_regions_overlap(
		    output, sizeof(*output), event, sizeof(*event)))
		return V34_PHASE4_DECISION_RECEIVER_INVALID_ARGUMENT;
	if (!v34_phase4_decision_receiver_is_valid(receiver))
		return V34_PHASE4_DECISION_RECEIVER_INVALID_STATE;
	if (observation->epoch == 0u || observation->lineage == 0u ||
	    decoder_power_q15 == 0u || decoder_power_q15 > INT16_MAX)
		return V34_PHASE4_DECISION_RECEIVER_INVALID_ARGUMENT;
	if (receiver->record_pending)
		return V34_PHASE4_DECISION_RECEIVER_INVALID_STATE;
	if (receiver->stage == V34_PHASE4_DECISION_RECEIVER_COMPLETE)
		return V34_PHASE4_DECISION_RECEIVER_ALREADY_COMPLETE;
	next = *receiver;
	constellation =
		next.stage == V34_PHASE4_DECISION_RECEIVER_CALL_J_PRIME ||
				next.stage == V34_PHASE4_DECISION_RECEIVER_ANSWER_S ?
			V34_CONSTELLATION_4_POINT :
			next.mp_receiver.constellation;
	if (v34_training_constellation_fixed_slice(
		    constellation, observation->decision, &hard) !=
	    V34_CONSTELLATION_OK)
		return V34_PHASE4_DECISION_RECEIVER_INVALID_STATE;
	if (next.stage ==
	    V34_PHASE4_DECISION_RECEIVER_CALL_TRN_CANDIDATE) {
		if (constellation == V34_CONSTELLATION_4_POINT)
			j_hard = hard;
		else if (v34_training_constellation_fixed_slice(
				 V34_CONSTELLATION_4_POINT,
				 observation->decision, &j_hard) !=
			 V34_CONSTELLATION_OK)
			return V34_PHASE4_DECISION_RECEIVER_INVALID_STATE;
		result = receive_call_trn_candidate(
			&next, observation, decoder_power_q15, &j_hard, &hard,
			&generated);
	} else if (next.stage == V34_PHASE4_DECISION_RECEIVER_TRN)
		result = receive_trn(
			&next, observation, decoder_power_q15, &hard,
			&generated);
	else if (next.stage == V34_PHASE4_DECISION_RECEIVER_CALL_J_PRIME)
		result = receive_call_j_prime(
			&next, observation, decoder_power_q15, &hard,
			&generated);
	else if (next.stage == V34_PHASE4_DECISION_RECEIVER_ANSWER_S)
		result = receive_answer_s(
			&next, observation, &hard, &generated);
	else if (next.stage == V34_PHASE4_DECISION_RECEIVER_MP)
		result = receive_mp(
			&next, observation, decoder_power_q15, &hard,
			&generated, &emitted);
	else
		result = receive_e(
			&next, observation, decoder_power_q15, &hard,
			&generated);
	if (!consumed_result(result))
		return result;
	if (!v34_phase4_decision_receiver_is_valid(&next) ||
	    !output_is_valid(&generated, observation, decoder_power_q15))
		return V34_PHASE4_DECISION_RECEIVER_INVALID_STATE;
	*receiver = next;
	*output = generated;
	if (result == V34_PHASE4_DECISION_RECEIVER_RECORD_READY)
		*event = emitted;
	return result;
}
