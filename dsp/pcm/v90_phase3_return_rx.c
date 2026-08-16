// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase3_return_rx.h"

#include "v34_constellation.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>

_Static_assert(
	V90_PHASE3_RETURN_RX_ONSET_SYMBOLS <= UINT8_MAX,
	"V.90 S onset history must fit its counters");

static bool adaptation_is_valid(double adaptation)
{
	return isfinite(adaptation) && adaptation >= 0.0 && adaptation <= 1.0;
}

static bool equalizer_is_at_handoff(
	const struct qam_equalizer *equalizer)
{
	return qam_equalizer_is_valid(equalizer) &&
	       equalizer->fill == equalizer->decision_delay + 1u &&
	       !equalizer->can_adapt;
}

static bool onset_history_is_valid(
	const struct v90_phase3_return_rx *rx)
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

static bool s_exit_is_valid(const struct v90_phase3_return_rx *rx)
{
	if (rx->exit_symbols > V90_PHASE3_RETURN_RX_EXIT_SYMBOLS ||
	    rx->exit_errors > rx->exit_symbols)
		return false;
	if (rx->exit_symbols != 0 &&
	    rx->confirmed_s_symbols <
		    V90_PHASE3_RETURN_RX_MIN_IMPLICIT_S_SYMBOLS)
		return false;
	return true;
}

