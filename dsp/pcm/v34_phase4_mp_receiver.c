// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase4_mp_receiver.h"

#include <stddef.h>

bool sippymodem_v34_phase4_mp_receiver_is_valid(
	const struct sippymodem_v34_phase4_mp_receiver *receiver)
{
	return receiver != NULL &&
	       sippymodem_v34_constellation_size_is_valid(
		       receiver->constellation) &&
	       sippymodem_v34_transmit_scrambler_is_valid(
		       &receiver->descrambler, receiver->transmitting_role) &&
	       sippymodem_v34_mp_receiver_is_valid(&receiver->records) &&
	       receiver->previous_clockwise_quarter_turns <= 3u &&
	       (receiver->has_previous_symbol ||
		receiver->previous_clockwise_quarter_turns == 0u);
}

enum sippymodem_v34_phase4_mp_receiver_result
sippymodem_v34_phase4_mp_receiver_init(
	struct sippymodem_v34_phase4_mp_receiver *receiver,
	enum sippymodem_v34_station_role transmitting_role,
	enum sippymodem_v34_constellation_size constellation)
{
	struct sippymodem_v34_phase4_mp_receiver initialized = {
		.transmitting_role = transmitting_role,
		.constellation = constellation,
	};

	if (receiver == NULL ||
	    !sippymodem_v34_constellation_size_is_valid(constellation) ||
	    sippymodem_v34_transmit_scrambler_init(
		    &initialized.descrambler, transmitting_role) !=
		    SIPPYMODEM_V34_FRAMING_OK ||
	    sippymodem_v34_mp_receiver_init(&initialized.records) !=
		    SIPPYMODEM_V34_MP_OK ||
	    !sippymodem_v34_phase4_mp_receiver_is_valid(&initialized))
		return SIPPYMODEM_V34_PHASE4_MP_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return SIPPYMODEM_V34_PHASE4_MP_RECEIVER_OK;
}

static bool decision_is_valid(
	const struct sippymodem_v34_phase4_mp_receiver *receiver,
	uint8_t quarter_point, uint8_t clockwise_quarter_turns)
{
	if (clockwise_quarter_turns > 3u)
		return false;
	if (receiver->constellation == SIPPYMODEM_V34_CONSTELLATION_4_POINT)
		return quarter_point == 0u;
	return quarter_point <= 3u;
}

static enum sippymodem_v34_phase4_mp_receiver_result receive_dibit(
	struct sippymodem_v34_mp_receiver *records, uint8_t dibit,
	struct sippymodem_v34_mp_receive_event *event)
{
	struct sippymodem_v34_mp_receive_event first = { 0 };
	struct sippymodem_v34_mp_receive_event second = { 0 };

	if (sippymodem_v34_mp_receive_bit(
		    records, (uint8_t)((dibit >> 1) & 1u), &first) !=
		    SIPPYMODEM_V34_MP_OK ||
	    sippymodem_v34_mp_receive_bit(
		    records, (uint8_t)(dibit & 1u), &second) !=
		    SIPPYMODEM_V34_MP_OK ||
	    (first.type != SIPPYMODEM_V34_MP_RECEIVE_NO_EVENT &&
	     second.type != SIPPYMODEM_V34_MP_RECEIVE_NO_EVENT))
		return SIPPYMODEM_V34_PHASE4_MP_RECEIVER_INVALID_ARGUMENT;
	*event = first.type != SIPPYMODEM_V34_MP_RECEIVE_NO_EVENT ? first :
								 second;
	return SIPPYMODEM_V34_PHASE4_MP_RECEIVER_OK;
}

enum sippymodem_v34_phase4_mp_receiver_result
sippymodem_v34_phase4_mp_receive_decision(
	struct sippymodem_v34_phase4_mp_receiver *receiver,
	uint8_t quarter_point, uint8_t clockwise_quarter_turns,
	struct sippymodem_v34_mp_receive_event *event)
{
	struct sippymodem_v34_phase4_mp_receiver next;
	struct sippymodem_v34_mp_receive_event differential_event = { 0 };
	struct sippymodem_v34_mp_receive_event mapped_event = { 0 };
	struct sippymodem_v34_mp_receive_event emitted = { 0 };
	uint8_t dibit;

