// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase4_mp_receiver.h"

#include "memory_region.h"
#include "v34_phase4_e_sequence.h"
#include "v34_training_sequence.h"

#include <stddef.h>

bool v34_phase4_mp_receiver_is_valid(
	const struct v34_phase4_mp_receiver *receiver)
{
	return receiver != NULL &&
	       v34_constellation_size_is_valid(
		       receiver->constellation) &&
	       v34_transmit_scrambler_is_valid(
		       &receiver->descrambler, receiver->transmitting_role) &&
	       v34_mp_receiver_is_valid(&receiver->records) &&
	       receiver->previous_clockwise_quarter_turns <= 3u &&
	       receiver->consecutive_ones <= V34_PHASE4_E_BITS &&
	       (receiver->has_previous_symbol ||
		(receiver->previous_clockwise_quarter_turns == 0u &&
		 receiver->consecutive_ones == 0u));
}

enum v34_phase4_mp_receiver_result
v34_phase4_mp_receiver_init(
	struct v34_phase4_mp_receiver *receiver,
	enum v34_station_role transmitting_role,
	enum v34_constellation_size constellation)
{
	struct v34_phase4_mp_receiver initialized = {
		.transmitting_role = transmitting_role,
		.constellation = constellation,
	};

	if (receiver == NULL ||
	    !v34_constellation_size_is_valid(constellation) ||
	    v34_transmit_scrambler_init(
		    &initialized.descrambler, transmitting_role) !=
		    V34_FRAMING_OK ||
	    v34_mp_receiver_init(&initialized.records) !=
		    V34_MP_OK ||
	    !v34_phase4_mp_receiver_is_valid(&initialized))
		return V34_PHASE4_MP_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return V34_PHASE4_MP_RECEIVER_OK;
}

enum v34_phase4_mp_receiver_result
v34_phase4_mp_receiver_init_from_trn(
	struct v34_phase4_mp_receiver *receiver,
	const struct v34_training_sequence *trn)
{
	struct v34_phase4_mp_receiver initialized = { 0 };

	if (receiver == NULL || trn == NULL ||
	    memory_regions_overlap(
		    receiver, sizeof(*receiver), trn, sizeof(*trn)) ||
	    !v34_training_sequence_is_valid(trn) ||
	    trn->constellation != V34_CONSTELLATION_4_POINT ||
	    !trn->has_last_symbol ||
	    v34_mp_receiver_init(&initialized.records) != V34_MP_OK)
		return V34_PHASE4_MP_RECEIVER_INVALID_ARGUMENT;
	initialized.descrambler = trn->scrambler;
	initialized.transmitting_role = trn->role;
	initialized.constellation = trn->constellation;
	initialized.previous_clockwise_quarter_turns =
		trn->last_clockwise_quarter_turns;
	initialized.has_previous_symbol = true;
	if (!v34_phase4_mp_receiver_is_valid(&initialized))
		return V34_PHASE4_MP_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return V34_PHASE4_MP_RECEIVER_OK;
}

static enum v34_phase4_mp_receiver_result receive_dibit(
	struct v34_mp_receiver *records, uint8_t dibit,
	struct v34_mp_receive_event *event)
{
	struct v34_mp_receive_event first = { 0 };
	struct v34_mp_receive_event second = { 0 };

	if (v34_mp_receive_bit(
		    records, (uint8_t)((dibit >> 1) & 1u), &first) !=
		    V34_MP_OK ||
	    v34_mp_receive_bit(
		    records, (uint8_t)(dibit & 1u), &second) !=
		    V34_MP_OK ||
	    (first.type != V34_MP_RECEIVE_NO_EVENT &&
	     second.type != V34_MP_RECEIVE_NO_EVENT))
		return V34_PHASE4_MP_RECEIVER_INVALID_ARGUMENT;
	*event = first.type != V34_MP_RECEIVE_NO_EVENT ? first :
								 second;
	return V34_PHASE4_MP_RECEIVER_OK;
}

enum v34_phase4_mp_receiver_result
v34_phase4_mp_receive_decision(
	struct v34_phase4_mp_receiver *receiver,
	uint8_t quarter_point, uint8_t clockwise_quarter_turns,
	struct v34_mp_receive_event *event)
{
	struct v34_phase4_mp_receiver next;
	struct v34_mp_receive_event emitted = { 0 };
	uint8_t bits;

	if (!v34_phase4_mp_receiver_is_valid(receiver) ||
	    !v34_training_decision_is_valid(receiver->constellation,
		    quarter_point, clockwise_quarter_turns) ||
	    event == NULL)
		return V34_PHASE4_MP_RECEIVER_INVALID_ARGUMENT;

	next = *receiver;
	if (!next.has_previous_symbol) {
		next.has_previous_symbol = true;
	} else {
		unsigned int count = v34_constellation_bits_per_symbol(
			next.constellation);

		if (v34_descramble_control_symbol(
			    &next.descrambler,
			    next.previous_clockwise_quarter_turns,
			    clockwise_quarter_turns, quarter_point, count,
			    &bits) != V34_FRAMING_OK)
			return V34_PHASE4_MP_RECEIVER_INVALID_ARGUMENT;
		for (; count != 0u; count -= 2u) {
			struct v34_mp_receive_event received = { 0 };
			uint8_t dibit = (uint8_t)((bits >> (count - 2u)) & 3u);

			if (receive_dibit(&next.records, dibit, &received) !=
				    V34_PHASE4_MP_RECEIVER_OK ||
			    (emitted.type != V34_MP_RECEIVE_NO_EVENT &&
			     received.type != V34_MP_RECEIVE_NO_EVENT))
				return V34_PHASE4_MP_RECEIVER_INVALID_ARGUMENT;
			if (received.type != V34_MP_RECEIVE_NO_EVENT)
				emitted = received;
			next.consecutive_ones = v34_phase4_e_extend_one_run(
				next.consecutive_ones, dibit);
		}
	}
	next.previous_clockwise_quarter_turns = clockwise_quarter_turns;
	if (!v34_phase4_mp_receiver_is_valid(&next))
		return V34_PHASE4_MP_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
	*event = emitted;
	return V34_PHASE4_MP_RECEIVER_OK;
}
