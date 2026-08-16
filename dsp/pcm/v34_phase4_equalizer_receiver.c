// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase4_equalizer_receiver.h"
#include "v34_constellation.h"

#include <math.h>
#include <stddef.h>

static bool stage_is_valid(
	enum v34_phase4_equalizer_receiver_stage stage)
{
	return stage >= V34_PHASE4_EQUALIZER_RECEIVER_STAGE_ANSWER_S &&
	       stage <= V34_PHASE4_EQUALIZER_RECEIVER_STAGE_COMPLETE;
}

static bool answer_s_state_is_valid(
	const struct v34_phase4_equalizer_receiver *receiver)
{
	bool complete;

	if (!v34_phase3_s_receiver_is_valid(&receiver->answer_s))
		return false;
	complete = v34_phase3_s_receiver_is_complete(&receiver->answer_s);
	if (receiver->mp_receiver.transmitting_role == V34_ANSWERING_STATION)
		return complete ==
		       (receiver->stage !=
			V34_PHASE4_EQUALIZER_RECEIVER_STAGE_ANSWER_S);
	return receiver->stage !=
		       V34_PHASE4_EQUALIZER_RECEIVER_STAGE_ANSWER_S &&
	       !complete && receiver->answer_s.buffered_decisions == 0;
}

static bool trn_state_is_valid(
	const struct v34_phase4_equalizer_receiver *receiver)
{
	bool answering_peer =
		receiver->mp_receiver.transmitting_role == V34_ANSWERING_STATION;
	uint16_t expected_errors;

	if (!v34_phase4_trn_is_valid(&receiver->expected_trn) ||
	    receiver->expected_trn.role !=
		    receiver->mp_receiver.transmitting_role ||
	    receiver->expected_trn.constellation !=
		    receiver->mp_receiver.constellation ||
	    receiver->trn_symbols > V34_PHASE4_TRN_MIN_SYMBOLS ||
	    !isfinite(receiver->trn_squared_error) ||
	    receiver->trn_squared_error < 0.0 ||
	    receiver->expected_trn.has_last_symbol !=
		    (receiver->trn_symbols != 0))
		return false;
	if (!answering_peer)
		return receiver->stage !=
			       V34_PHASE4_EQUALIZER_RECEIVER_STAGE_TRN &&
		       receiver->trn_symbols == 0 &&
		       receiver->trn_error_symbols == 0 &&
		       receiver->trn_squared_error == 0.0;
	if (receiver->stage ==
	    V34_PHASE4_EQUALIZER_RECEIVER_STAGE_ANSWER_S)
		return receiver->trn_symbols == 0 &&
		       receiver->trn_error_symbols == 0 &&
		       receiver->trn_squared_error == 0.0;
	if (receiver->trn_symbols < receiver->answer_s.post_marker_symbols)
		return false;
	expected_errors = receiver->trn_symbols -
		receiver->answer_s.post_marker_symbols;
	if (receiver->trn_error_symbols != expected_errors)
		return false;
	if (receiver->stage == V34_PHASE4_EQUALIZER_RECEIVER_STAGE_TRN)
		return receiver->trn_symbols < V34_PHASE4_TRN_MIN_SYMBOLS;
	return receiver->trn_symbols == V34_PHASE4_TRN_MIN_SYMBOLS;
}