	if (!sippymodem_v34_phase4_mp_receiver_is_valid(receiver) ||
	    !decision_is_valid(receiver, quarter_point,
			       clockwise_quarter_turns) ||
	    event == NULL)
		return SIPPYMODEM_V34_PHASE4_MP_RECEIVER_INVALID_ARGUMENT;
	next = *receiver;
	if (!next.has_previous_symbol) {
		next.previous_clockwise_quarter_turns =
			clockwise_quarter_turns;
		next.has_previous_symbol = true;
	} else {
		if (sippymodem_v34_descramble_differential_dibit(
			    &next.descrambler,
			    next.previous_clockwise_quarter_turns,
			    clockwise_quarter_turns, &dibit) !=
			    SIPPYMODEM_V34_FRAMING_OK ||
		    receive_dibit(
			    &next.records, dibit, &differential_event) !=
			    SIPPYMODEM_V34_PHASE4_MP_RECEIVER_OK)
			return SIPPYMODEM_V34_PHASE4_MP_RECEIVER_INVALID_ARGUMENT;
		if (next.constellation ==
		    SIPPYMODEM_V34_CONSTELLATION_16_POINT) {
			if (sippymodem_v34_descramble_mapped_dibit(
				    &next.descrambler, quarter_point,
				    &dibit) != SIPPYMODEM_V34_FRAMING_OK ||
			    receive_dibit(
				    &next.records, dibit, &mapped_event) !=
				    SIPPYMODEM_V34_PHASE4_MP_RECEIVER_OK ||
			    (differential_event.type !=
				     SIPPYMODEM_V34_MP_RECEIVE_NO_EVENT &&
			     mapped_event.type !=
				     SIPPYMODEM_V34_MP_RECEIVE_NO_EVENT))
				return SIPPYMODEM_V34_PHASE4_MP_RECEIVER_INVALID_ARGUMENT;
		}
		emitted = differential_event.type !=
				  SIPPYMODEM_V34_MP_RECEIVE_NO_EVENT ?
				  differential_event : mapped_event;
		next.previous_clockwise_quarter_turns =
			clockwise_quarter_turns;
	}
	if (!sippymodem_v34_phase4_mp_receiver_is_valid(&next))
		return SIPPYMODEM_V34_PHASE4_MP_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
	*event = emitted;
	return SIPPYMODEM_V34_PHASE4_MP_RECEIVER_OK;
}

enum sippymodem_v34_phase4_mp_receiver_result
sippymodem_v34_phase4_mp_receive_point(
	struct sippymodem_v34_phase4_mp_receiver *receiver,
	const struct sippymodem_qam_point *point,
	struct sippymodem_v34_mp_receive_event *event)
{
	uint8_t quarter_point;
	uint8_t clockwise_quarter_turns;

	if (!sippymodem_v34_phase4_mp_receiver_is_valid(receiver) ||
	    event == NULL ||
	    sippymodem_v34_training_constellation_slice(
		    receiver->constellation, point, &quarter_point,
		    &clockwise_quarter_turns) !=
		    SIPPYMODEM_V34_CONSTELLATION_OK)
		return SIPPYMODEM_V34_PHASE4_MP_RECEIVER_INVALID_ARGUMENT;
	return sippymodem_v34_phase4_mp_receive_decision(
		receiver, quarter_point, clockwise_quarter_turns, event);
}

const char *sippymodem_v34_phase4_mp_receiver_result_name(
	enum sippymodem_v34_phase4_mp_receiver_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_PHASE4_MP_RECEIVER_OK:
		return "ok";
	case SIPPYMODEM_V34_PHASE4_MP_RECEIVER_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
