// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase4_e_receiver.h"

#include <stddef.h>

static uint8_t bits_per_symbol(
	enum v34_constellation_size constellation)
{
	return constellation == V34_CONSTELLATION_16_POINT ? 4u :
								    2u;
}

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
	return receiver->searching ||
	       receiver->received_bits %
		       bits_per_symbol(receiver->constellation) ==
		       0;
}

bool v34_phase4_e_receiver_is_complete(
	const struct v34_phase4_e_receiver *receiver)
{
	return v34_phase4_e_receiver_is_valid(receiver) &&
	       receiver->received_bits == V34_PHASE4_E_BITS;
}

enum v34_phase4_e_receiver_result
v34_phase4_e_receiver_init_from_mp(
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

enum v34_phase4_e_receiver_result
v34_phase4_e_receiver_search_from_mp(
	struct v34_phase4_e_receiver *receiver,
	const struct v34_phase4_mp_receiver *mp_receiver)
{
	struct v34_phase4_e_receiver initialized;
	enum v34_phase4_e_receiver_result result =
		v34_phase4_e_receiver_init_from_mp(&initialized, mp_receiver);

	if (receiver == NULL || result != V34_PHASE4_E_RECEIVER_OK)
		return V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT;
	initialized.searching = true;
	if (!v34_phase4_e_receiver_is_valid(&initialized))
		return V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return V34_PHASE4_E_RECEIVER_OK;
}

static uint8_t extend_one_run(uint8_t current, uint8_t dibit)
{
	if (dibit == 3u) {
		unsigned int extended = (unsigned int)current + 2u;

		return extended < V34_PHASE4_E_BITS ?
			       (uint8_t)extended : V34_PHASE4_E_BITS;
	}
	return dibit == 1u ? 1u : 0u;
}

static enum v34_phase4_e_receiver_result search_decision(
	struct v34_phase4_e_receiver *receiver,
	uint8_t quarter_point, uint8_t clockwise_quarter_turns)
{
	struct v34_phase4_e_receiver next = *receiver;
	uint8_t dibit;

	if (v34_descramble_differential_dibit(
		    &next.descrambler,
		    next.previous_clockwise_quarter_turns,
		    clockwise_quarter_turns, &dibit) != V34_FRAMING_OK)
		return V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT;
	next.received_bits = extend_one_run(next.received_bits, dibit);
	if (next.constellation == V34_CONSTELLATION_16_POINT) {
		if (v34_descramble_mapped_dibit(
			    &next.descrambler, quarter_point, &dibit) !=
		    V34_FRAMING_OK)
			return V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT;
		next.received_bits = extend_one_run(next.received_bits, dibit);
	}
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
	struct v34_phase4_e_receiver next;
	struct qam_point checked_point;
	uint8_t dibit;

	if (!v34_phase4_e_receiver_is_valid(receiver) ||
	    v34_training_constellation_point(
		    receiver->constellation, quarter_point,
		    clockwise_quarter_turns, &checked_point) !=
		    V34_CONSTELLATION_OK)
		return V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT;
	if (v34_phase4_e_receiver_is_complete(receiver))
		return V34_PHASE4_E_RECEIVER_COMPLETE;
	if (receiver->searching)
		return search_decision(
			receiver, quarter_point, clockwise_quarter_turns);
	next = *receiver;
	if (v34_descramble_differential_dibit(
		    &next.descrambler,
		    next.previous_clockwise_quarter_turns,
		    clockwise_quarter_turns, &dibit) !=
		    V34_FRAMING_OK ||
	    dibit != 3u)
		return V34_PHASE4_E_RECEIVER_SEQUENCE_MISMATCH;
	if (next.constellation == V34_CONSTELLATION_16_POINT &&
	    (v34_descramble_mapped_dibit(
		     &next.descrambler, quarter_point, &dibit) !=
		     V34_FRAMING_OK ||
	     dibit != 3u))
		return V34_PHASE4_E_RECEIVER_SEQUENCE_MISMATCH;
	next.previous_clockwise_quarter_turns = clockwise_quarter_turns;
	next.received_bits = (uint8_t)(
		next.received_bits + bits_per_symbol(next.constellation));
	if (!v34_phase4_e_receiver_is_valid(&next))
		return V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
	return v34_phase4_e_receiver_is_complete(receiver) ?
		       V34_PHASE4_E_RECEIVER_COMPLETE :
		       V34_PHASE4_E_RECEIVER_OK;
}

enum v34_phase4_e_receiver_result
v34_phase4_e_receive_point(
	struct v34_phase4_e_receiver *receiver,
	const struct qam_point *point)
{
	uint8_t quarter_point;
	uint8_t clockwise_quarter_turns;

	if (!v34_phase4_e_receiver_is_valid(receiver) ||
	    v34_training_constellation_slice(
		    receiver->constellation, point, &quarter_point,
		    &clockwise_quarter_turns) !=
		    V34_CONSTELLATION_OK)
		return V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT;
	return v34_phase4_e_receive_decision(
		receiver, quarter_point, clockwise_quarter_turns);
}

const char *v34_phase4_e_receiver_result_name(
	enum v34_phase4_e_receiver_result result)
{
	switch (result) {
	case V34_PHASE4_E_RECEIVER_OK:
		return "ok";
	case V34_PHASE4_E_RECEIVER_COMPLETE:
		return "complete";
	case V34_PHASE4_E_RECEIVER_SEQUENCE_MISMATCH:
		return "sequence mismatch";
	case V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