bool v34_phase4_equalizer_receiver_is_valid(
	const struct v34_phase4_equalizer_receiver *receiver)
{
	if (receiver == NULL || !qam_equalizer_is_valid(&receiver->equalizer) ||
	    receiver->equalizer.can_adapt || !isfinite(receiver->adaptation) ||
	    receiver->adaptation < 0.0 || receiver->adaptation > 1.0 ||
	    !stage_is_valid(receiver->stage) ||
	    !v34_phase4_mp_receiver_is_valid(&receiver->mp_receiver) ||
	    !answer_s_state_is_valid(receiver) ||
	    !trn_state_is_valid(receiver) ||
	    (receiver->pending_record_is_valid && !receiver->record_pending) ||
	    (receiver->stage != V34_PHASE4_EQUALIZER_RECEIVER_STAGE_MP &&
	     receiver->record_pending))
		return false;
	if (receiver->stage ==
		    V34_PHASE4_EQUALIZER_RECEIVER_STAGE_ANSWER_S ||
	    receiver->stage == V34_PHASE4_EQUALIZER_RECEIVER_STAGE_TRN ||
	    receiver->stage == V34_PHASE4_EQUALIZER_RECEIVER_STAGE_MP)
		return true;
	if (!v34_phase4_e_receiver_is_valid(&receiver->e_receiver))
		return false;
	if (receiver->e_receiver.transmitting_role !=
		    receiver->mp_receiver.transmitting_role ||
	    receiver->e_receiver.constellation !=
		    receiver->mp_receiver.constellation)
		return false;
	return (receiver->stage ==
		V34_PHASE4_EQUALIZER_RECEIVER_STAGE_COMPLETE) ==
	       v34_phase4_e_receiver_is_complete(&receiver->e_receiver);
}

enum v34_phase4_equalizer_receiver_result
v34_phase4_equalizer_receiver_handoff(
	const struct v34_phase4_equalizer_receiver *receiver,
	struct v34_equalizer_handoff *handoff)
{
	struct v34_equalizer_handoff generated;

	if (!v34_phase4_equalizer_receiver_is_valid(receiver) ||
	    handoff == NULL)
		return V34_PHASE4_EQUALIZER_RECEIVER_INVALID_ARGUMENT;
	if (receiver->stage !=
	    V34_PHASE4_EQUALIZER_RECEIVER_STAGE_COMPLETE)
		return V34_PHASE4_EQUALIZER_RECEIVER_INVALID_STATE;
	generated.equalizer = receiver->equalizer;
	if (!v34_equalizer_handoff_is_valid(&generated))
		return V34_PHASE4_EQUALIZER_RECEIVER_INVALID_STATE;
	*handoff = generated;
	return V34_PHASE4_EQUALIZER_RECEIVER_OK;
}

enum v34_phase4_equalizer_receiver_result
v34_phase4_equalizer_receiver_init(
	struct v34_phase4_equalizer_receiver *receiver,
	const struct qam_equalizer *equalizer,
	enum v34_station_role transmitting_role,
	enum v34_constellation_size constellation, double adaptation)
{
	struct v34_phase4_equalizer_receiver initialized = {
		.adaptation = adaptation,
		.stage = transmitting_role == V34_ANSWERING_STATION ?
				 V34_PHASE4_EQUALIZER_RECEIVER_STAGE_ANSWER_S :
				 V34_PHASE4_EQUALIZER_RECEIVER_STAGE_MP,
	};

	if (receiver == NULL || !qam_equalizer_is_valid(equalizer) ||
	    !isfinite(adaptation) || adaptation < 0.0 || adaptation > 1.0 ||
	    v34_phase3_s_receiver_init(&initialized.answer_s) !=
		    V34_PHASE3_S_RECEIVER_OK ||
	    v34_phase4_trn_init(
		    &initialized.expected_trn, transmitting_role,
		    constellation) != V34_PHASE4_TRN_OK ||
	    v34_phase4_mp_receiver_init(
		    &initialized.mp_receiver, transmitting_role,
		    constellation) != V34_PHASE4_MP_RECEIVER_OK)
		return V34_PHASE4_EQUALIZER_RECEIVER_INVALID_ARGUMENT;
	initialized.equalizer = *equalizer;
	initialized.equalizer.can_adapt = false;
	if (!v34_phase4_equalizer_receiver_is_valid(&initialized))
		return V34_PHASE4_EQUALIZER_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return V34_PHASE4_EQUALIZER_RECEIVER_OK;
}

