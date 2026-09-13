// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase4_ri_search.h"

#include "v90_info1.h"
#include "v90_pcm_symbol.h"

#include <stddef.h>

enum {
	RI_SEARCH_NO_HYPOTHESIS = 2,
};

static bool stage_is_valid(enum v90_phase4_ri_search_stage stage)
{
	return stage >= V90_PHASE4_RI_SEARCH_HUNT &&
	       stage <= V90_PHASE4_RI_SEARCH_COMPLETE;
}

static bool hypothesis_matches(
	const struct v90_phase4_ri_rx *rx, enum g711_law law,
	uint8_t training_ucode)
{
	return v90_phase4_ri_rx_is_valid(rx) && rx->law == law &&
	       rx->training_ucode == training_ucode &&
	       rx->polarity_known &&
	       rx->ri_symbols == V90_PHASE4_RI_MIN_SYMBOLS;
}

bool v90_phase4_ri_search_is_valid(
	const struct v90_phase4_ri_search *search)
{
	if (search == NULL || !stage_is_valid(search->stage) ||
	    !g711_law_is_valid(search->law) ||
	    search->training_ucode < V90_INFO1A_MIN_TRAINING_UCODE ||
	    search->training_ucode > V90_INFO1A_MAX_TRAINING_UCODE ||
	    search->history_count > V90_PHASE4_RI_SEARCH_SYMBOLS ||
	    search->history_cursor >= V90_PHASE4_RI_SEARCH_SYMBOLS ||
	    search->selected_hypothesis > RI_SEARCH_NO_HYPOTHESIS)
		return false;
	switch (search->stage) {
	case V90_PHASE4_RI_SEARCH_HUNT:
		return search->history_count <= V90_PHASE4_RI_SEARCH_SYMBOLS &&
		       search->selected_hypothesis == RI_SEARCH_NO_HYPOTHESIS;
	case V90_PHASE4_RI_SEARCH_AMBIGUOUS:
		return search->history_count == V90_PHASE4_RI_SEARCH_SYMBOLS &&
		       search->selected_hypothesis == RI_SEARCH_NO_HYPOTHESIS &&
		       hypothesis_matches(
			       &search->hypotheses[0], search->law,
			       search->training_ucode) &&
		       hypothesis_matches(
			       &search->hypotheses[1], search->law,
			       search->training_ucode) &&
		       search->hypotheses[0].stage == V90_PHASE4_RI &&
		       search->hypotheses[1].stage == V90_PHASE4_RI &&
		       search->hypotheses[0].inverted !=
			       search->hypotheses[1].inverted &&
		       (search->hypotheses[0].phase + 3u) %
			       V90_PHASE4_RI_PERIOD ==
			       search->hypotheses[1].phase;
	case V90_PHASE4_RI_SEARCH_BAR:
		return search->selected_hypothesis < 2u &&
		       hypothesis_matches(
			       &search->hypotheses[search->selected_hypothesis],
			       search->law, search->training_ucode) &&
		       search->hypotheses[search->selected_hypothesis].stage ==
			       V90_PHASE4_RI_BAR;
	case V90_PHASE4_RI_SEARCH_COMPLETE:
		return search->selected_hypothesis < 2u &&
		       hypothesis_matches(
			       &search->hypotheses[search->selected_hypothesis],
			       search->law, search->training_ucode) &&
		       v90_phase4_ri_rx_is_complete(
			       &search->hypotheses[
				       search->selected_hypothesis]);
	}
	return false;
}

