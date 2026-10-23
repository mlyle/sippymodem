// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase4_decision_receiver.h"

#include "memory_region.h"
#include "sample_math.h"
#include "v34_constellation.h"
#include "v34_phase4_e_sequence.h"

#include <limits.h>
#include <stddef.h>

enum {
	V34_PHASE4_DECISION_RECEIVER_TRN_CONFIRM_MAX_ERRORS = 1,
};

static bool s_is_initial(const struct v34_phase3_s_hunt *receiver)
{
	return receiver->recent_observation_count == 0u &&
	       receiver->prefix.recurrence_points == 0u &&
	       !receiver->prefix.previous_valid && !receiver->candidate;
}

static bool trn_is_initial(const struct v34_training_sequence *trn)
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
	uint64_t error_energy = sample_pair_distance_s16(
		observation->decision.in_phase, observation->decision.quadrature,
		target.in_phase, target.quadrature);
	uint64_t target_energy =
		sample_pair_power_s16(target.in_phase, target.quadrature);

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

	if (!v34_phase3_s_hunt_is_valid(&receiver->answer_s) ||
	    !receiver->answer_s.acquire_phase)
		return false;
	if (!answering_peer)
		return receiver->stage != V34_PHASE4_DECISION_RECEIVER_ANSWER_S &&
		       s_is_initial(&receiver->answer_s);
	return receiver->stage == V34_PHASE4_DECISION_RECEIVER_ANSWER_S ||
	       v34_phase3_s_hunt_is_complete(&receiver->answer_s);
}

static bool answer_confirmation_is_valid(
	const struct v34_phase4_decision_receiver *receiver)
{
	uint64_t bound = (uint64_t)receiver->trn_symbols * 2u *
			32768u * 32768u;

	if (receiver->stage != V34_PHASE4_DECISION_RECEIVER_ANSWER_S ||
	    !v34_phase3_s_hunt_is_complete(&receiver->answer_s))
		bound = 0u;
	return receiver->trn_correlation_in_phase >= -(int64_t)bound &&
	       receiver->trn_correlation_in_phase <= (int64_t)bound &&
	       receiver->trn_correlation_quadrature >= -(int64_t)bound &&
	       receiver->trn_correlation_quadrature <= (int64_t)bound &&
	       receiver->trn_observed_energy <= bound &&
	       receiver->trn_target_energy <= bound;
}

