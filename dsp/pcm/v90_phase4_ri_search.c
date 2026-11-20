// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase4_ri_search.h"

#include <stddef.h>

bool v90_phase4_ri_search_is_valid(
	const struct v90_phase4_ri_search *search)
{
	if (search == NULL || !v90_phase4_ri_rx_is_valid(&search->rx) ||
	    search->history_count > V90_PHASE4_RI_SEARCH_SYMBOLS ||
	    search->history_cursor >= V90_PHASE4_RI_SEARCH_SYMBOLS)
		return false;
	switch (search->stage) {
	case V90_PHASE4_RI_SEARCH_HUNT:
		return !search->rx.polarity_known;
	case V90_PHASE4_RI_SEARCH_AMBIGUOUS:
		return search->history_count == V90_PHASE4_RI_SEARCH_SYMBOLS &&
		       search->rx.stage == V90_PHASE4_RI &&
		       search->rx.ri_symbols == V90_PHASE4_RI_MIN_SYMBOLS &&
		       !search->rx.inverted;
	case V90_PHASE4_RI_SEARCH_BAR:
		return search->rx.stage == V90_PHASE4_RI_BAR;
	case V90_PHASE4_RI_SEARCH_COMPLETE:
		return search->rx.stage == V90_PHASE4_RI_COMPLETE;
	}
	return false;
}

enum v90_phase4_ri_search_result v90_phase4_ri_search_init(
	struct v90_phase4_ri_search *search, enum g711_law law,
	uint8_t training_ucode)
{
	struct v90_phase4_ri_search initialized = {
		.stage = V90_PHASE4_RI_SEARCH_HUNT,
	};

	if (search == NULL ||
	    v90_phase4_ri_rx_init(&initialized.rx, law, training_ucode) != V90_PHASE4_RI_OK)
		return V90_PHASE4_RI_SEARCH_INVALID_ARGUMENT;
	*search = initialized;
	return V90_PHASE4_RI_SEARCH_OK;
}

bool v90_phase4_ri_search_is_acquired(
	const struct v90_phase4_ri_search *search)
{
	return v90_phase4_ri_search_is_valid(search) &&
	       search->stage != V90_PHASE4_RI_SEARCH_HUNT;
}

bool v90_phase4_ri_search_is_complete(
	const struct v90_phase4_ri_search *search)
{
	return v90_phase4_ri_search_is_valid(search) &&
	       search->stage == V90_PHASE4_RI_SEARCH_COMPLETE;
}

static bool nominal_sign(uint8_t phase)
{
	return phase < V90_PHASE4_RI_PERIOD / 2u;
}

static void push_sign(
	struct v90_phase4_ri_search *search, bool positive)
{
	search->history[search->history_cursor] = positive ? 1u : 0u;
	search->history_cursor++;
	if (search->history_cursor == V90_PHASE4_RI_SEARCH_SYMBOLS)
		search->history_cursor = 0;
	if (search->history_count < V90_PHASE4_RI_SEARCH_SYMBOLS)
		search->history_count++;
}

static bool find_phase(
	const struct v90_phase4_ri_search *search, uint8_t *phase_out)
{
	for (uint8_t phase = 0; phase < V90_PHASE4_RI_PERIOD; ++phase) {
		bool matches = true;

		for (uint8_t offset = 0;
		     offset < V90_PHASE4_RI_SEARCH_SYMBOLS; ++offset) {
			uint8_t index = (uint8_t)(
				search->history_cursor + offset);

			if (index >= V90_PHASE4_RI_SEARCH_SYMBOLS)
				index -= V90_PHASE4_RI_SEARCH_SYMBOLS;
			if ((search->history[index] != 0u) !=
			    nominal_sign((uint8_t)(
				    (phase + offset) %
				    V90_PHASE4_RI_PERIOD))) {
				matches = false;
				break;
			}
		}
		if (matches) {
			*phase_out = phase;
			return true;
		}
	}
	return false;
}

static enum v90_phase4_ri_search_result receive_hunt(
	struct v90_phase4_ri_search *search, bool qualified, bool positive)
{
	uint8_t phase;

	if (!qualified) {
		search->history_count = 0;
		search->history_cursor = 0;
		return V90_PHASE4_RI_SEARCH_OK;
	}
	push_sign(search, positive);
	if (search->history_count < V90_PHASE4_RI_SEARCH_SYMBOLS ||
	    !find_phase(search, &phase))
		return V90_PHASE4_RI_SEARCH_OK;