enum v34_phase4_equalizer_receiver_result
v34_phase4_equalizer_receiver_continue_mp(
	struct v34_phase4_equalizer_receiver *receiver)
{
	struct v34_phase4_equalizer_receiver next;

	if (!v34_phase4_equalizer_receiver_is_valid(receiver))
		return V34_PHASE4_EQUALIZER_RECEIVER_INVALID_ARGUMENT;
	if (receiver->stage != V34_PHASE4_EQUALIZER_RECEIVER_STAGE_MP ||
	    !receiver->record_pending)
		return V34_PHASE4_EQUALIZER_RECEIVER_INVALID_STATE;
	next = *receiver;
	next.record_pending = false;
	next.pending_record_is_valid = false;
	if (!v34_phase4_equalizer_receiver_is_valid(&next))
		return V34_PHASE4_EQUALIZER_RECEIVER_INVALID_STATE;
	*receiver = next;
	return V34_PHASE4_EQUALIZER_RECEIVER_OK;
}

enum v34_phase4_equalizer_receiver_result
v34_phase4_equalizer_receiver_start_e(
	struct v34_phase4_equalizer_receiver *receiver)
{
	struct v34_phase4_equalizer_receiver next;

	if (!v34_phase4_equalizer_receiver_is_valid(receiver))
		return V34_PHASE4_EQUALIZER_RECEIVER_INVALID_ARGUMENT;
	if (receiver->stage != V34_PHASE4_EQUALIZER_RECEIVER_STAGE_MP ||
	    !receiver->record_pending || !receiver->pending_record_is_valid)
		return V34_PHASE4_EQUALIZER_RECEIVER_INVALID_STATE;
	next = *receiver;
	if (v34_phase4_e_receiver_init_from_mp(
		    &next.e_receiver, &next.mp_receiver) !=
	    V34_PHASE4_E_RECEIVER_OK)
		return V34_PHASE4_EQUALIZER_RECEIVER_INVALID_STATE;
	next.stage = V34_PHASE4_EQUALIZER_RECEIVER_STAGE_E;
	next.record_pending = false;
	next.pending_record_is_valid = false;
	if (!v34_phase4_equalizer_receiver_is_valid(&next))
		return V34_PHASE4_EQUALIZER_RECEIVER_INVALID_STATE;
	*receiver = next;
	return V34_PHASE4_EQUALIZER_RECEIVER_OK;
}

enum v34_phase4_equalizer_receiver_result
v34_phase4_equalizer_receiver_expect_e(
	struct v34_phase4_equalizer_receiver *receiver)
{
	struct v34_phase4_equalizer_receiver next;

	if (!v34_phase4_equalizer_receiver_is_valid(receiver))
		return V34_PHASE4_EQUALIZER_RECEIVER_INVALID_ARGUMENT;
	if (receiver->stage != V34_PHASE4_EQUALIZER_RECEIVER_STAGE_MP ||
	    !receiver->record_pending || !receiver->pending_record_is_valid)
		return V34_PHASE4_EQUALIZER_RECEIVER_INVALID_STATE;
	next = *receiver;
	if (v34_phase4_e_receiver_search_from_mp(
		    &next.e_receiver, &next.mp_receiver) !=
	    V34_PHASE4_E_RECEIVER_OK)
		return V34_PHASE4_EQUALIZER_RECEIVER_INVALID_STATE;
	next.stage = V34_PHASE4_EQUALIZER_RECEIVER_STAGE_E;
	next.record_pending = false;
	next.pending_record_is_valid = false;
	if (!v34_phase4_equalizer_receiver_is_valid(&next))
		return V34_PHASE4_EQUALIZER_RECEIVER_INVALID_STATE;
	*receiver = next;
	return V34_PHASE4_EQUALIZER_RECEIVER_OK;
}

static enum v34_phase4_equalizer_receiver_result receive_mp(
	struct v34_phase4_equalizer_receiver *receiver,
	uint8_t quarter_point, uint8_t clockwise_quarter_turns,
	struct v34_mp_receive_event *event)
{
	struct v34_mp_receive_event emitted = { 0 };