bool v90_phase3_return_rx_is_valid(
	const struct v90_phase3_return_rx *rx)
{
	if (rx == NULL || !adaptation_is_valid(rx->adaptation) ||
	    !equalizer_is_at_handoff(&rx->equalizer) ||
	    !v34_phase3_s_receiver_is_valid(&rx->marker) ||
	    rx->marker.s_symbols != V90_PHASE3_RETURN_RX_ONSET_SYMBOLS ||
	    !onset_history_is_valid(rx) || !s_exit_is_valid(rx))
		return false;
	switch (rx->stage) {
	case V90_PHASE3_RETURN_RX_STAGE_SEARCH_S:
		return rx->onset_offset == 0 && rx->onset_errors == 0 &&
		       rx->confirmed_s_symbols == 0 &&
		       rx->exit_symbols == 0 && rx->exit_errors == 0 &&
		       rx->marker.buffered_decisions == 0 &&
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
		       rx->exit_errors >=
			       V90_PHASE3_RETURN_RX_MIN_EXIT_ERRORS &&
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

bool v90_phase3_return_rx_is_complete(
	const struct v90_phase3_return_rx *rx)
{
	return v90_phase3_return_rx_is_valid(rx) &&
	       rx->stage == V90_PHASE3_RETURN_RX_STAGE_COMPLETE;
}

enum v90_phase3_return_rx_result v90_phase3_return_rx_init(
	struct v90_phase3_return_rx *rx,
	const struct v34_equalizer_handoff *equalizer,
	double adaptation)
{
	struct v90_phase3_return_rx initialized = {
		.adaptation = adaptation,
		.stage = V90_PHASE3_RETURN_RX_STAGE_SEARCH_S,
	};

	if (rx == NULL || !v34_equalizer_handoff_is_valid(equalizer) ||
	    !adaptation_is_valid(adaptation))
		return V90_PHASE3_RETURN_RX_INVALID_ARGUMENT;
	initialized.equalizer = equalizer->equalizer;
	if (v34_phase3_s_receiver_init_suffix(
		    &initialized.marker,
		    V90_PHASE3_RETURN_RX_ONSET_SYMBOLS) !=
		    V34_PHASE3_S_RECEIVER_OK ||
	    !v90_phase3_return_rx_is_valid(&initialized))
		return V90_PHASE3_RETURN_RX_INVALID_ARGUMENT;
	*rx = initialized;
	return V90_PHASE3_RETURN_RX_OK;
}

enum v90_phase3_return_rx_result
v90_phase3_return_rx_allow_implicit_s_end(
	struct v90_phase3_return_rx *rx)
{
	struct v90_phase3_return_rx next;

	if (!v90_phase3_return_rx_is_valid(rx))
		return V90_PHASE3_RETURN_RX_INVALID_ARGUMENT;
	if (rx->stage != V90_PHASE3_RETURN_RX_STAGE_SEARCH_S)
		return V90_PHASE3_RETURN_RX_INVALID_STATE;
	next = *rx;
	next.allow_implicit_s_end = true;
	if (!v90_phase3_return_rx_is_valid(&next))
		return V90_PHASE3_RETURN_RX_NUMERIC_FAILURE;
	*rx = next;
	return V90_PHASE3_RETURN_RX_OK;
}

enum v90_phase3_return_rx_result v90_phase3_return_rx_restart(
	struct v90_phase3_return_rx *rx)
{
	struct v34_equalizer_handoff equalizer;
	double adaptation;

	if (!v90_phase3_return_rx_is_valid(rx))
		return V90_PHASE3_RETURN_RX_INVALID_ARGUMENT;
	if (rx->stage != V90_PHASE3_RETURN_RX_STAGE_COMPLETE &&
	    rx->stage != V90_PHASE3_RETURN_RX_STAGE_IMPLICIT_END)
		return V90_PHASE3_RETURN_RX_INVALID_STATE;
	equalizer.equalizer = rx->equalizer;
	adaptation = rx->adaptation;
	return v90_phase3_return_rx_init(rx, &equalizer, adaptation);
}

static uint8_t onset_decision(
	const struct v90_phase3_return_rx *rx, size_t index)
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
	const struct v90_phase3_return_rx *rx,
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
	struct v90_phase3_return_rx *rx, uint8_t rotation)
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

static bool seed_marker(struct v90_phase3_return_rx *rx)
{
	for (size_t symbol = 0;
	     symbol < V90_PHASE3_RETURN_RX_ONSET_SYMBOLS; ++symbol)
		if (v34_phase3_s_receiver_receive(
			    &rx->marker, 0, onset_decision(rx, symbol)) !=
		    V34_PHASE3_S_RECEIVER_OK)
			return false;
	return true;
}

static bool adapt_decision(
	struct v90_phase3_return_rx *rx,
	struct qam_equalizer_observation *observation,
	uint8_t rotation)
{
	struct qam_point target;

	return v34_training_constellation_point(
		       V34_CONSTELLATION_4_POINT, 0, rotation, &target) ==
		       V34_CONSTELLATION_OK &&
	       qam_equalizer_adapt_observation(
		       &rx->equalizer, observation, &target,
		       rx->adaptation) == QAM_EQUALIZER_OK;
}

static bool expected_s_decision(
	const struct v90_phase3_return_rx *rx, uint8_t rotation)
{
	size_t symbol = (size_t)rx->confirmed_s_symbols + rx->exit_symbols;

	return rotation == expected_s_rotation(symbol, rx->onset_offset);
}

static enum v90_phase3_return_rx_result observe_s_exit(
	struct v90_phase3_return_rx *rx, uint8_t rotation)
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
	rx->exit_symbols++;
	if (!matches)
		rx->exit_errors++;
	if (rx->exit_symbols < V90_PHASE3_RETURN_RX_EXIT_SYMBOLS)
		return V90_PHASE3_RETURN_RX_OK;
	if (rx->allow_implicit_s_end &&
	    rx->exit_errors >= V90_PHASE3_RETURN_RX_MIN_EXIT_ERRORS) {
		rx->allow_implicit_s_end = false;
		rx->stage = V90_PHASE3_RETURN_RX_STAGE_IMPLICIT_END;
		return V90_PHASE3_RETURN_RX_S_ENDED;
	}
	if (UINT16_MAX - rx->confirmed_s_symbols < rx->exit_symbols)
		rx->confirmed_s_symbols = UINT16_MAX;
	else
		rx->confirmed_s_symbols += rx->exit_symbols;
	rx->exit_symbols = 0;
	rx->exit_errors = 0;
	return V90_PHASE3_RETURN_RX_OK;
}