static bool trn_state_is_valid(
	const struct v34_phase4_decision_receiver *receiver)
{
	bool calling_peer = receiver->mp_receiver.transmitting_role ==
		V34_CALLING_STATION;

	if (!v34_training_sequence_is_valid(&receiver->expected_trn) ||
	    receiver->expected_trn.role !=
		    receiver->mp_receiver.transmitting_role ||
	    receiver->expected_trn.constellation !=
		    receiver->mp_receiver.constellation ||
	    receiver->trn_symbols > V34_TRAINING_SEQUENCE_MIN_SYMBOLS ||
	    receiver->trn_candidate_errors >
		    V34_PHASE4_DECISION_RECEIVER_TRN_CONFIRM_MAX_ERRORS)
		return false;
	if (receiver->stage == V34_PHASE4_DECISION_RECEIVER_ANSWER_S &&
	    v34_phase3_s_hunt_is_complete(&receiver->answer_s))
		return receiver->trn_symbols <
			       V34_PHASE4_DECISION_RECEIVER_TRN_CONFIRM_SYMBOLS &&
		       receiver->trn_candidate_errors == 0u &&
		       receiver->expected_trn.has_last_symbol ==
			       (receiver->trn_symbols != 0u);
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
			    V34_PHASE4_DECISION_RECEIVER_TRN_CONFIRM_SYMBOLS)
			return false;
	} else if (receiver->trn_candidate_errors != 0u) {
		return false;
	}
	if (receiver->expected_trn.has_last_symbol !=
	    (receiver->trn_symbols != 0u))
		return false;
	if (receiver->stage == V34_PHASE4_DECISION_RECEIVER_TRN)
		return receiver->trn_symbols < V34_TRAINING_SEQUENCE_MIN_SYMBOLS &&
		       receiver->trn_symbols >=
			       V34_PHASE4_DECISION_RECEIVER_TRN_CONFIRM_SYMBOLS;
	if (receiver->stage ==
	    V34_PHASE4_DECISION_RECEIVER_TRN_OR_MP)
		return receiver->mp_receiver.constellation ==
			       V34_CONSTELLATION_4_POINT &&
		       receiver->trn_symbols ==
			       V34_TRAINING_SEQUENCE_MIN_SYMBOLS &&
		       receiver->mp_receiver.has_previous_symbol &&
		       receiver->mp_receiver.descrambler.history ==
			       receiver->expected_trn.scrambler.history &&
		       receiver->mp_receiver.descrambler.polynomial ==
			       receiver->expected_trn.scrambler.polynomial &&
		       receiver->mp_receiver
			       .previous_clockwise_quarter_turns ==
			       receiver->expected_trn
				       .last_clockwise_quarter_turns;
	return receiver->stage ==
		       V34_PHASE4_DECISION_RECEIVER_CALL_TRN_CANDIDATE ||
	       receiver->trn_symbols == V34_TRAINING_SEQUENCE_MIN_SYMBOLS;
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
	    !answer_confirmation_is_valid(receiver) ||
	    (receiver->rate_renegotiation ?
	     (receiver->stage < V34_PHASE4_DECISION_RECEIVER_MP ||
	      receiver->mp_receiver.constellation != V34_CONSTELLATION_4_POINT) :
	     (!answer_s_state_is_valid(receiver) ||
	      !call_j_prime_state_is_valid(receiver) ||
	      !trn_state_is_valid(receiver))) ||
	    (receiver->mp_received &&
	     receiver->stage < V34_PHASE4_DECISION_RECEIVER_MP) ||
	    (receiver->record_pending &&
	     receiver->stage != V34_PHASE4_DECISION_RECEIVER_MP))
		return false;
	return (receiver->mp_received &&
		receiver->mp_receiver.consecutive_ones == V34_PHASE4_E_BITS) ==
	       (receiver->stage == V34_PHASE4_DECISION_RECEIVER_COMPLETE);
}

bool v34_phase4_decision_receiver_is_complete(
	const struct v34_phase4_decision_receiver *receiver)
{
	return v34_phase4_decision_receiver_is_valid(receiver) &&
	       receiver->stage == V34_PHASE4_DECISION_RECEIVER_COMPLETE;
}

enum v34_phase4_decision_receiver_result
v34_phase4_decision_receiver_resume_online(
	struct v34_phase4_decision_receiver *receiver,
	enum v34_station_role transmitting_role)
{
	struct v34_phase4_decision_receiver next = {
		.stage = V34_PHASE4_DECISION_RECEIVER_MP,
		.rate_renegotiation = true,
	};

	if (receiver == NULL ||
	    v34_phase4_mp_receiver_init(&next.mp_receiver, transmitting_role,
		    V34_CONSTELLATION_4_POINT) != V34_PHASE4_MP_RECEIVER_OK ||
	    !v34_phase4_decision_receiver_is_valid(&next))
		return V34_PHASE4_DECISION_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
	return V34_PHASE4_DECISION_RECEIVER_OK;
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
	    v34_phase3_s_hunt_init_training(&initialized.answer_s) !=
		    V34_PHASE3_S_RECEIVER_OK ||
	    v34_training_sequence_init(
		    &initialized.expected_trn, transmitting_role,
		    constellation) != V34_TRAINING_SEQUENCE_OK ||
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

enum v34_phase4_decision_receiver_result
v34_phase4_decision_receiver_continue_mp(
	struct v34_phase4_decision_receiver *receiver)
{
	if (!v34_phase4_decision_receiver_is_valid(receiver))
		return V34_PHASE4_DECISION_RECEIVER_INVALID_STATE;
	if (receiver->stage != V34_PHASE4_DECISION_RECEIVER_MP ||
	    !receiver->record_pending)
		return V34_PHASE4_DECISION_RECEIVER_INVALID_STATE;
	receiver->record_pending = false;
	return V34_PHASE4_DECISION_RECEIVER_OK;
}

static bool reset_expected_trn(
	struct v34_phase4_decision_receiver *receiver)
{
	struct v34_training_sequence reset;

