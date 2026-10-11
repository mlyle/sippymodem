// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase3_return_marker.h"

#include "memory_region.h"

#include <limits.h>
#include <stddef.h>

_Static_assert(
	V90_PHASE3_RETURN_RX_ONSET_SYMBOLS <= UINT8_MAX,
	"V.90 S onset history must fit its counters");
_Static_assert(
	V90_PHASE3_RETURN_RX_ONSET_SYMBOLS <
		V90_PHASE3_RETURN_RX_TRACKED_ONSET_SYMBOLS,
	"tracked V.90 S onset must extend the ordinary onset");
_Static_assert(
	V90_PHASE3_RETURN_RX_TRACKED_ONSET_SYMBOLS <=
		V90_PHASE3_RETURN_RX_MARKER_SUFFIX_SYMBOLS,
	"tracked V.90 S onset must fit the retained marker suffix");

static bool onset_history_is_valid(
	const struct v90_phase3_return_marker *rx)
{
	if (rx->next_onset_decision >= V90_PHASE3_RETURN_RX_ONSET_SYMBOLS ||
	    rx->buffered_onset_decisions >
		    V90_PHASE3_RETURN_RX_ONSET_SYMBOLS ||
	    (rx->buffered_onset_decisions <
		     V90_PHASE3_RETURN_RX_ONSET_SYMBOLS &&
	     rx->next_onset_decision != rx->buffered_onset_decisions))
		return false;
	for (size_t index = 0;
	     index < V90_PHASE3_RETURN_RX_ONSET_SYMBOLS; ++index)
		if (rx->onset_decisions[index] >= 4u)
			return false;
	return true;
}

static bool s_exit_is_valid(const struct v90_phase3_return_marker *rx)
{
	if (rx->exit_symbols > V90_PHASE3_RETURN_RX_EXIT_SYMBOLS)
		return false;
	for (unsigned phase = 0; phase < 4u; ++phase)
		if (rx->exit_errors[phase] > rx->exit_symbols)
			return false;
	if (rx->exit_symbols != 0 &&
	    rx->confirmed_s_symbols <
		    V90_PHASE3_RETURN_RX_MIN_IMPLICIT_S_SYMBOLS)
		return false;
	return true;
}

static bool s_exit_is_nonperiodic(const struct v90_phase3_return_marker *rx)
{
	for (unsigned phase = 0; phase < 4u; ++phase)
		if (rx->exit_errors[phase] < V90_PHASE3_RETURN_RX_MIN_EXIT_ERRORS)
			return false;
	return true;
}