	if (v34_phase4_mp_receive_decision(
		    &receiver->mp_receiver, quarter_point,
		    clockwise_quarter_turns, &emitted) !=
	    V34_PHASE4_MP_RECEIVER_OK)
		return V34_PHASE4_EQUALIZER_RECEIVER_NUMERIC_FAILURE;
	if (emitted.type == V34_MP_RECEIVE_NO_EVENT)
		return V34_PHASE4_EQUALIZER_RECEIVER_SYMBOL;
	receiver->record_pending = true;
	receiver->pending_record_is_valid =
		(emitted.type == V34_MP_RECEIVE_TYPE0 ||
		 emitted.type == V34_MP_RECEIVE_TYPE1) &&
		emitted.frame_result == V34_MP_OK;
	*event = emitted;
	return V34_PHASE4_EQUALIZER_RECEIVER_RECORD_READY;
}

static enum v34_phase4_equalizer_receiver_result receive_answer_s(
	struct v34_phase4_equalizer_receiver *receiver,
	uint8_t quarter_point, uint8_t clockwise_quarter_turns)
{
	enum v34_phase3_s_receiver_result result =
		v34_phase3_s_receiver_receive(
			&receiver->answer_s, quarter_point,
			clockwise_quarter_turns);

	if (result == V34_PHASE3_S_RECEIVER_DETECTED) {
		struct qam_point ignored_point;
		uint8_t ignored_quarter_point;
		uint8_t ignored_quarter_turns;

		for (uint8_t symbol = 0;
		     symbol < receiver->answer_s.post_marker_symbols; ++symbol)
			if (v34_phase4_trn_next(
				    &receiver->expected_trn, &ignored_point,
				    &ignored_quarter_point,
				    &ignored_quarter_turns) != V34_PHASE4_TRN_OK)
				return V34_PHASE4_EQUALIZER_RECEIVER_NUMERIC_FAILURE;
		receiver->trn_symbols =
			receiver->answer_s.post_marker_symbols;
		receiver->stage = V34_PHASE4_EQUALIZER_RECEIVER_STAGE_TRN;
		return V34_PHASE4_EQUALIZER_RECEIVER_S_DETECTED;
	}
	if (result != V34_PHASE3_S_RECEIVER_OK)
		return V34_PHASE4_EQUALIZER_RECEIVER_NUMERIC_FAILURE;
	return V34_PHASE4_EQUALIZER_RECEIVER_SYMBOL;
}

static enum v34_phase4_equalizer_receiver_result receive_trn(
	struct v34_phase4_equalizer_receiver *receiver,
	struct qam_equalizer_observation *observation)
{
	struct qam_point target;
	uint8_t quarter_point;
	uint8_t clockwise_quarter_turns;

	if (v34_phase4_trn_next(
		    &receiver->expected_trn, &target, &quarter_point,
		    &clockwise_quarter_turns) != V34_PHASE4_TRN_OK)
		return V34_PHASE4_EQUALIZER_RECEIVER_NUMERIC_FAILURE;
	receiver->trn_squared_error +=
		(observation->output.in_phase - target.in_phase) *
			(observation->output.in_phase - target.in_phase) +
		(observation->output.quadrature - target.quadrature) *
			(observation->output.quadrature - target.quadrature);
	receiver->trn_error_symbols++;
	if (!isfinite(receiver->trn_squared_error) ||
	    qam_equalizer_adapt_observation(
		    &receiver->equalizer, observation, &target,
		    receiver->adaptation) != QAM_EQUALIZER_OK)
		return V34_PHASE4_EQUALIZER_RECEIVER_NUMERIC_FAILURE;
	receiver->trn_symbols++;
	if (receiver->trn_symbols != V34_PHASE4_TRN_MIN_SYMBOLS)
		return V34_PHASE4_EQUALIZER_RECEIVER_SYMBOL;
	/* TRN and MP use one continuous scrambler sequence.  The known TRN input
	 * gives us its exact state; the final TRN point initializes differential
	 * decoding as required at the MP boundary. */
	receiver->mp_receiver.descrambler = receiver->expected_trn.scrambler;
	receiver->mp_receiver.previous_clockwise_quarter_turns =
		clockwise_quarter_turns;
	receiver->mp_receiver.has_previous_symbol = true;
	if (!v34_phase4_mp_receiver_is_valid(&receiver->mp_receiver))
		return V34_PHASE4_EQUALIZER_RECEIVER_NUMERIC_FAILURE;
	receiver->stage = V34_PHASE4_EQUALIZER_RECEIVER_STAGE_MP;
	return V34_PHASE4_EQUALIZER_RECEIVER_SYMBOL;
}