	if (v34_training_sequence_init(
		    &reset, receiver->mp_receiver.transmitting_role,
		    receiver->mp_receiver.constellation) != V34_TRAINING_SEQUENCE_OK)
		return false;
	receiver->expected_trn = reset;
	receiver->trn_symbols = 0u;
	receiver->trn_candidate_errors = 0u;
	return true;
}

static bool confirm_answer_trn(
	struct v34_phase4_decision_receiver *receiver,
	struct v34_analytic_sample observed)
{
	struct v34_analytic_sample target;
	uint8_t quarter;
	uint8_t rotation;

	if (v34_training_sequence_next_fixed(
		    &receiver->expected_trn, &target, &quarter, &rotation) !=
	    V34_TRAINING_SEQUENCE_OK)
		return false;
	receiver->trn_symbols++;
	receiver->trn_correlation_in_phase +=
		(int64_t)observed.in_phase * target.in_phase +
		(int64_t)observed.quadrature * target.quadrature;
	receiver->trn_correlation_quadrature +=
		(int64_t)observed.quadrature * target.in_phase -
		(int64_t)observed.in_phase * target.quadrature;
	receiver->trn_observed_energy +=
		sample_pair_power_s16(observed.in_phase, observed.quadrature);
	receiver->trn_target_energy +=
		sample_pair_power_s16(target.in_phase, target.quadrature);
	return true;
}

static enum v34_phase4_decision_receiver_result finish_answer_confirmation(
	struct v34_phase4_decision_receiver *receiver,
	struct v34_receive_action *output)
{
	double real = (double)receiver->trn_correlation_in_phase;
	double imaginary = (double)receiver->trn_correlation_quadrature;
	bool qualified;

	*output = v34_receive_action_only(V34_RECEIVE_DISCARD);
	if (receiver->trn_symbols <
	    V34_PHASE4_DECISION_RECEIVER_TRN_CONFIRM_SYMBOLS)
		return V34_PHASE4_DECISION_RECEIVER_OK;
	/* A reflected local S has the same marker but a different TRN scrambler.
	 * Require correlation >= 0.65 with peer TRN at useful signal power.
	 * Complex magnitude permits residual carrier phase; no known-sequence
	 * feedback or transmitter handoff is committed before qualification. */
	qualified = receiver->trn_target_energy != 0u &&
		receiver->trn_observed_energy * 16u >=
			receiver->trn_target_energy &&
		real * real + imaginary * imaginary >= 0.4225 *
			(double)receiver->trn_observed_energy *
			(double)receiver->trn_target_energy;
	receiver->trn_correlation_in_phase = 0;
	receiver->trn_correlation_quadrature = 0;
	receiver->trn_observed_energy = 0u;
	receiver->trn_target_energy = 0u;
	if (!qualified) {
		if (!reset_expected_trn(receiver) ||
		    v34_phase3_s_hunt_init_training(&receiver->answer_s) !=
			    V34_PHASE3_S_RECEIVER_OK)
			return V34_PHASE4_DECISION_RECEIVER_INVALID_STATE;
		return V34_PHASE4_DECISION_RECEIVER_OK;
	}
	receiver->stage = V34_PHASE4_DECISION_RECEIVER_TRN;
	*output = v34_receive_action_only(V34_RECEIVE_BEGIN_TRACKING);
	return V34_PHASE4_DECISION_RECEIVER_S_DETECTED;
}

static enum v34_phase4_decision_receiver_result receive_answer_s(
	struct v34_phase4_decision_receiver *receiver,
	const struct v34_adaptive_equalizer_observation *observation,
	uint16_t decoder_power_q15,
	struct v34_receive_action *output)
{
	struct v34_phase3_s_adaptation adaptation;
	uint8_t post_marker_symbols;
	bool control;
	enum v34_phase3_s_receiver_result result;

	if (v34_phase3_s_hunt_is_complete(&receiver->answer_s)) {
		if (!confirm_answer_trn(receiver, observation->decision))
			return V34_PHASE4_DECISION_RECEIVER_INVALID_STATE;
		return finish_answer_confirmation(receiver, output);
	}
	result =
		v34_phase3_s_hunt_receive(
			&receiver->answer_s, observation->decision,
			&adaptation, &control);

