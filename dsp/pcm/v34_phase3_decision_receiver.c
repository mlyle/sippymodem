// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase3_decision_receiver.h"

#include "memory_region.h"
#include "v34_constellation.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

static bool detection_is_clear(
	const struct v34_phase3_s_detection *detection)
{
	return detection->clockwise_quarter_turn_offset == 0u &&
	       detection->s_errors == 0u && detection->s_bar_errors == 0u;
}

static bool j_is_initial(
	const struct v34_phase3_decision_receiver *receiver)
{
	return receiver->j.received_symbols == 0u &&
	       receiver->j.buffered_decisions == 0u &&
	       receiver->j.qualified_history == 0u &&
	       !receiver->j.complete &&
	       receiver->j.transmitting_role == receiver->transmitting_role;
}

static bool trn_state_is_valid(
	const struct v34_phase3_decision_receiver *receiver)
{
	if (!v34_training_sequence_is_valid(&receiver->trn) ||
	    receiver->trn.role != receiver->transmitting_role ||
	    receiver->trn.constellation != V34_CONSTELLATION_4_POINT)
		return false;
	if (receiver->j.received_symbols == 0u)
		return !receiver->trn.has_last_symbol &&
		       receiver->trn.last_clockwise_quarter_turns == 0u &&
		       receiver->trn.scrambler.history == 0u;
	return receiver->trn.has_last_symbol;
}

bool v34_phase3_decision_receiver_is_valid(
	const struct v34_phase3_decision_receiver *receiver)
{
	if (receiver == NULL ||
	    !v34_station_role_is_valid(receiver->transmitting_role) ||
	    receiver->md_symbols >
		    V34_PHASE3_DECISION_RECEIVER_MAX_MD_SYMBOLS ||
	    (receiver->md_symbols != 0u &&
	     receiver->md_symbols <=
		     V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS) ||
	    !v34_phase3_s_receiver_is_valid(&receiver->s) ||
	    !v34_phase3_pp_sequence_is_valid(&receiver->pp) ||
	    !trn_state_is_valid(receiver) ||
	    !v34_phase3_j_receiver_is_valid(&receiver->j) ||
	    receiver->j.transmitting_role != receiver->transmitting_role)
		return false;
	switch (receiver->stage) {
	case V34_PHASE3_DECISION_RECEIVER_S:
		return !v34_phase3_s_receiver_is_complete(&receiver->s) &&
		       detection_is_clear(&receiver->s_detection) &&
		       receiver->md_symbols_remaining == 0u &&
		       receiver->pp.emitted_symbols == 0u && j_is_initial(receiver);
	case V34_PHASE3_DECISION_RECEIVER_MD:
		return receiver->md_symbols != 0u &&
		       receiver->md_symbols_remaining != 0u &&
		       receiver->md_symbols_remaining <= receiver->md_symbols &&
		       v34_phase3_s_receiver_is_complete(&receiver->s) &&
		       v34_phase3_s_detection_is_valid(&receiver->s_detection) &&
		       receiver->pp.emitted_symbols == 0u && j_is_initial(receiver);
	case V34_PHASE3_DECISION_RECEIVER_REPEATED_S:
		return receiver->md_symbols != 0u &&
		       receiver->md_symbols_remaining == 0u &&
		       !v34_phase3_s_receiver_is_complete(&receiver->s) &&
		       v34_phase3_s_detection_is_valid(&receiver->s_detection) &&
		       receiver->pp.emitted_symbols == 0u && j_is_initial(receiver);
	case V34_PHASE3_DECISION_RECEIVER_PP:
		return receiver->md_symbols_remaining == 0u &&
		       v34_phase3_s_receiver_is_complete(&receiver->s) &&
		       v34_phase3_s_detection_is_valid(&receiver->s_detection) &&
		       !v34_phase3_pp_sequence_is_complete(&receiver->pp) &&
		       j_is_initial(receiver);
	case V34_PHASE3_DECISION_RECEIVER_J:
		return receiver->md_symbols_remaining == 0u &&
		       v34_phase3_s_receiver_is_complete(&receiver->s) &&
		       v34_phase3_s_detection_is_valid(&receiver->s_detection) &&
		       v34_phase3_pp_sequence_is_complete(&receiver->pp) &&
		       !v34_phase3_j_receiver_is_complete(&receiver->j);
	case V34_PHASE3_DECISION_RECEIVER_COMPLETE:
		return receiver->md_symbols_remaining == 0u &&
		       v34_phase3_s_receiver_is_complete(&receiver->s) &&
		       v34_phase3_s_detection_is_valid(&receiver->s_detection) &&
		       v34_phase3_pp_sequence_is_complete(&receiver->pp) &&
		       v34_phase3_j_receiver_is_complete(&receiver->j);
	}
	return false;
}

