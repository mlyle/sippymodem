// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase4_e_receiver.h"

#include <stddef.h>

bool v34_phase4_e_receiver_is_valid(
	const struct v34_phase4_e_receiver *receiver)
{
	if (receiver == NULL ||
	    !v34_constellation_size_is_valid(
		    receiver->constellation) ||
	    !v34_transmit_scrambler_is_valid(
		    &receiver->descrambler, receiver->transmitting_role) ||
	    receiver->previous_clockwise_quarter_turns > 3u ||
	    receiver->received_bits > V34_PHASE4_E_BITS)
		return false;
	return true;
}

bool v34_phase4_e_receiver_is_complete(
	const struct v34_phase4_e_receiver *receiver)
{
	return v34_phase4_e_receiver_is_valid(receiver) &&
	       receiver->received_bits == V34_PHASE4_E_BITS;
}

enum v34_phase4_e_receiver_result
v34_phase4_e_receiver_search_from_mp(
	struct v34_phase4_e_receiver *receiver,
	const struct v34_phase4_mp_receiver *mp_receiver)
{
	struct v34_phase4_e_receiver initialized;

	if (receiver == NULL ||
	    !v34_phase4_mp_receiver_is_valid(mp_receiver) ||
	    !mp_receiver->has_previous_symbol)
		return V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT;
	initialized = (struct v34_phase4_e_receiver){
		.descrambler = mp_receiver->descrambler,
		.transmitting_role = mp_receiver->transmitting_role,
		.constellation = mp_receiver->constellation,
		.previous_clockwise_quarter_turns =
			mp_receiver->previous_clockwise_quarter_turns,
	};
	if (!v34_phase4_e_receiver_is_valid(&initialized))
		return V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return V34_PHASE4_E_RECEIVER_OK;
}

static enum v34_phase4_e_receiver_result search_decision(
	struct v34_phase4_e_receiver *receiver,
	uint8_t quarter_point, uint8_t clockwise_quarter_turns)
{
	struct v34_phase4_e_receiver next = *receiver;
	unsigned int count = v34_constellation_bits_per_symbol(next.constellation);
	uint8_t bits;

	if (v34_descramble_control_symbol(
		    &next.descrambler,
		    next.previous_clockwise_quarter_turns,
		    clockwise_quarter_turns, quarter_point, count,
		    &bits) != V34_FRAMING_OK)
		return V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT;

	for (; count != 0u; count -= 2u)
		next.received_bits = v34_phase4_e_extend_one_run(
			next.received_bits,
			(uint8_t)((bits >> (count - 2u)) & 3u));

	next.previous_clockwise_quarter_turns = clockwise_quarter_turns;
	if (!v34_phase4_e_receiver_is_valid(&next))
		return V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
	return v34_phase4_e_receiver_is_complete(receiver) ?
		       V34_PHASE4_E_RECEIVER_COMPLETE :
		       V34_PHASE4_E_RECEIVER_OK;
}

enum v34_phase4_e_receiver_result
v34_phase4_e_receive_decision(
	struct v34_phase4_e_receiver *receiver,
	uint8_t quarter_point, uint8_t clockwise_quarter_turns)
{
	struct qam_point checked_point;

	if (!v34_phase4_e_receiver_is_valid(receiver) ||
	    v34_training_constellation_point(
		    receiver->constellation, quarter_point,
		    clockwise_quarter_turns, &checked_point) !=
		    V34_CONSTELLATION_OK)
		return V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT;
	if (v34_phase4_e_receiver_is_complete(receiver))
		return V34_PHASE4_E_RECEIVER_COMPLETE;
	return search_decision(
		receiver, quarter_point, clockwise_quarter_turns);
}