	if (result == V34_PHASE3_S_RECEIVER_OK) {
		*output = adaptation.adapt ?
			v34_receive_action_commit(
				observation, adaptation.target,
				decoder_power_q15,
				V34_ADAPTIVE_EQUALIZER_CENTER) :
			v34_receive_action_only(V34_RECEIVE_DISCARD);
		return V34_PHASE4_DECISION_RECEIVER_OK;
	}
	*output = v34_receive_action_only(V34_RECEIVE_DISCARD);
	if (result != V34_PHASE3_S_RECEIVER_DETECTED ||
	    !v34_phase3_s_hunt_get_post_marker_symbols(
		    &receiver->answer_s, &post_marker_symbols))
		return V34_PHASE4_DECISION_RECEIVER_INVALID_STATE;
	for (uint8_t symbol = 0; symbol < post_marker_symbols; ++symbol) {
		struct v34_analytic_sample observed;

		if (!v34_phase3_s_hunt_get_post_marker_observation(
			    &receiver->answer_s, symbol, &observed) ||
		    !confirm_answer_trn(receiver, observed))
			return V34_PHASE4_DECISION_RECEIVER_INVALID_STATE;
	}
	return finish_answer_confirmation(receiver, output);
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
	struct v34_receive_action *output)
{
	bool detected;

	if (!advance_call_j_prime(
		    receiver, observation, hard, &detected))
		return V34_PHASE4_DECISION_RECEIVER_INVALID_STATE;
	*output = v34_receive_action_commit(
		observation, hard->target, decoder_power_q15,
		V34_ADAPTIVE_EQUALIZER_FULL);
	if (detected)
		receiver->stage =
			V34_PHASE4_DECISION_RECEIVER_CALL_TRN_CANDIDATE;
	return V34_PHASE4_DECISION_RECEIVER_OK;
}

static enum v34_phase4_decision_receiver_result
receive_call_trn_candidate(
	struct v34_phase4_decision_receiver *receiver,
	const struct v34_adaptive_equalizer_observation *observation,
	uint16_t decoder_power_q15,
	const struct v34_fixed_training_decision *j_hard,
	const struct v34_fixed_training_decision *trn_hard,
	struct v34_receive_action *output)
{
	struct v34_analytic_sample expected;
	uint8_t expected_quarter;
	uint8_t expected_rotation;
	bool j_prime_detected;