bool v34_phase3_decision_receiver_is_complete(
	const struct v34_phase3_decision_receiver *receiver)
{
	return v34_phase3_decision_receiver_is_valid(receiver) &&
	       receiver->stage == V34_PHASE3_DECISION_RECEIVER_COMPLETE;
}

bool v34_phase3_decision_handoff_is_valid(
	const struct v34_phase3_decision_handoff *handoff)
{
	return handoff != NULL &&
	       v34_phase3_s_detection_is_valid(&handoff->s) &&
	       v34_phase3_j_detection_is_valid(&handoff->j);
}

enum v34_phase3_decision_receiver_result
v34_phase3_decision_receiver_init_suffix(
	struct v34_phase3_decision_receiver *receiver,
	enum v34_station_role transmitting_role, uint32_t md_symbols,
	uint8_t s_symbols)
{
	struct v34_phase3_decision_receiver initialized;

	if (receiver == NULL ||
	    !v34_station_role_is_valid(transmitting_role) ||
	    md_symbols > V34_PHASE3_DECISION_RECEIVER_MAX_MD_SYMBOLS ||
	    (md_symbols != 0u &&
	     md_symbols <= V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS))
		return V34_PHASE3_DECISION_RECEIVER_INVALID_ARGUMENT;
	memset(&initialized, 0, sizeof(initialized));
	initialized.transmitting_role = transmitting_role;
	initialized.md_symbols = md_symbols;
	initialized.stage = V34_PHASE3_DECISION_RECEIVER_S;
	if (v34_phase3_s_receiver_init_fixed_suffix(
		    &initialized.s, s_symbols) !=
		    V34_PHASE3_S_RECEIVER_OK ||
	    v34_phase3_pp_sequence_init(&initialized.pp) !=
		    V34_PHASE3_PP_SEQUENCE_OK ||
	    v34_training_sequence_init(
		    &initialized.trn, transmitting_role,
		    V34_CONSTELLATION_4_POINT) !=
		    V34_TRAINING_SEQUENCE_OK ||
	    v34_phase3_j_receiver_init(&initialized.j, transmitting_role) !=
		    V34_PHASE3_J_RECEIVER_OK ||
	    !v34_phase3_decision_receiver_is_valid(&initialized))
		return V34_PHASE3_DECISION_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return V34_PHASE3_DECISION_RECEIVER_OK;
}

static bool advance_pp_prefix(
	struct v34_phase3_decision_receiver *receiver, uint8_t symbols)
{
	for (uint8_t symbol = 0; symbol < symbols; ++symbol) {
		struct v34_analytic_sample ignored;

		if (v34_phase3_pp_sequence_next_fixed(
			    &receiver->pp, &ignored) !=
		    V34_PHASE3_PP_SEQUENCE_OK)
			return false;
	}
	return true;
}

