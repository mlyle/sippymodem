// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase4_cp_rx.h"

#include "v34_phase4_e_sequence.h"

#include <stddef.h>
#include <string.h>

bool v90_phase4_cp_rx_is_valid(
	const struct v90_phase4_cp_rx *rx)
{
	return rx != NULL &&
	       v34_constellation_size_is_valid(rx->constellation) &&
	       v34_transmit_scrambler_is_valid(
		       &rx->descrambler, rx->transmitting_role) &&
	       v90_cp_receiver_is_valid(&rx->records) &&
	       rx->previous_clockwise_quarter_turns <= 3u &&
	       rx->consecutive_ones <= V34_PHASE4_E_BITS;
}

bool v90_phase4_cp_rx_equal(
	const struct v90_phase4_cp_rx *left,
	const struct v90_phase4_cp_rx *right)
{
	return left != NULL && right != NULL &&
	       left->descrambler.feedback.history == right->descrambler.feedback.history &&
	       left->descrambler.polynomial == right->descrambler.polynomial &&
	       memcmp(left->records.wire.frame, right->records.wire.frame,
		      sizeof(left->records.wire.frame)) == 0 &&
	       left->records.wire.frame_bit_count == right->records.wire.frame_bit_count &&
	       left->records.wire.bit_index == right->records.wire.bit_index &&
	       left->records.wire.sync_one_count == right->records.wire.sync_one_count &&
	       left->records.wire.stage == right->records.wire.stage &&
	       left->transmitting_role == right->transmitting_role &&
	       left->constellation == right->constellation &&
	       left->previous_clockwise_quarter_turns ==
		       right->previous_clockwise_quarter_turns &&
	       left->consecutive_ones == right->consecutive_ones;
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

enum v90_cp_result v90_phase4_cp_rx_receive_decision(
	struct v90_phase4_cp_rx *rx,
	uint8_t quarter_point, uint8_t clockwise_quarter_turns,
	struct v90_cp_receive_event *event)
{
	struct v90_phase4_cp_rx next;
	struct v90_cp_receive_event emitted = { 0 };
	uint8_t bits;
	unsigned count, emitted_dibit = 0;

	if (!v90_phase4_cp_rx_is_valid(rx) ||
	    !v34_training_decision_is_valid(
		    rx->constellation, quarter_point, clockwise_quarter_turns) ||
	    event == NULL)
		return V90_CP_INVALID_ARGUMENT;

	next = *rx;
	count = v34_constellation_bits_per_symbol(next.constellation);

	if (!v34_phase4_receive_symbol(&next.descrambler,
		    &next.previous_clockwise_quarter_turns, &next.consecutive_ones,
		    quarter_point, clockwise_quarter_turns, count, &bits))
		return V90_CP_INVALID_ARGUMENT;
	for (; count != 0u; --count) {
		struct v90_cp_receive_event received;
		unsigned index = count - 1u;
		uint8_t bit = (uint8_t)((bits >> index) & 1u);

		if (v90_cp_receiver_receive_bit(&next.records, bit, &received) != V90_CP_OK)
			return V90_CP_INVALID_ARGUMENT;
		if (received.type == V90_CP_RECEIVE_NO_EVENT)
			continue;
		/* Retain the first event. A noisy symbol can terminate one
		 * rejected candidate in each dibit. */
		if (emitted.type == V90_CP_RECEIVE_NO_EVENT) {
			emitted = received;
			emitted_dibit = index / 2u;
		} else if (emitted_dibit == index / 2u ||
			   emitted.type != V90_CP_RECEIVE_REJECTED ||
			   received.type != V90_CP_RECEIVE_REJECTED) {
			return V90_CP_INVALID_ARGUMENT;
		}
	}
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
