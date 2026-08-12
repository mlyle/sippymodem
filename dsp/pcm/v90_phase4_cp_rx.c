// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase4_cp_rx.h"

#include <stddef.h>

bool v90_phase4_cp_rx_is_valid(
	const struct v90_phase4_cp_rx *rx)
{
	return rx != NULL &&
	       v34_constellation_size_is_valid(rx->constellation) &&
	       v34_transmit_scrambler_is_valid(
		       &rx->descrambler, rx->transmitting_role) &&
	       v90_cp_receiver_is_valid(&rx->records) &&
	       rx->previous_clockwise_quarter_turns <= 3u;
}

enum v90_cp_result v90_phase4_cp_rx_init(
	struct v90_phase4_cp_rx *rx,
	enum v34_station_role transmitting_role,
	enum v34_constellation_size constellation)
{
	struct v90_phase4_cp_rx initialized = {
		.transmitting_role = transmitting_role,
		.constellation = constellation,
	};

	if (rx == NULL ||
	    !v34_constellation_size_is_valid(constellation) ||
	    v34_transmit_scrambler_init(
		    &initialized.descrambler, transmitting_role) !=
		    V34_FRAMING_OK ||
	    v90_cp_receiver_init(&initialized.records) != V90_CP_OK ||
	    !v90_phase4_cp_rx_is_valid(&initialized))
		return V90_CP_INVALID_ARGUMENT;
	*rx = initialized;
	return V90_CP_OK;
}

static bool decision_is_valid(
	const struct v90_phase4_cp_rx *rx,
	uint8_t quarter_point, uint8_t clockwise_quarter_turns)
{
	if (clockwise_quarter_turns > 3u)
		return false;
	if (rx->constellation == V34_CONSTELLATION_4_POINT)
		return quarter_point == 0u;
	return quarter_point <= 3u;
}

static enum v90_cp_result receive_dibit(
	struct v90_cp_receiver *records, uint8_t dibit,
	struct v90_cp_receive_event *event)
{
	struct v90_cp_receive_event first = { 0 };
	struct v90_cp_receive_event second = { 0 };

	if (v90_cp_receiver_receive_bit(
		    records, (uint8_t)((dibit >> 1) & 1u), &first) !=
		    V90_CP_OK ||
	    v90_cp_receiver_receive_bit(
		    records, (uint8_t)(dibit & 1u), &second) !=
		    V90_CP_OK ||
	    (first.type != V90_CP_RECEIVE_NO_EVENT &&
	     second.type != V90_CP_RECEIVE_NO_EVENT))
		return V90_CP_INVALID_ARGUMENT;
	*event = first.type != V90_CP_RECEIVE_NO_EVENT ? first : second;
	return V90_CP_OK;
}

enum v90_cp_result v90_phase4_cp_rx_receive_decision(
	struct v90_phase4_cp_rx *rx,
	uint8_t quarter_point, uint8_t clockwise_quarter_turns,
	struct v90_cp_receive_event *event)
{
	struct v90_phase4_cp_rx next;
	struct v90_cp_receive_event differential_event = { 0 };
	struct v90_cp_receive_event mapped_event = { 0 };
	struct v90_cp_receive_event emitted;
	uint8_t dibit;

	if (!v90_phase4_cp_rx_is_valid(rx) ||
	    !decision_is_valid(rx, quarter_point,
			       clockwise_quarter_turns) ||
	    event == NULL)
		return V90_CP_INVALID_ARGUMENT;
	next = *rx;
	if (v34_descramble_differential_dibit(
		    &next.descrambler,
		    next.previous_clockwise_quarter_turns,
		    clockwise_quarter_turns, &dibit) != V34_FRAMING_OK ||
	    receive_dibit(
		    &next.records, dibit, &differential_event) != V90_CP_OK)
		return V90_CP_INVALID_ARGUMENT;
	if (next.constellation == V34_CONSTELLATION_16_POINT) {
		if (v34_descramble_mapped_dibit(
			    &next.descrambler, quarter_point,
			    &dibit) != V34_FRAMING_OK ||
		    receive_dibit(
			    &next.records, dibit, &mapped_event) != V90_CP_OK ||
		    (differential_event.type != V90_CP_RECEIVE_NO_EVENT &&
		     mapped_event.type != V90_CP_RECEIVE_NO_EVENT))
			return V90_CP_INVALID_ARGUMENT;
	}
	emitted = differential_event.type != V90_CP_RECEIVE_NO_EVENT ?
			  differential_event :
			  mapped_event;
	next.previous_clockwise_quarter_turns =
		clockwise_quarter_turns;
	if (!v90_phase4_cp_rx_is_valid(&next))
		return V90_CP_INVALID_ARGUMENT;
	*rx = next;
	*event = emitted;
	return V90_CP_OK;
}

enum v90_cp_result v90_phase4_cp_rx_receive_point(
	struct v90_phase4_cp_rx *rx,
	const struct qam_point *point,
	struct v90_cp_receive_event *event)
{
	uint8_t quarter_point;
	uint8_t clockwise_quarter_turns;

	if (!v90_phase4_cp_rx_is_valid(rx) || event == NULL ||
	    v34_training_constellation_slice(
		    rx->constellation, point, &quarter_point,
		    &clockwise_quarter_turns) != V34_CONSTELLATION_OK)
		return V90_CP_INVALID_ARGUMENT;
	return v90_phase4_cp_rx_receive_decision(
		rx, quarter_point, clockwise_quarter_turns, event);
}