static enum v34_phase3_decision_receiver_result receive_s(
	struct v34_phase3_decision_receiver *receiver,
	const struct v34_adaptive_equalizer_observation *observation,
	uint16_t decoder_power_q15,
	struct v34_receive_action *output)
{
	struct v34_fixed_training_decision hard;
	struct v34_phase3_s_adaptation adaptation;
	struct v34_phase3_s_detection detection;
	enum v34_phase3_s_receiver_result result;
	bool repeated = receiver->stage ==
		V34_PHASE3_DECISION_RECEIVER_REPEATED_S;

	if (v34_training_constellation_fixed_slice(
		    V34_CONSTELLATION_4_POINT, observation->decision,
		    &hard) != V34_CONSTELLATION_OK)
		return V34_PHASE3_DECISION_RECEIVER_INVALID_STATE;
	result = v34_phase3_s_receiver_receive_adaptive(
		&receiver->s, hard.quarter_point,
		hard.clockwise_quarter_turns, observation->decision,
		&adaptation);
	if (result == V34_PHASE3_S_RECEIVER_OK) {
		*output = !adaptation.adapt ?
			v34_receive_action_only(V34_RECEIVE_DISCARD) :
			v34_receive_action_commit(
				observation, adaptation.target,
				decoder_power_q15,
				V34_ADAPTIVE_EQUALIZER_CENTER);
		return V34_PHASE3_DECISION_RECEIVER_OK;
	}
	if (result != V34_PHASE3_S_RECEIVER_DETECTED ||
	    !v34_phase3_s_receiver_get_detection(&receiver->s, &detection))
		return V34_PHASE3_DECISION_RECEIVER_INVALID_STATE;
	receiver->s_detection = detection;
	if (!repeated && receiver->md_symbols != 0u) {
		if (receiver->s.post_marker_symbols >= receiver->md_symbols)
			return V34_PHASE3_DECISION_RECEIVER_INVALID_STATE;
		receiver->md_symbols_remaining = receiver->md_symbols -
			receiver->s.post_marker_symbols;
		receiver->stage = V34_PHASE3_DECISION_RECEIVER_MD;
		*output = v34_receive_action_only(V34_RECEIVE_DISCARD);
		return V34_PHASE3_DECISION_RECEIVER_S_DETECTED;
	}
	if (!advance_pp_prefix(receiver, receiver->s.post_marker_symbols))
		return V34_PHASE3_DECISION_RECEIVER_INVALID_STATE;
	receiver->stage = V34_PHASE3_DECISION_RECEIVER_PP;
	*output = v34_receive_action_only(V34_RECEIVE_BEGIN_TRACKING);
	return repeated ? V34_PHASE3_DECISION_RECEIVER_REPEATED_S_DETECTED :
			  V34_PHASE3_DECISION_RECEIVER_S_DETECTED;
}

static enum v34_phase3_decision_receiver_result receive_md(
	struct v34_phase3_decision_receiver *receiver,
	struct v34_receive_action *output)
{
	receiver->md_symbols_remaining--;
	if (receiver->md_symbols_remaining != 0u) {
		*output = v34_receive_action_only(V34_RECEIVE_DISCARD);
		return V34_PHASE3_DECISION_RECEIVER_OK;
	}
	/* The aggregate restarts carrier/timing at the MD boundary. Detect the
	 * bounded suffix that remains after that physical reacquisition. */
	if (v34_phase3_s_receiver_init_fixed_suffix(
		    &receiver->s,
		    V34_PHASE3_S_RECEIVER_MIN_S_SUFFIX_SYMBOLS) !=
		    V34_PHASE3_S_RECEIVER_OK)
		return V34_PHASE3_DECISION_RECEIVER_INVALID_STATE;
	receiver->stage = V34_PHASE3_DECISION_RECEIVER_REPEATED_S;
	*output = v34_receive_action_only(V34_RECEIVE_RESTART_PATH);
	return V34_PHASE3_DECISION_RECEIVER_MD_COMPLETE;
}