bool v90_phase3_return_marker_is_valid(
	const struct v90_phase3_return_marker *rx)
{
	if (rx == NULL || !v34_phase3_s_receiver_is_valid(&rx->marker) ||
	    rx->marker.s_symbols != V90_PHASE3_RETURN_RX_MARKER_SUFFIX_SYMBOLS ||
	    !onset_history_is_valid(rx) || !s_exit_is_valid(rx) ||
	    (rx->allow_implicit_s_end && rx->second_return))
		return false;
	switch (rx->stage) {
	case V90_PHASE3_RETURN_RX_STAGE_SEARCH_S:
		if (rx->confirmed_s_symbols == 0)
			return rx->onset_offset == 0 &&
			       rx->onset_errors == 0 &&
			       rx->exit_symbols == 0 &&
			       rx->marker.buffered_decisions == 0 &&
			       !v34_phase3_s_receiver_is_complete(
				       &rx->marker);
		return rx->confirm_onset &&
		       rx->buffered_onset_decisions ==
			       V90_PHASE3_RETURN_RX_ONSET_SYMBOLS &&
		       rx->confirmed_s_symbols >=
			       V90_PHASE3_RETURN_RX_ONSET_SYMBOLS &&
		       rx->confirmed_s_symbols <
			       V90_PHASE3_RETURN_RX_TRACKED_ONSET_SYMBOLS &&
		       rx->onset_offset < 4u &&
		       rx->onset_errors <=
			       V90_PHASE3_RETURN_RX_MAX_ONSET_ERRORS &&
		       rx->marker.buffered_decisions ==
			       rx->confirmed_s_symbols &&
		       rx->exit_symbols == 0 &&
		       !v34_phase3_s_receiver_is_complete(&rx->marker);
	case V90_PHASE3_RETURN_RX_STAGE_S:
		return rx->buffered_onset_decisions ==
			       V90_PHASE3_RETURN_RX_ONSET_SYMBOLS &&
		       rx->confirmed_s_symbols >=
			       V90_PHASE3_RETURN_RX_ONSET_SYMBOLS &&
		       rx->exit_symbols < V90_PHASE3_RETURN_RX_EXIT_SYMBOLS &&
		       rx->onset_offset < 4u &&
		       rx->onset_errors <=
			       V90_PHASE3_RETURN_RX_MAX_ONSET_ERRORS &&
		       rx->marker.buffered_decisions >=
			       V90_PHASE3_RETURN_RX_ONSET_SYMBOLS &&
		       !v34_phase3_s_receiver_is_complete(&rx->marker);
	case V90_PHASE3_RETURN_RX_STAGE_IMPLICIT_END:
		return !rx->allow_implicit_s_end &&
		       rx->confirmed_s_symbols >=
			       V90_PHASE3_RETURN_RX_MIN_IMPLICIT_S_SYMBOLS &&
		       rx->exit_symbols == V90_PHASE3_RETURN_RX_EXIT_SYMBOLS &&
		       s_exit_is_nonperiodic(rx) &&
		       !v34_phase3_s_receiver_is_complete(&rx->marker);
	case V90_PHASE3_RETURN_RX_STAGE_COMPLETE:
		return rx->buffered_onset_decisions ==
			       V90_PHASE3_RETURN_RX_ONSET_SYMBOLS &&
		       rx->onset_offset < 4u &&
		       rx->onset_errors <=
			       V90_PHASE3_RETURN_RX_MAX_ONSET_ERRORS &&
		       v34_phase3_s_receiver_is_complete(&rx->marker);
	}
	return false;
}

bool v90_phase3_return_marker_is_complete(
	const struct v90_phase3_return_marker *rx)
{
	return v90_phase3_return_marker_is_valid(rx) &&
	       rx->stage == V90_PHASE3_RETURN_RX_STAGE_COMPLETE;
}

enum v90_phase3_return_rx_result v90_phase3_return_marker_init(
	struct v90_phase3_return_marker *rx, bool confirm_onset)
{
	struct v90_phase3_return_marker initialized = {
		.stage = V90_PHASE3_RETURN_RX_STAGE_SEARCH_S,
		.confirm_onset = confirm_onset,
	};

	if (rx == NULL)
		return V90_PHASE3_RETURN_RX_INVALID_ARGUMENT;
	if (v34_phase3_s_receiver_init_suffix(
		    &initialized.marker,
		    V90_PHASE3_RETURN_RX_MARKER_SUFFIX_SYMBOLS) !=
		    V34_PHASE3_S_RECEIVER_OK ||
	    !v90_phase3_return_marker_is_valid(&initialized))
		return V90_PHASE3_RETURN_RX_NUMERIC_FAILURE;
	*rx = initialized;
	return V90_PHASE3_RETURN_RX_OK;
}

enum v90_phase3_return_rx_result
v90_phase3_return_marker_allow_implicit_s_end(
	struct v90_phase3_return_marker *rx)
{
	struct v90_phase3_return_marker next;

	if (!v90_phase3_return_marker_is_valid(rx))
		return V90_PHASE3_RETURN_RX_INVALID_ARGUMENT;
	if (rx->stage != V90_PHASE3_RETURN_RX_STAGE_SEARCH_S || rx->second_return)
		return V90_PHASE3_RETURN_RX_INVALID_STATE;
	next = *rx;
	next.allow_implicit_s_end = true;
	if (!v90_phase3_return_marker_is_valid(&next))
		return V90_PHASE3_RETURN_RX_NUMERIC_FAILURE;
	*rx = next;
	return V90_PHASE3_RETURN_RX_OK;
}