static enum v34_phase4_equalizer_receiver_result receive_e(
	struct v34_phase4_equalizer_receiver *receiver,
	uint8_t quarter_point, uint8_t clockwise_quarter_turns)
{
	enum v34_phase4_e_receiver_result result =
		v34_phase4_e_receive_decision(
			&receiver->e_receiver, quarter_point,
			clockwise_quarter_turns);

	if (result == V34_PHASE4_E_RECEIVER_SEQUENCE_MISMATCH)
		return V34_PHASE4_EQUALIZER_RECEIVER_SEQUENCE_MISMATCH;
	if (result == V34_PHASE4_E_RECEIVER_COMPLETE) {
		receiver->stage =
			V34_PHASE4_EQUALIZER_RECEIVER_STAGE_COMPLETE;
		return V34_PHASE4_EQUALIZER_RECEIVER_E_COMPLETE;
	}
	if (result != V34_PHASE4_E_RECEIVER_OK)
		return V34_PHASE4_EQUALIZER_RECEIVER_NUMERIC_FAILURE;
	return V34_PHASE4_EQUALIZER_RECEIVER_SYMBOL;
}

enum v34_phase4_equalizer_receiver_result
v34_phase4_equalizer_receive_point(
	struct v34_phase4_equalizer_receiver *receiver,
	const struct qam_point *received,
	struct v34_mp_receive_event *event)
{
	struct v34_phase4_equalizer_receiver next;
	struct qam_equalizer_observation observation = { 0 };
	struct v34_mp_receive_event emitted = { 0 };
	struct qam_point target;
	enum v34_constellation_size constellation;
	enum v34_phase4_equalizer_receiver_result result;
	uint8_t quarter_point;
	uint8_t clockwise_quarter_turns;
	bool ready;