static enum v34_phase3_decision_receiver_result receive_pp(
	struct v34_phase3_decision_receiver *receiver,
	const struct v34_adaptive_equalizer_observation *observation,
	uint16_t decoder_power_q15,
	struct v34_receive_action *output)
{
	struct v34_analytic_sample target;

	if (v34_phase3_pp_sequence_next_fixed(&receiver->pp, &target) !=
	    V34_PHASE3_PP_SEQUENCE_OK)
		return V34_PHASE3_DECISION_RECEIVER_INVALID_STATE;
	*output = v34_receive_action_commit(
		observation, target, decoder_power_q15,
		V34_ADAPTIVE_EQUALIZER_FULL);
	if (!v34_phase3_pp_sequence_is_complete(&receiver->pp))
		return V34_PHASE3_DECISION_RECEIVER_OK;
	receiver->stage = V34_PHASE3_DECISION_RECEIVER_J;
	return V34_PHASE3_DECISION_RECEIVER_PP_COMPLETE;
}

static enum v34_phase3_decision_receiver_result receive_j(
	struct v34_phase3_decision_receiver *receiver,
	const struct v34_adaptive_equalizer_observation *observation,
	uint16_t decoder_power_q15,
	struct v34_receive_action *output)
{
	struct v34_fixed_training_decision hard;
	struct v34_analytic_sample target;
	enum v34_phase3_j_receiver_result result;

	if (v34_training_constellation_fixed_slice(
		    V34_CONSTELLATION_4_POINT, observation->decision,
		    &hard) != V34_CONSTELLATION_OK)
		return V34_PHASE3_DECISION_RECEIVER_INVALID_STATE;
	if (receiver->j.received_symbols < V34_TRAINING_SEQUENCE_MIN_SYMBOLS) {
		uint8_t expected_quarter;
		uint8_t expected_rotation;

		if (v34_training_sequence_next_fixed(
			    &receiver->trn, &target, &expected_quarter,
			    &expected_rotation) !=
		    V34_TRAINING_SEQUENCE_OK)
			return V34_PHASE3_DECISION_RECEIVER_INVALID_STATE;
	} else {
		target = hard.target;
	}
	result = v34_phase3_j_receiver_receive(
		&receiver->j, hard.quarter_point,
		hard.clockwise_quarter_turns);
	if (result == V34_PHASE3_J_RECEIVER_OUT_OF_RANGE)
		return V34_PHASE3_DECISION_RECEIVER_OUT_OF_RANGE;
	if (result != V34_PHASE3_J_RECEIVER_OK &&
	    result != V34_PHASE3_J_RECEIVER_DETECTED)
		return V34_PHASE3_DECISION_RECEIVER_INVALID_STATE;
	*output = v34_receive_action_commit(
		observation, target, decoder_power_q15,
		V34_ADAPTIVE_EQUALIZER_FULL);
	if (result == V34_PHASE3_J_RECEIVER_OK)
		return V34_PHASE3_DECISION_RECEIVER_OK;
	receiver->stage = V34_PHASE3_DECISION_RECEIVER_COMPLETE;
	return V34_PHASE3_DECISION_RECEIVER_J_DETECTED;
}

static bool result_commits(
	enum v34_phase3_decision_receiver_result result)
{
	return result == V34_PHASE3_DECISION_RECEIVER_OK ||
	       result == V34_PHASE3_DECISION_RECEIVER_S_DETECTED ||
	       result == V34_PHASE3_DECISION_RECEIVER_MD_COMPLETE ||
	       result ==
		       V34_PHASE3_DECISION_RECEIVER_REPEATED_S_DETECTED ||
	       result == V34_PHASE3_DECISION_RECEIVER_PP_COMPLETE ||
	       result == V34_PHASE3_DECISION_RECEIVER_J_DETECTED;
}