enum v90_phase3_return_rx_result v90_phase3_return_marker_restart(
	struct v90_phase3_return_marker *rx)
{
	struct v90_phase3_return_marker restarted;

	if (!v90_phase3_return_marker_is_valid(rx))
		return V90_PHASE3_RETURN_RX_INVALID_ARGUMENT;
	if (rx->stage != V90_PHASE3_RETURN_RX_STAGE_COMPLETE &&
	    rx->stage != V90_PHASE3_RETURN_RX_STAGE_IMPLICIT_END)
		return V90_PHASE3_RETURN_RX_INVALID_STATE;
	if (v90_phase3_return_marker_init(&restarted, rx->confirm_onset) !=
	    V90_PHASE3_RETURN_RX_OK)
		return V90_PHASE3_RETURN_RX_NUMERIC_FAILURE;
	restarted.second_return = true;
	*rx = restarted;
	return V90_PHASE3_RETURN_RX_OK;
}

static uint8_t onset_decision(
	const struct v90_phase3_return_marker *rx, size_t index)
{
	return rx->onset_decisions[
		(rx->next_onset_decision + index) %
		V90_PHASE3_RETURN_RX_ONSET_SYMBOLS];
}

static uint8_t expected_s_rotation(size_t symbol, uint8_t offset)
{
	return (uint8_t)((offset + 4u - (symbol & 1u)) & 3u);
}

static bool correlate_onset(
	const struct v90_phase3_return_marker *rx,
	uint8_t *selected_offset, uint8_t *selected_errors)
{
	unsigned int best_errors = UINT_MAX;
	uint8_t best_offset = 0;
	bool found = false;

	for (uint8_t offset = 0; offset < 4u; ++offset) {
		unsigned int errors = 0;

		for (size_t symbol = 0;
		     symbol < V90_PHASE3_RETURN_RX_ONSET_SYMBOLS; ++symbol)
			if (onset_decision(rx, symbol) !=
			    expected_s_rotation(symbol, offset))
				errors++;
		if (errors <= V90_PHASE3_RETURN_RX_MAX_ONSET_ERRORS &&
		    (!found || errors < best_errors)) {
			best_errors = errors;
			best_offset = offset;
			found = true;
		}
	}
	if (found) {
		*selected_offset = best_offset;
		*selected_errors = (uint8_t)best_errors;
	}
	return found;
}

static bool remember_onset(
	struct v90_phase3_return_marker *rx, uint8_t rotation)
{
	rx->onset_decisions[rx->next_onset_decision] = rotation;
	rx->next_onset_decision++;
	if (rx->next_onset_decision == V90_PHASE3_RETURN_RX_ONSET_SYMBOLS)
		rx->next_onset_decision = 0;
	if (rx->buffered_onset_decisions <
	    V90_PHASE3_RETURN_RX_ONSET_SYMBOLS)
		rx->buffered_onset_decisions++;
	return rx->buffered_onset_decisions ==
		       V90_PHASE3_RETURN_RX_ONSET_SYMBOLS &&
	       correlate_onset(rx, &rx->onset_offset, &rx->onset_errors);
}

static bool seed_marker(struct v90_phase3_return_marker *rx)
{
	for (size_t symbol = 0;
	     symbol < V90_PHASE3_RETURN_RX_ONSET_SYMBOLS; ++symbol)
		if (v34_phase3_s_receiver_receive(
			    &rx->marker, 0, onset_decision(rx, symbol)) !=
		    V34_PHASE3_S_RECEIVER_OK)
			return false;
	return true;
}

static bool reset_onset_candidate(
	struct v90_phase3_return_marker *rx, uint8_t rotation)
{
	if (v34_phase3_s_receiver_init_suffix(
		    &rx->marker,
		    V90_PHASE3_RETURN_RX_MARKER_SUFFIX_SYMBOLS) !=
	    V34_PHASE3_S_RECEIVER_OK)
		return false;
	rx->next_onset_decision = 0;
	rx->buffered_onset_decisions = 0;
	rx->onset_offset = 0;
	rx->onset_errors = 0;
	rx->confirmed_s_symbols = 0;
	rx->exit_symbols = 0;
	for (unsigned phase = 0; phase < 4u; ++phase)
		rx->exit_errors[phase] = 0;
	rx->stage = V90_PHASE3_RETURN_RX_STAGE_SEARCH_S;
	(void)remember_onset(rx, rotation);
	return true;
}

