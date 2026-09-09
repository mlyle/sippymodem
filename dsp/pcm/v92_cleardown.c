// SPDX-License-Identifier: BSD-3-Clause

#include "v92_cleardown.h"

#include <stddef.h>

bool v92_cleardown_is_valid(const struct v92_cleardown *state)
{
	if (state == NULL)
		return false;
	if (!state->active)
		return !state->cp_end && !state->acknowledgement_end &&
			!state->round_trip_delay && !state->initiator &&
			!state->peer_requested && !state->acknowledged;
	return (state->initiator || state->peer_requested) &&
		(!state->acknowledged || state->cp_end) &&
		(!state->acknowledgement_end || (state->peer_requested &&
		 state->cp_end && state->acknowledgement_end >= state->cp_end));
}

void v92_cleardown_start(struct v92_cleardown *state,
	uint16_t round_trip_delay, bool initiator)
{
	if (!state->active)
		*state = (struct v92_cleardown){
			.round_trip_delay = round_trip_delay,
			.active = true,
			.initiator = initiator,
			.peer_requested = !initiator,
		};
}

void v92_cleardown_receive(struct v92_cleardown *state,
	bool zero_rate, bool acknowledges, uint64_t transmitted_symbols)
{
	state->peer_requested |= zero_rate;
	/* An acknowledgement of an earlier, nonzero selection cannot shorten
	 * this exchange before our first zero-rate CP has reached the wire. */
	if (state->cp_end && transmitted_symbols >= state->cp_end)
		state->acknowledged |= acknowledges;
}

void v92_cleardown_sent(struct v92_cleardown *state,
	uint64_t end_symbol, bool acknowledges)
{
	if (!state->cp_end)
		state->cp_end = end_symbol;
	if (acknowledges && !state->acknowledgement_end)
		state->acknowledgement_end = end_symbol;
}

bool v92_cleardown_is_ready(const struct v92_cleardown *state,
	uint64_t transmitted_symbols)
{
	uint64_t end = state->initiator ? state->cp_end : state->acknowledgement_end;
	uint32_t wait = 800u + (state->initiator ? state->round_trip_delay :
		((uint32_t)state->round_trip_delay + 1u) / 2u);

	if (!state->active || !end || transmitted_symbols < end ||
	    (state->peer_requested && (!state->acknowledgement_end ||
	     transmitted_symbols < state->acknowledgement_end)))
		return false;
	return (state->initiator && state->acknowledged) ||
		transmitted_symbols - end >= wait;
}