static enum v90_phase3_return_rx_result receive_decision(
	struct v90_phase3_return_rx *rx,
	struct qam_equalizer_observation *observation,
	uint8_t rotation)
{
	if (rx->stage == V90_PHASE3_RETURN_RX_STAGE_SEARCH_S) {
		if (!remember_onset(rx, rotation))
			return V90_PHASE3_RETURN_RX_OK;
		if (!seed_marker(rx) ||
		    !adapt_decision(rx, observation, rotation))
			return V90_PHASE3_RETURN_RX_NUMERIC_FAILURE;
		rx->confirmed_s_symbols =
			V90_PHASE3_RETURN_RX_ONSET_SYMBOLS;
		rx->stage = V90_PHASE3_RETURN_RX_STAGE_S;
		return V90_PHASE3_RETURN_RX_S_DETECTED;
	}
	if (!adapt_decision(rx, observation, rotation))
		return V90_PHASE3_RETURN_RX_NUMERIC_FAILURE;
	switch (v34_phase3_s_receiver_receive(
		&rx->marker, 0, rotation)) {
	case V34_PHASE3_S_RECEIVER_OK:
		return observe_s_exit(rx, rotation);
	case V34_PHASE3_S_RECEIVER_DETECTED:
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

enum v90_phase3_return_rx_result v90_phase3_return_rx_point(
	struct v90_phase3_return_rx *rx,
	const struct qam_point *received)
{
	struct v90_phase3_return_rx next;
	struct qam_equalizer_observation observation = { 0 };
	uint8_t quarter_point;
	uint8_t rotation;
	bool ready;
	enum v90_phase3_return_rx_result result;

	if (!v90_phase3_return_rx_is_valid(rx) || received == NULL ||
	    !isfinite(received->in_phase) ||
	    !isfinite(received->quadrature))
		return V90_PHASE3_RETURN_RX_INVALID_ARGUMENT;
	if (rx->stage == V90_PHASE3_RETURN_RX_STAGE_COMPLETE)
		return V90_PHASE3_RETURN_RX_COMPLETE;
	if (rx->stage == V90_PHASE3_RETURN_RX_STAGE_IMPLICIT_END)
		return V90_PHASE3_RETURN_RX_INVALID_STATE;
	next = *rx;
	if (qam_equalizer_observe(
		    &next.equalizer, received, &observation, &ready) !=
		    QAM_EQUALIZER_OK ||
	    !ready ||
	    v34_training_constellation_slice(
		    V34_CONSTELLATION_4_POINT, &observation.output,
		    &quarter_point, &rotation) != V34_CONSTELLATION_OK ||
	    quarter_point != 0)
		return V90_PHASE3_RETURN_RX_NUMERIC_FAILURE;
	result = receive_decision(&next, &observation, rotation);
	if (!result_commits(result))
		return result;
	if (!v90_phase3_return_rx_is_valid(&next))
		return V90_PHASE3_RETURN_RX_NUMERIC_FAILURE;
	*rx = next;
	return result;
}

bool v90_phase3_return_handoff_is_valid(
	const struct v90_phase3_return_handoff *handoff)
{
	return handoff != NULL &&
	       v34_phase3_s_detection_is_valid(&handoff->marker) &&
	       v34_equalizer_handoff_is_valid(&handoff->equalizer);
}

enum v90_phase3_return_rx_result v90_phase3_return_rx_handoff(
	const struct v90_phase3_return_rx *rx,
	struct v90_phase3_return_handoff *handoff)
{
	struct v90_phase3_return_handoff generated;

	if (!v90_phase3_return_rx_is_valid(rx) || handoff == NULL)
		return V90_PHASE3_RETURN_RX_INVALID_ARGUMENT;
	if (!v90_phase3_return_rx_is_complete(rx))
		return V90_PHASE3_RETURN_RX_INVALID_STATE;
	generated.equalizer.equalizer = rx->equalizer;
	if (!v34_phase3_s_receiver_get_detection(
		    &rx->marker, &generated.marker) ||
	    !v90_phase3_return_handoff_is_valid(&generated))
		return V90_PHASE3_RETURN_RX_INVALID_STATE;
	*handoff = generated;
	return V90_PHASE3_RETURN_RX_OK;
}

const char *v90_phase3_return_rx_result_name(
	enum v90_phase3_return_rx_result result)
{
	switch (result) {
	case V90_PHASE3_RETURN_RX_OK:
		return "ok";
	case V90_PHASE3_RETURN_RX_S_DETECTED:
		return "S detected";
	case V90_PHASE3_RETURN_RX_S_ENDED:
		return "S ended without S-bar";
	case V90_PHASE3_RETURN_RX_S_BAR_DETECTED:
		return "S-bar detected";
	case V90_PHASE3_RETURN_RX_COMPLETE:
		return "complete";
	case V90_PHASE3_RETURN_RX_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_PHASE3_RETURN_RX_INVALID_STATE:
		return "invalid state";
	case V90_PHASE3_RETURN_RX_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