	if (!v34_phase4_equalizer_receiver_is_valid(receiver) ||
	    !point_is_finite(received) || event == NULL)
		return V34_PHASE4_EQUALIZER_RECEIVER_INVALID_ARGUMENT;
	if (receiver->record_pending)
		return V34_PHASE4_EQUALIZER_RECEIVER_INVALID_STATE;
	if (receiver->stage ==
	    V34_PHASE4_EQUALIZER_RECEIVER_STAGE_COMPLETE)
		return V34_PHASE4_EQUALIZER_RECEIVER_COMPLETE;
	next = *receiver;
	if (qam_equalizer_observe(
		    &next.equalizer, received, &observation, &ready) !=
	    QAM_EQUALIZER_OK)
		return V34_PHASE4_EQUALIZER_RECEIVER_NUMERIC_FAILURE;
	if (!ready) {
		if (!v34_phase4_equalizer_receiver_is_valid(&next))
			return V34_PHASE4_EQUALIZER_RECEIVER_NUMERIC_FAILURE;
		*receiver = next;
		return V34_PHASE4_EQUALIZER_RECEIVER_WARMING;
	}
	if (next.stage == V34_PHASE4_EQUALIZER_RECEIVER_STAGE_TRN) {
		result = receive_trn(&next, &observation);
		if (result == V34_PHASE4_EQUALIZER_RECEIVER_NUMERIC_FAILURE ||
		    !v34_phase4_equalizer_receiver_is_valid(&next))
			return V34_PHASE4_EQUALIZER_RECEIVER_NUMERIC_FAILURE;
		*receiver = next;
		return result;
	}
	constellation = next.stage ==
				V34_PHASE4_EQUALIZER_RECEIVER_STAGE_ANSWER_S ?
				V34_CONSTELLATION_4_POINT :
			next.stage ==
				V34_PHASE4_EQUALIZER_RECEIVER_STAGE_MP ?
				next.mp_receiver.constellation :
				next.e_receiver.constellation;
	if (v34_training_constellation_slice(
		    constellation, &observation.output, &quarter_point,
		    &clockwise_quarter_turns) != V34_CONSTELLATION_OK ||
	    v34_training_constellation_point(
		    constellation, quarter_point, clockwise_quarter_turns,
		    &target) != V34_CONSTELLATION_OK)
		return V34_PHASE4_EQUALIZER_RECEIVER_NUMERIC_FAILURE;
	/* Before S/S-bar, the input may still be the required Phase 3 drain.
	 * A nearest-point decision in that interval is not a training symbol. */
	if (next.stage != V34_PHASE4_EQUALIZER_RECEIVER_STAGE_ANSWER_S &&
	    qam_equalizer_adapt_observation(
		    &next.equalizer, &observation, &target,
		    next.adaptation) != QAM_EQUALIZER_OK)
		return V34_PHASE4_EQUALIZER_RECEIVER_NUMERIC_FAILURE;
	if (next.stage == V34_PHASE4_EQUALIZER_RECEIVER_STAGE_ANSWER_S)
		result = receive_answer_s(
			&next, quarter_point, clockwise_quarter_turns);
	else if (next.stage == V34_PHASE4_EQUALIZER_RECEIVER_STAGE_MP)
		result = receive_mp(
			&next, quarter_point, clockwise_quarter_turns, &emitted);
	else
		result = receive_e(
			&next, quarter_point, clockwise_quarter_turns);
	if (result == V34_PHASE4_EQUALIZER_RECEIVER_NUMERIC_FAILURE ||
	    result == V34_PHASE4_EQUALIZER_RECEIVER_SEQUENCE_MISMATCH)
		return result;
	if (!v34_phase4_equalizer_receiver_is_valid(&next))
		return V34_PHASE4_EQUALIZER_RECEIVER_NUMERIC_FAILURE;
	*receiver = next;
	if (result == V34_PHASE4_EQUALIZER_RECEIVER_RECORD_READY)
		*event = emitted;
	return result;
}

const char *v34_phase4_equalizer_receiver_result_name(
	enum v34_phase4_equalizer_receiver_result result)
{
	switch (result) {
	case V34_PHASE4_EQUALIZER_RECEIVER_OK:
		return "ok";
	case V34_PHASE4_EQUALIZER_RECEIVER_WARMING:
		return "warming";
	case V34_PHASE4_EQUALIZER_RECEIVER_SYMBOL:
		return "symbol";
	case V34_PHASE4_EQUALIZER_RECEIVER_S_DETECTED:
		return "S detected";
	case V34_PHASE4_EQUALIZER_RECEIVER_RECORD_READY:
		return "record ready";
	case V34_PHASE4_EQUALIZER_RECEIVER_E_COMPLETE:
		return "E complete";
	case V34_PHASE4_EQUALIZER_RECEIVER_COMPLETE:
		return "complete";
	case V34_PHASE4_EQUALIZER_RECEIVER_INVALID_ARGUMENT:
		return "invalid argument";
	case V34_PHASE4_EQUALIZER_RECEIVER_INVALID_STATE:
		return "invalid state";
	case V34_PHASE4_EQUALIZER_RECEIVER_NUMERIC_FAILURE:
		return "numeric failure";
	case V34_PHASE4_EQUALIZER_RECEIVER_SEQUENCE_MISMATCH:
		return "sequence mismatch";
	}
	return "unknown";
}