	if (!advance_call_j_prime(
		    receiver, observation, j_hard, &j_prime_detected))
		return V34_PHASE4_DECISION_RECEIVER_INVALID_STATE;
	if (v34_training_sequence_next_fixed(
		    &receiver->expected_trn, &expected, &expected_quarter,
		    &expected_rotation) != V34_TRAINING_SEQUENCE_OK)
		return V34_PHASE4_DECISION_RECEIVER_INVALID_STATE;
	receiver->trn_symbols++;
	if (trn_hard->quarter_point != expected_quarter ||
	    trn_hard->clockwise_quarter_turns != expected_rotation)
		receiver->trn_candidate_errors++;
	*output = v34_receive_action_commit(
		observation, trn_hard->target, decoder_power_q15,
		V34_ADAPTIVE_EQUALIZER_FULL);
	if (receiver->trn_candidate_errors <=
	    V34_PHASE4_DECISION_RECEIVER_TRN_CONFIRM_MAX_ERRORS) {
		if (receiver->trn_symbols ==
		    V34_PHASE4_DECISION_RECEIVER_TRN_CONFIRM_SYMBOLS) {
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
	struct v34_receive_action *output)
{
	struct v34_mp_receive_event ignored = { 0 };
	struct v34_analytic_sample target;
	uint8_t quarter;
	uint8_t rotation;

	if (v34_training_sequence_next_fixed(
		    &receiver->expected_trn, &target, &quarter, &rotation) !=
	    V34_TRAINING_SEQUENCE_OK)
		return V34_PHASE4_DECISION_RECEIVER_INVALID_STATE;
	receiver->trn_symbols++;
	*output = v34_receive_action_commit(
		observation, target, decoder_power_q15,
		V34_ADAPTIVE_EQUALIZER_FULL);
	if (receiver->trn_symbols != V34_TRAINING_SEQUENCE_MIN_SYMBOLS)
		return V34_PHASE4_DECISION_RECEIVER_OK;
	if (receiver->mp_receiver.constellation ==
	    V34_CONSTELLATION_4_POINT) {
		if (v34_phase4_mp_receiver_init_from_trn(
			    &receiver->mp_receiver,
			    &receiver->expected_trn) !=
		    V34_PHASE4_MP_RECEIVER_OK)
			return V34_PHASE4_DECISION_RECEIVER_INVALID_STATE;
		receiver->stage =
			V34_PHASE4_DECISION_RECEIVER_TRN_OR_MP;
	} else {
		if (v34_phase4_mp_receive_decision(
			    &receiver->mp_receiver, hard->quarter_point,
			    hard->clockwise_quarter_turns, &ignored) !=
			    V34_PHASE4_MP_RECEIVER_OK ||
		    ignored.type != V34_MP_RECEIVE_NO_EVENT)
			return V34_PHASE4_DECISION_RECEIVER_INVALID_STATE;
		receiver->stage = V34_PHASE4_DECISION_RECEIVER_MP;
	}
	return V34_PHASE4_DECISION_RECEIVER_TRN_COMPLETE;
}

static enum v34_phase4_decision_receiver_result receive_mp(
	struct v34_phase4_decision_receiver *receiver,
	const struct v34_adaptive_equalizer_observation *observation,
	uint16_t decoder_power_q15,
	const struct v34_fixed_training_decision *hard,
	struct v34_receive_action *output,
	struct v34_mp_receive_event *event);

static bool mp_decoder_matches_trn(
	const struct v34_phase4_mp_receiver *receiver,
	const struct v34_training_sequence *trn)
{
	return receiver->has_previous_symbol &&
	       receiver->descrambler.history == trn->scrambler.history &&
	       receiver->descrambler.polynomial == trn->scrambler.polynomial &&
	       receiver->previous_clockwise_quarter_turns ==
		       trn->last_clockwise_quarter_turns;
}

static enum v34_phase4_decision_receiver_result receive_trn_or_mp(
	struct v34_phase4_decision_receiver *receiver,
	const struct v34_adaptive_equalizer_observation *observation,
	uint16_t decoder_power_q15,
	const struct v34_fixed_training_decision *hard,
	struct v34_receive_action *output,
	struct v34_mp_receive_event *event)
{
	struct v34_training_sequence expected = receiver->expected_trn;
	struct v34_phase4_mp_receiver candidate = receiver->mp_receiver;
	struct v34_mp_receive_event ignored = { 0 };
	struct v34_analytic_sample target;
	uint8_t quarter;
	uint8_t rotation;

	if (v34_training_sequence_next_fixed(
		    &expected, &target, &quarter, &rotation) !=
	    V34_TRAINING_SEQUENCE_OK)
		return V34_PHASE4_DECISION_RECEIVER_INVALID_STATE;
	if (hard->quarter_point != quarter ||
	    hard->clockwise_quarter_turns != rotation) {
		receiver->stage = V34_PHASE4_DECISION_RECEIVER_MP;
		return receive_mp(
			receiver, observation, decoder_power_q15, hard,
			output, event);
	}
	if (v34_phase4_mp_receive_decision(
		    &candidate, hard->quarter_point,
		    hard->clockwise_quarter_turns, &ignored) !=
		    V34_PHASE4_MP_RECEIVER_OK ||
	    ignored.type != V34_MP_RECEIVE_NO_EVENT)
		return V34_PHASE4_DECISION_RECEIVER_INVALID_STATE;
	receiver->expected_trn = expected;
	if (mp_decoder_matches_trn(&candidate, &expected)) {
		receiver->mp_receiver = candidate;
	} else if (v34_phase4_mp_receiver_init_from_trn(
			   &receiver->mp_receiver, &expected) !=
		   V34_PHASE4_MP_RECEIVER_OK) {
		return V34_PHASE4_DECISION_RECEIVER_INVALID_STATE;
	}
	*output = v34_receive_action_commit(
		observation, target, decoder_power_q15,
		V34_ADAPTIVE_EQUALIZER_FULL);
	return V34_PHASE4_DECISION_RECEIVER_OK;
}

static enum v34_phase4_decision_receiver_result receive_mp(
	struct v34_phase4_decision_receiver *receiver,
	const struct v34_adaptive_equalizer_observation *observation,
	uint16_t decoder_power_q15,
	const struct v34_fixed_training_decision *hard,
	struct v34_receive_action *output,
	struct v34_mp_receive_event *event)
{
	struct v34_mp_receive_event emitted = { 0 };

	if (v34_phase4_mp_receive_decision(
		    &receiver->mp_receiver, hard->quarter_point,
		    hard->clockwise_quarter_turns, &emitted) !=
	    V34_PHASE4_MP_RECEIVER_OK)
		return V34_PHASE4_DECISION_RECEIVER_INVALID_STATE;
	*output = v34_receive_action_commit(
		observation, hard->target, decoder_power_q15,
		V34_ADAPTIVE_EQUALIZER_FULL);
	if (receiver->mp_received &&
	    receiver->mp_receiver.consecutive_ones == V34_PHASE4_E_BITS) {
		receiver->stage = V34_PHASE4_DECISION_RECEIVER_COMPLETE;
		return V34_PHASE4_DECISION_RECEIVER_E_COMPLETE;
	}
	if (emitted.type == V34_MP_RECEIVE_NO_EVENT)
		return V34_PHASE4_DECISION_RECEIVER_OK;
	receiver->record_pending = true;
	receiver->mp_received |=
		(emitted.type == V34_MP_RECEIVE_TYPE0 ||
		 emitted.type == V34_MP_RECEIVE_TYPE1) &&
		emitted.frame_result == V34_MP_OK;
	*event = emitted;
	return V34_PHASE4_DECISION_RECEIVER_RECORD_READY;
}

static bool output_is_valid(
	const struct v34_receive_action *output,
	const struct v34_adaptive_equalizer_observation *observation,
	uint16_t decoder_power_q15,
	const struct v34_phase4_decision_receiver *receiver)
{
	if (!v34_receive_action_is_valid(
		    output, observation, decoder_power_q15) ||
	    output->action == V34_RECEIVE_RESTART_PATH)
		return false;
	return output->action != V34_RECEIVE_COMMIT ||
	       output->equalizer.adaptation ==
		       (receiver->stage ==
					V34_PHASE4_DECISION_RECEIVER_ANSWER_S ?
				V34_ADAPTIVE_EQUALIZER_CENTER :
				V34_ADAPTIVE_EQUALIZER_FULL);
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
	struct v34_receive_action *output,
	struct v34_mp_receive_event *event)
{
	const struct memory_region regions[] = {
		{ receiver, receiver == NULL ? 0 : sizeof(*receiver) },
		{ observation, sizeof(*observation) },
		{ output, sizeof(*output) },
		{ event, sizeof(*event) },
	};
	struct v34_phase4_decision_receiver next;
	struct v34_receive_action generated = { 0 };
	struct v34_mp_receive_event emitted = { 0 };
	struct v34_fixed_training_decision hard;
	struct v34_fixed_training_decision j_hard;
	enum v34_phase4_decision_receiver_result result;
	enum v34_constellation_size constellation;

	if (observation == NULL || output == NULL || event == NULL ||
	    memory_region_set_overlaps(regions, sizeof regions / sizeof *regions))
		return V34_PHASE4_DECISION_RECEIVER_INVALID_ARGUMENT;
	if (!v34_phase4_decision_receiver_is_valid(receiver))
		return V34_PHASE4_DECISION_RECEIVER_INVALID_STATE;
	if (!v34_adaptive_equalizer_observation_is_valid(observation) ||
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
	else if (next.stage ==
		 V34_PHASE4_DECISION_RECEIVER_TRN_OR_MP)
		result = receive_trn_or_mp(
			&next, observation, decoder_power_q15, &hard,
			&generated, &emitted);
	else if (next.stage == V34_PHASE4_DECISION_RECEIVER_CALL_J_PRIME)
		result = receive_call_j_prime(
			&next, observation, decoder_power_q15, &hard,
			&generated);
	else if (next.stage == V34_PHASE4_DECISION_RECEIVER_ANSWER_S)
		result = receive_answer_s(
			&next, observation, decoder_power_q15,
			&generated);
	else
		result = receive_mp(
			&next, observation, decoder_power_q15, &hard,
			&generated, &emitted);
	if (!consumed_result(result))
		return result;
	if (!v34_phase4_decision_receiver_is_valid(&next) ||
	    !output_is_valid(
		    &generated, observation, decoder_power_q15, &next))
		return V34_PHASE4_DECISION_RECEIVER_INVALID_STATE;
	*receiver = next;
	*output = generated;
	if (result == V34_PHASE4_DECISION_RECEIVER_RECORD_READY)
		*event = emitted;
	return result;
}