static bool expected_s_decision(
	const struct v90_phase3_return_marker *rx, uint8_t rotation)
{
	size_t symbol = (size_t)rx->confirmed_s_symbols + rx->exit_symbols;

	return rotation == expected_s_rotation(symbol, rx->onset_offset);
}

static enum v90_phase3_return_rx_result observe_s_exit(
	struct v90_phase3_return_marker *rx, uint8_t rotation)
{
	bool matches = expected_s_decision(rx, rotation);

	if (rx->exit_symbols == 0 &&
	    (rx->confirmed_s_symbols <
		     V90_PHASE3_RETURN_RX_MIN_IMPLICIT_S_SYMBOLS ||
	     matches)) {
		if (rx->confirmed_s_symbols != UINT16_MAX)
			rx->confirmed_s_symbols++;
		return V90_PHASE3_RETURN_RX_OK;
	}
	/* A carrier quarter-turn can change S's absolute phase without ending
	 * its alternating pattern. Only an exit from every phase can permit
	 * the legacy first-return fallback; the complete S-bar remains preferred. */
	for (uint8_t phase = 0; phase < 4u; ++phase)
		if (rotation != expected_s_rotation(
			    (size_t)rx->confirmed_s_symbols + rx->exit_symbols, phase))
			rx->exit_errors[phase]++;
	rx->exit_symbols++;
	if (rx->exit_symbols < V90_PHASE3_RETURN_RX_EXIT_SYMBOLS)
		return V90_PHASE3_RETURN_RX_OK;
	/* Do not rearm within an overlong alternating run: its remaining tail
	 * can otherwise look like a valid second S. Wait for an actual exit
	 * from the periodic signal, or reject its complete S-bar below. */
	if (rx->second_return && s_exit_is_nonperiodic(rx))
		return reset_onset_candidate(rx, rotation) ?
			V90_PHASE3_RETURN_RX_OK :
			V90_PHASE3_RETURN_RX_NUMERIC_FAILURE;
	if (rx->allow_implicit_s_end && s_exit_is_nonperiodic(rx)) {
		rx->allow_implicit_s_end = false;
		rx->stage = V90_PHASE3_RETURN_RX_STAGE_IMPLICIT_END;
		return V90_PHASE3_RETURN_RX_S_ENDED;
	}
	if (UINT16_MAX - rx->confirmed_s_symbols < rx->exit_symbols)
		rx->confirmed_s_symbols = UINT16_MAX;
	else
		rx->confirmed_s_symbols += rx->exit_symbols;
	rx->exit_symbols = 0;
	for (unsigned phase = 0; phase < 4u; ++phase)
		rx->exit_errors[phase] = 0;
	return V90_PHASE3_RETURN_RX_OK;
}