	/* The search can start in a later Ri period. With the prefix saturated,
	 * the sender's wire-visible Ri-bar boundary enforces the minimum. */
	search->rx.polarity_known = true;
	search->rx.phase = phase;
	search->rx.ri_symbols = V90_PHASE4_RI_MIN_SYMBOLS;
	search->stage = V90_PHASE4_RI_SEARCH_AMBIGUOUS;
	return V90_PHASE4_RI_SEARCH_ACQUIRED;
}

static enum v90_phase4_ri_search_result receive_aligned(
	struct v90_phase4_ri_search *search, bool positive)
{
	/* Inverting polarity and adding three symbols predict the same Ri.
	 * Only an unexpected sign at the start of a period can begin Ri-bar.
	 * In the positive-phase representation, the inverted period starts
	 * three symbols later. Select that phase and polarity at its edge. */
	if (search->stage == V90_PHASE4_RI_SEARCH_AMBIGUOUS &&
	    search->rx.phase == V90_PHASE4_RI_PERIOD / 2u && positive) {
		search->rx.phase = 0;
		search->rx.inverted = true;
	}

	switch (v90_phase4_ri_rx_sign_owned(&search->rx, positive)) {
	case V90_PHASE4_RI_OK:
		return V90_PHASE4_RI_SEARCH_OK;
	case V90_PHASE4_RI_TRANSITION_DETECTED:
		search->stage = V90_PHASE4_RI_SEARCH_BAR;
		return V90_PHASE4_RI_SEARCH_TRANSITION_DETECTED;
	case V90_PHASE4_RI_DONE:
		search->stage = V90_PHASE4_RI_SEARCH_COMPLETE;
		return V90_PHASE4_RI_SEARCH_DONE;
	case V90_PHASE4_RI_INVALID_SEQUENCE:
		return V90_PHASE4_RI_SEARCH_INVALID_SEQUENCE;
	default:
		return V90_PHASE4_RI_SEARCH_INVALID_STATE;
	}
}

enum v90_phase4_ri_search_result v90_phase4_ri_search_receive_owned(
	struct v90_phase4_ri_search *search, uint8_t network_octet)
{
	if (search->stage == V90_PHASE4_RI_SEARCH_COMPLETE)
		return V90_PHASE4_RI_SEARCH_DONE;

	bool positive = false;
	bool qualified = v90_phase4_ri_observe(search->rx.law,
		search->rx.training_ucode, network_octet, &positive);
	if (search->stage != V90_PHASE4_RI_SEARCH_HUNT) {
		if (qualified) {
			enum v90_phase4_ri_search_result result = receive_aligned(search, positive);

			if (result != V90_PHASE4_RI_SEARCH_INVALID_SEQUENCE)
				return result;
		}
		/* Keep hunting through a damaged Ri character or false Ri-bar
		 * edge. Reuse its sign when the magnitude still qualifies. */
		if (v90_phase4_ri_search_init(search, search->rx.law,
		    search->rx.training_ucode) != V90_PHASE4_RI_SEARCH_OK)
			return V90_PHASE4_RI_SEARCH_INVALID_STATE;
	}
	return receive_hunt(search, qualified, positive);
}

enum v90_phase4_ri_search_result v90_phase4_ri_search_receive(
	struct v90_phase4_ri_search *search, uint8_t network_octet)
{
	if (!v90_phase4_ri_search_is_valid(search))
		return V90_PHASE4_RI_SEARCH_INVALID_ARGUMENT;
	if (search->stage == V90_PHASE4_RI_SEARCH_COMPLETE)
		return V90_PHASE4_RI_SEARCH_DONE;

	struct v90_phase4_ri_search next = *search;
	enum v90_phase4_ri_search_result result =
		v90_phase4_ri_search_receive_owned(&next, network_octet);

	if (result >= V90_PHASE4_RI_SEARCH_INVALID_ARGUMENT)
		return result;
	if (!v90_phase4_ri_search_is_valid(&next))
		return V90_PHASE4_RI_SEARCH_INVALID_STATE;
	*search = next;
	return result;
}

const char *v90_phase4_ri_search_result_name(
	enum v90_phase4_ri_search_result result)
{
	switch (result) {
	case V90_PHASE4_RI_SEARCH_OK:
		return "ok";
	case V90_PHASE4_RI_SEARCH_ACQUIRED:
		return "Ri acquired";
	case V90_PHASE4_RI_SEARCH_TRANSITION_DETECTED:
		return "Ri-bar detected";
	case V90_PHASE4_RI_SEARCH_DONE:
		return "done";
	case V90_PHASE4_RI_SEARCH_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_PHASE4_RI_SEARCH_INVALID_STATE:
		return "invalid state";
	case V90_PHASE4_RI_SEARCH_INVALID_SEQUENCE:
		return "invalid sequence";
	}
	return "unknown";
}