enum v90_phase4_ri_search_result v90_phase4_ri_search_init(
	struct v90_phase4_ri_search *search, enum g711_law law,
	uint8_t training_ucode)
{
	struct v90_phase4_ri_search initialized = {
		.stage = V90_PHASE4_RI_SEARCH_HUNT,
		.law = law,
		.training_ucode = training_ucode,
		.selected_hypothesis = RI_SEARCH_NO_HYPOTHESIS,
	};

	if (search == NULL || !v90_phase4_ri_search_is_valid(&initialized))
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

static bool seed_hypothesis(
	struct v90_phase4_ri_rx *rx, enum g711_law law,
	uint8_t training_ucode, uint8_t phase, bool inverted)
{
	if (v90_phase4_ri_rx_init(rx, law, training_ucode) !=
	    V90_PHASE4_RI_OK)
		return false;
	rx->polarity_known = true;
	rx->inverted = inverted;
	rx->phase = phase;
	/* Acquisition may begin after an arbitrary number of complete Ri
	 * periods. Saturating the hidden prefix lets the standard-compliant
	 * sender's wire-visible Ri-bar boundary, rather than our observation
	 * start, enforce the 192-symbol minimum. */
	rx->ri_symbols = V90_PHASE4_RI_MIN_SYMBOLS;
	return v90_phase4_ri_rx_is_valid(rx);
}

static enum v90_phase4_ri_search_result receive_hunt(
	struct v90_phase4_ri_search *search, uint8_t network_octet)
{
	struct v90_pcm_symbol symbol;
	uint8_t phase;

	if (!v90_pcm_symbol_parse(&symbol, search->law, network_octet) ||
	    !v90_phase4_ri_level_matches(
		    search->law, search->training_ucode, symbol.sample)) {
		search->history_count = 0;
		search->history_cursor = 0;
		return V90_PHASE4_RI_SEARCH_OK;
	}
	push_sign(search, symbol.positive);
	if (search->history_count < V90_PHASE4_RI_SEARCH_SYMBOLS ||
	    !find_phase(search, &phase))
		return V90_PHASE4_RI_SEARCH_OK;
	if (!seed_hypothesis(
		    &search->hypotheses[0], search->law,
		    search->training_ucode, phase, false) ||
	    !seed_hypothesis(
		    &search->hypotheses[1], search->law,
		    search->training_ucode,
		    (uint8_t)((phase + 3u) % V90_PHASE4_RI_PERIOD), true))
		return V90_PHASE4_RI_SEARCH_INVALID_STATE;
	search->stage = V90_PHASE4_RI_SEARCH_AMBIGUOUS;
	return V90_PHASE4_RI_SEARCH_ACQUIRED;
}

static enum v90_phase4_ri_search_result receive_ambiguous(
	struct v90_phase4_ri_search *search, uint8_t network_octet)
{
	struct v90_phase4_ri_rx next[2] = {
		search->hypotheses[0], search->hypotheses[1],
	};
	enum v90_phase4_ri_result result[2];

	for (size_t index = 0; index < 2u; ++index)
		result[index] = v90_phase4_ri_rx_receive(
			&next[index], network_octet);
	if (result[0] == V90_PHASE4_RI_OK &&
	    result[1] == V90_PHASE4_RI_OK) {
		search->hypotheses[0] = next[0];
		search->hypotheses[1] = next[1];
		return V90_PHASE4_RI_SEARCH_OK;
	}
	if (result[0] == V90_PHASE4_RI_TRANSITION_DETECTED &&
	    result[1] == V90_PHASE4_RI_INVALID_SEQUENCE)
		search->selected_hypothesis = 0;
	else if (result[1] == V90_PHASE4_RI_TRANSITION_DETECTED &&
		 result[0] == V90_PHASE4_RI_INVALID_SEQUENCE)
		search->selected_hypothesis = 1;
	else if (result[0] == V90_PHASE4_RI_INVALID_STATE ||
		 result[1] == V90_PHASE4_RI_INVALID_STATE ||
		 result[0] == V90_PHASE4_RI_INVALID_ARGUMENT ||
		 result[1] == V90_PHASE4_RI_INVALID_ARGUMENT)
		return V90_PHASE4_RI_SEARCH_INVALID_STATE;
	else
		return V90_PHASE4_RI_SEARCH_INVALID_SEQUENCE;
	search->hypotheses[search->selected_hypothesis] =
		next[search->selected_hypothesis];
	search->stage = V90_PHASE4_RI_SEARCH_BAR;
	return V90_PHASE4_RI_SEARCH_TRANSITION_DETECTED;
}

static enum v90_phase4_ri_search_result receive_bar(
	struct v90_phase4_ri_search *search, uint8_t network_octet)
{
	struct v90_phase4_ri_rx *rx =
		&search->hypotheses[search->selected_hypothesis];
	enum v90_phase4_ri_result result =
		v90_phase4_ri_rx_receive(rx, network_octet);

	if (result == V90_PHASE4_RI_OK)
		return V90_PHASE4_RI_SEARCH_OK;
	if (result == V90_PHASE4_RI_DONE) {
		search->stage = V90_PHASE4_RI_SEARCH_COMPLETE;
		return V90_PHASE4_RI_SEARCH_DONE;
	}
	return result == V90_PHASE4_RI_INVALID_SEQUENCE ?
		       V90_PHASE4_RI_SEARCH_INVALID_SEQUENCE :
		       V90_PHASE4_RI_SEARCH_INVALID_STATE;
}

static enum v90_phase4_ri_search_result restart_hunt(
	struct v90_phase4_ri_search *search, uint8_t network_octet)
{
	enum g711_law law = search->law;
	uint8_t training_ucode = search->training_ucode;

	if (v90_phase4_ri_search_init(search, law, training_ucode) !=
	    V90_PHASE4_RI_SEARCH_OK)
		return V90_PHASE4_RI_SEARCH_INVALID_STATE;
	/* Retain a valid current sign as the first sliding-search sample.  A
	 * wrong-magnitude character simply starts an empty hunt. */
	return receive_hunt(search, network_octet);
}

enum v90_phase4_ri_search_result v90_phase4_ri_search_receive(
	struct v90_phase4_ri_search *search, uint8_t network_octet)
{
	struct v90_phase4_ri_search next;
	enum v90_phase4_ri_search_result result;

	if (!v90_phase4_ri_search_is_valid(search))
		return V90_PHASE4_RI_SEARCH_INVALID_ARGUMENT;
	if (v90_phase4_ri_search_is_complete(search))
		return V90_PHASE4_RI_SEARCH_DONE;
	next = *search;
	if (next.stage == V90_PHASE4_RI_SEARCH_HUNT)
		result = receive_hunt(&next, network_octet);
	else if (next.stage == V90_PHASE4_RI_SEARCH_AMBIGUOUS)
		result = receive_ambiguous(&next, network_octet);
	else
		result = receive_bar(&next, network_octet);
	/* Ri is deliberately long and periodic.  A damaged network character
	 * after acquisition is therefore loss of synchronization, not proof of
	 * a terminal protocol error: return to the sliding detector and use the
	 * remaining Ri repetitions to reacquire.  The same recovery also keeps a
	 * false Ri-bar edge from permanently latching the call failed. */
	if (result == V90_PHASE4_RI_SEARCH_INVALID_SEQUENCE)
		result = restart_hunt(&next, network_octet);
	if (result == V90_PHASE4_RI_SEARCH_INVALID_SEQUENCE ||
	    result == V90_PHASE4_RI_SEARCH_INVALID_STATE)
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