static enum v90_phase3_return_rx_result receive_decision(
	struct v90_phase3_return_marker *rx, uint8_t rotation)
{
	if (rx->stage == V90_PHASE3_RETURN_RX_STAGE_SEARCH_S) {
		if (rx->confirmed_s_symbols != 0) {
			if (rotation != expected_s_rotation(
						rx->confirmed_s_symbols,
						rx->onset_offset))
				return reset_onset_candidate(rx, rotation) ?
					       V90_PHASE3_RETURN_RX_OK :
					       V90_PHASE3_RETURN_RX_NUMERIC_FAILURE;
			if (v34_phase3_s_receiver_receive(
				    &rx->marker, 0, rotation) !=
				    V34_PHASE3_S_RECEIVER_OK)
				return V90_PHASE3_RETURN_RX_NUMERIC_FAILURE;
			rx->confirmed_s_symbols++;
			if (rx->confirmed_s_symbols <
			    V90_PHASE3_RETURN_RX_TRACKED_ONSET_SYMBOLS)
				return V90_PHASE3_RETURN_RX_OK;
			rx->stage = V90_PHASE3_RETURN_RX_STAGE_S;
			return V90_PHASE3_RETURN_RX_S_DETECTED;
		}
		if (!remember_onset(rx, rotation))
			return V90_PHASE3_RETURN_RX_OK;
		if (!seed_marker(rx))
			return V90_PHASE3_RETURN_RX_NUMERIC_FAILURE;
		rx->confirmed_s_symbols =
			V90_PHASE3_RETURN_RX_ONSET_SYMBOLS;
		if (rx->confirm_onset)
			return V90_PHASE3_RETURN_RX_OK;
		rx->stage = V90_PHASE3_RETURN_RX_STAGE_S;
		return V90_PHASE3_RETURN_RX_S_DETECTED;
	}
	switch (v34_phase3_s_receiver_receive(
		&rx->marker, 0, rotation)) {
	case V34_PHASE3_S_RECEIVER_OK:
		return observe_s_exit(rx, rotation);
	case V34_PHASE3_S_RECEIVER_DETECTED:
		if (rx->second_return &&
		    (rx->confirmed_s_symbols <
			     V90_PHASE3_RETURN_RX_MIN_SECOND_S_SYMBOLS ||
		     rx->confirmed_s_symbols >
			     V90_PHASE3_RETURN_RX_MAX_SECOND_S_SYMBOLS))
			return reset_onset_candidate(rx, rotation) ?
				V90_PHASE3_RETURN_RX_OK :
				V90_PHASE3_RETURN_RX_NUMERIC_FAILURE;
		rx->stage = V90_PHASE3_RETURN_RX_STAGE_COMPLETE;
		return V90_PHASE3_RETURN_RX_S_BAR_DETECTED;
	case V34_PHASE3_S_RECEIVER_COMPLETE:
	case V34_PHASE3_S_RECEIVER_INVALID_ARGUMENT:
		break;
	}
	return V90_PHASE3_RETURN_RX_NUMERIC_FAILURE;
}

static bool result_commits(enum v90_phase3_return_rx_result result)
{
	return result == V90_PHASE3_RETURN_RX_OK ||
	       result == V90_PHASE3_RETURN_RX_S_DETECTED ||
	       result == V90_PHASE3_RETURN_RX_S_ENDED ||
	       result == V90_PHASE3_RETURN_RX_S_BAR_DETECTED;
}

enum v90_phase3_return_rx_result v90_phase3_return_marker_observe(
	struct v90_phase3_return_marker *rx, uint8_t rotation)
{
	struct v90_phase3_return_marker next;
	enum v90_phase3_return_rx_result result;

	if (!v90_phase3_return_marker_is_valid(rx) || rotation >= 4u)
		return V90_PHASE3_RETURN_RX_INVALID_ARGUMENT;
	if (rx->stage == V90_PHASE3_RETURN_RX_STAGE_COMPLETE)
		return V90_PHASE3_RETURN_RX_COMPLETE;
	if (rx->stage == V90_PHASE3_RETURN_RX_STAGE_IMPLICIT_END)
		return V90_PHASE3_RETURN_RX_INVALID_STATE;
	next = *rx;
	result = receive_decision(&next, rotation);
	if (!result_commits(result))
		return result;
	if (!v90_phase3_return_marker_is_valid(&next))
		return V90_PHASE3_RETURN_RX_NUMERIC_FAILURE;
	*rx = next;
	return result;
}

bool v90_phase3_return_marker_get_detection(
	const struct v90_phase3_return_marker *rx,
	struct v34_phase3_s_detection *detection)
{
	return v90_phase3_return_marker_is_complete(rx) && detection != NULL &&
	       !memory_regions_overlap(
		       rx, sizeof(*rx), detection, sizeof(*detection)) &&
	       v34_phase3_s_receiver_get_detection(&rx->marker, detection);
}