enum v34_phase3_decision_receiver_result
v34_phase3_decision_receiver_observe(
	struct v34_phase3_decision_receiver *receiver,
	const struct v34_adaptive_equalizer_observation *observation,
	uint16_t decoder_power_q15,
	struct v34_receive_action *output)
{
	const struct memory_region regions[] = {
		{ receiver, receiver == NULL ? 0 : sizeof(*receiver) },
		{ observation, sizeof(*observation) },
		{ output, sizeof(*output) },
	};
	struct v34_phase3_decision_receiver next;
	struct v34_receive_action generated;
	enum v34_phase3_decision_receiver_result result;

	if (observation == NULL || output == NULL ||
	    memory_region_set_overlaps(regions, sizeof regions / sizeof *regions))
		return V34_PHASE3_DECISION_RECEIVER_INVALID_ARGUMENT;
	if (!v34_phase3_decision_receiver_is_valid(receiver))
		return V34_PHASE3_DECISION_RECEIVER_INVALID_STATE;
	if (!v34_adaptive_equalizer_observation_is_valid(observation) ||
	    decoder_power_q15 == 0u ||
	    decoder_power_q15 > INT16_MAX)
		return V34_PHASE3_DECISION_RECEIVER_INVALID_ARGUMENT;
	if (receiver->stage == V34_PHASE3_DECISION_RECEIVER_COMPLETE)
		return V34_PHASE3_DECISION_RECEIVER_ALREADY_COMPLETE;
	next = *receiver;
	generated = (struct v34_receive_action){ 0 };
	switch (next.stage) {
	case V34_PHASE3_DECISION_RECEIVER_S:
	case V34_PHASE3_DECISION_RECEIVER_REPEATED_S:
		result = receive_s(
			&next, observation, decoder_power_q15, &generated);
		break;
	case V34_PHASE3_DECISION_RECEIVER_MD:
		result = receive_md(&next, &generated);
		break;
	case V34_PHASE3_DECISION_RECEIVER_PP:
		result = receive_pp(
			&next, observation, decoder_power_q15, &generated);
		break;
	case V34_PHASE3_DECISION_RECEIVER_J:
		result = receive_j(
			&next, observation, decoder_power_q15, &generated);
		break;
	case V34_PHASE3_DECISION_RECEIVER_COMPLETE:
		return V34_PHASE3_DECISION_RECEIVER_ALREADY_COMPLETE;
	default:
		return V34_PHASE3_DECISION_RECEIVER_INVALID_STATE;
	}
	if (!result_commits(result))
		return result;
	if (!v34_phase3_decision_receiver_is_valid(&next) ||
	    !v34_receive_action_is_valid(
		    &generated, observation, decoder_power_q15))
		return V34_PHASE3_DECISION_RECEIVER_INVALID_STATE;
	*receiver = next;
	*output = generated;
	return result;
}

enum v34_phase3_decision_receiver_result
v34_phase3_decision_receiver_handoff(
	const struct v34_phase3_decision_receiver *receiver,
	struct v34_phase3_decision_handoff *handoff)
{
	struct v34_phase3_decision_handoff generated;

	if (handoff == NULL ||
	    memory_regions_overlap(
		    receiver, receiver == NULL ? 0 : sizeof(*receiver),
		    handoff, sizeof(*handoff)))
		return V34_PHASE3_DECISION_RECEIVER_INVALID_ARGUMENT;
	if (!v34_phase3_decision_receiver_is_valid(receiver))
		return V34_PHASE3_DECISION_RECEIVER_INVALID_STATE;
	if (!v34_phase3_decision_receiver_is_complete(receiver))
		return V34_PHASE3_DECISION_RECEIVER_INVALID_STATE;
	generated.s = receiver->s_detection;
	if (!v34_phase3_j_receiver_get_detection(
		    &receiver->j, &generated.j) ||
	    !v34_phase3_decision_handoff_is_valid(&generated))
		return V34_PHASE3_DECISION_RECEIVER_INVALID_STATE;
	*handoff = generated;
	return V34_PHASE3_DECISION_RECEIVER_OK;
}
