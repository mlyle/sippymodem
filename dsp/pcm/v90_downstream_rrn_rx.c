// SPDX-License-Identifier: BSD-3-Clause

#include "v90_downstream_rrn_rx.h"

#include "v90_pcm_symbol.h"

#include <stddef.h>
#include <string.h>

static bool plane_is_valid(enum v90_pcm_mapping_plane plane)
{
	return plane == V90_PCM_MAPPING_TRANSMIT ||
	       plane == V90_PCM_MAPPING_CODEC;
}

static bool stage_is_valid(enum v90_downstream_rrn_rx_stage stage)
{
	return stage >= V90_DOWNSTREAM_RRN_RX_SEARCH &&
	       stage <= V90_DOWNSTREAM_RRN_RX_COMPLETE;
}

static bool candidates_are_valid(
	const struct v90_downstream_rrn_rx *receiver, bool must_be_clear)
{
	for (size_t index = 0;
	     index < V90_DOWNSTREAM_RRN_RX_HYPOTHESES; ++index) {
		if (receiver->candidate_runs[index] >
		    V90_DOWNSTREAM_RRN_RX_MIN_RD_SYMBOLS)
			return false;
		if (must_be_clear && receiver->candidate_runs[index] != 0)
			return false;
	}
	return true;
}

bool v90_downstream_rrn_rx_is_valid(
	const struct v90_downstream_rrn_rx *receiver)
{
	if (receiver == NULL ||
	    !v90_pcm_mapping_is_valid(&receiver->mapping) ||
	    receiver->mapping.kind != V90_CP_DATA ||
	    !plane_is_valid(receiver->decision_plane) ||
	    !stage_is_valid(receiver->stage) ||
	    receiver->phase >= V90_CP_FRAME_INTERVALS ||
	    (receiver->locked_hypotheses &
	     ~((UINT16_C(1) << V90_DOWNSTREAM_RRN_RX_HYPOTHESES) - 1u)) !=
		    0 ||
	    receiver->rd_bar_symbols >
		    V90_DOWNSTREAM_RRN_RX_RD_BAR_SYMBOLS)
		return false;
	if (receiver->stage == V90_DOWNSTREAM_RRN_RX_SEARCH)
		return receiver->locked_hypotheses == 0 &&
		       receiver->rd_bar_symbols == 0 &&
		       candidates_are_valid(receiver, false);
	if (receiver->locked_hypotheses == 0 ||
	    !candidates_are_valid(receiver, true))
		return false;
	if (receiver->stage == V90_DOWNSTREAM_RRN_RX_RD)
		return receiver->rd_bar_symbols == 0;
	if (receiver->stage == V90_DOWNSTREAM_RRN_RX_RD_BAR)
		return receiver->rd_bar_symbols > 0 &&
		       receiver->rd_bar_symbols <
			       V90_DOWNSTREAM_RRN_RX_RD_BAR_SYMBOLS;
	return receiver->rd_bar_symbols ==
	       V90_DOWNSTREAM_RRN_RX_RD_BAR_SYMBOLS;
}

enum v90_downstream_rrn_rx_result v90_downstream_rrn_rx_init(
	struct v90_downstream_rrn_rx *receiver,
	const struct v90_pcm_mapping *current_data_mapping,
	enum v90_pcm_mapping_plane decision_plane)
{
	struct v90_downstream_rrn_rx initialized = { 0 };

	if (receiver == NULL || current_data_mapping == NULL)
		return V90_DOWNSTREAM_RRN_RX_INVALID_ARGUMENT;
	if (!v90_pcm_mapping_is_valid(current_data_mapping) ||
	    current_data_mapping->kind != V90_CP_DATA ||
	    !plane_is_valid(decision_plane))
		return V90_DOWNSTREAM_RRN_RX_INVALID_PARAMETERS;
	initialized.mapping = *current_data_mapping;
	initialized.decision_plane = decision_plane;
	initialized.stage = V90_DOWNSTREAM_RRN_RX_SEARCH;
	if (!v90_downstream_rrn_rx_is_valid(&initialized))
		return V90_DOWNSTREAM_RRN_RX_INTERNAL_ERROR;
	*receiver = initialized;
	return V90_DOWNSTREAM_RRN_RX_OK;
}

bool v90_downstream_rrn_rx_rd_detected(
	const struct v90_downstream_rrn_rx *receiver)
{
	return v90_downstream_rrn_rx_is_valid(receiver) &&
	       receiver->stage != V90_DOWNSTREAM_RRN_RX_SEARCH;
}

bool v90_downstream_rrn_rx_is_complete(
	const struct v90_downstream_rrn_rx *receiver)
{
	return v90_downstream_rrn_rx_is_valid(receiver) &&
	       receiver->stage == V90_DOWNSTREAM_RRN_RX_COMPLETE;
}

static bool symbol_matches(
	const struct v90_downstream_rrn_rx *receiver,
	const struct v90_pcm_symbol *symbol, uint8_t interval,
	bool inverted, bool bar, bool *matched)
{
	uint8_t expected_ucode;
	bool expected_positive =
		interval < V90_CP_FRAME_INTERVALS / 2u;

	if (v90_pcm_mapping_ucode(
		    &receiver->mapping, receiver->decision_plane,
		    interval, 0, &expected_ucode) != V90_PCM_MAPPING_OK)
		return false;
	if (inverted)
		expected_positive = !expected_positive;
	if (bar)
		expected_positive = !expected_positive;
	*matched = symbol->ucode == expected_ucode &&
		   symbol->positive == expected_positive;
	return true;
}

static void reset_search(struct v90_downstream_rrn_rx *receiver)
{
	memset(receiver->candidate_runs, 0,
	       sizeof(receiver->candidate_runs));
	receiver->stage = V90_DOWNSTREAM_RRN_RX_SEARCH;
	receiver->locked_hypotheses = 0;
	receiver->phase = 0;
	receiver->rd_bar_symbols = 0;
}

static enum v90_downstream_rrn_rx_result search(
	struct v90_downstream_rrn_rx *receiver,
	const struct v90_pcm_symbol *symbol)
{
	uint16_t saturated = 0;
	uint8_t phase = receiver->phase;

	for (size_t hypothesis = 0;
	     hypothesis < V90_DOWNSTREAM_RRN_RX_HYPOTHESES;
	     ++hypothesis) {
		uint8_t offset =
			(uint8_t)(hypothesis % V90_CP_FRAME_INTERVALS);
		uint8_t interval =
			(uint8_t)((phase + offset) %
				  V90_CP_FRAME_INTERVALS);
		bool matched;

		if (!symbol_matches(
			    receiver, symbol, interval,
			    hypothesis >= V90_CP_FRAME_INTERVALS,
			    false, &matched))
			return V90_DOWNSTREAM_RRN_RX_INTERNAL_ERROR;
		if (!matched) {
			receiver->candidate_runs[hypothesis] = 0;
			continue;
		}
		if (receiver->candidate_runs[hypothesis] <
		    V90_DOWNSTREAM_RRN_RX_MIN_RD_SYMBOLS)
			receiver->candidate_runs[hypothesis]++;
		if (receiver->candidate_runs[hypothesis] ==
		    V90_DOWNSTREAM_RRN_RX_MIN_RD_SYMBOLS)
			saturated |= (uint16_t)(UINT16_C(1) << hypothesis);
	}
	receiver->phase =
		(uint8_t)((phase + 1u) % V90_CP_FRAME_INTERVALS);
	if (saturated == 0)
		return V90_DOWNSTREAM_RRN_RX_BUFFERING;
	receiver->locked_hypotheses = saturated;
	receiver->stage = V90_DOWNSTREAM_RRN_RX_RD;
	memset(receiver->candidate_runs, 0,
	       sizeof(receiver->candidate_runs));
	return V90_DOWNSTREAM_RRN_RX_RD_DETECTED;
}

static void advance_phase(struct v90_downstream_rrn_rx *receiver)
{
	receiver->phase =
		(uint8_t)((receiver->phase + 1u) %
			  V90_CP_FRAME_INTERVALS);
}

static enum v90_downstream_rrn_rx_result restart_after_mismatch(
	struct v90_downstream_rrn_rx *receiver,
	const struct v90_pcm_symbol *symbol)
{
	reset_search(receiver);
	if (search(receiver, symbol) == V90_DOWNSTREAM_RRN_RX_INTERNAL_ERROR)
		return V90_DOWNSTREAM_RRN_RX_INTERNAL_ERROR;
	return V90_DOWNSTREAM_RRN_RX_SEQUENCE_MISMATCH;
}

static enum v90_downstream_rrn_rx_result receive_locked_rd(
	struct v90_downstream_rrn_rx *receiver,
	const struct v90_pcm_symbol *symbol)
{
	uint16_t rd = 0;
	uint16_t rd_bar = 0;

	for (size_t hypothesis = 0;
	     hypothesis < V90_DOWNSTREAM_RRN_RX_HYPOTHESES;
	     ++hypothesis) {
		uint16_t bit = (uint16_t)(UINT16_C(1) << hypothesis);
		uint8_t interval;
		bool matched;

		if ((receiver->locked_hypotheses & bit) == 0)
			continue;
		interval = (uint8_t)((receiver->phase +
				      hypothesis % V90_CP_FRAME_INTERVALS) %
				     V90_CP_FRAME_INTERVALS);
		if (!symbol_matches(
			    receiver, symbol, interval,
			    hypothesis >= V90_CP_FRAME_INTERVALS,
			    false, &matched))
			return V90_DOWNSTREAM_RRN_RX_INTERNAL_ERROR;
		if (matched)
			rd |= bit;
		if (interval != 0)
			continue;
		if (!symbol_matches(
			    receiver, symbol, interval,
			    hypothesis >= V90_CP_FRAME_INTERVALS,
			    true, &matched))
			return V90_DOWNSTREAM_RRN_RX_INTERNAL_ERROR;
		if (matched)
			rd_bar |= bit;
	}
	if (rd_bar != 0) {
		receiver->locked_hypotheses = rd_bar;
		receiver->stage = V90_DOWNSTREAM_RRN_RX_RD_BAR;
		receiver->rd_bar_symbols = 1;
		advance_phase(receiver);
		return V90_DOWNSTREAM_RRN_RX_RD_BAR_STARTED;
	}
	if (rd != 0) {
		receiver->locked_hypotheses = rd;
		advance_phase(receiver);
		return V90_DOWNSTREAM_RRN_RX_BUFFERING;
	}
	return restart_after_mismatch(receiver, symbol);
}

static enum v90_downstream_rrn_rx_result receive_rd_bar(
	struct v90_downstream_rrn_rx *receiver,
	const struct v90_pcm_symbol *symbol)
{
	uint16_t matched_hypotheses = 0;

	for (size_t hypothesis = 0;
	     hypothesis < V90_DOWNSTREAM_RRN_RX_HYPOTHESES;
	     ++hypothesis) {
		uint16_t bit = (uint16_t)(UINT16_C(1) << hypothesis);
		uint8_t interval;
		bool matched;

		if ((receiver->locked_hypotheses & bit) == 0)
			continue;
		interval = (uint8_t)((receiver->phase +
				      hypothesis % V90_CP_FRAME_INTERVALS) %
				     V90_CP_FRAME_INTERVALS);
		if (!symbol_matches(
			    receiver, symbol, interval,
			    hypothesis >= V90_CP_FRAME_INTERVALS,
			    true, &matched))
			return V90_DOWNSTREAM_RRN_RX_INTERNAL_ERROR;
		if (matched)
			matched_hypotheses |= bit;
	}
	if (matched_hypotheses == 0)
		return restart_after_mismatch(receiver, symbol);
	receiver->locked_hypotheses = matched_hypotheses;
	receiver->rd_bar_symbols++;
	advance_phase(receiver);
	if (receiver->rd_bar_symbols ==
	    V90_DOWNSTREAM_RRN_RX_RD_BAR_SYMBOLS) {
		receiver->stage = V90_DOWNSTREAM_RRN_RX_COMPLETE;
		return V90_DOWNSTREAM_RRN_RX_RD_BAR_COMPLETE;
	}
	return V90_DOWNSTREAM_RRN_RX_BUFFERING;
}

enum v90_downstream_rrn_rx_result v90_downstream_rrn_rx_receive(
	struct v90_downstream_rrn_rx *receiver, uint8_t network_octet)
{
	struct v90_downstream_rrn_rx next;
	struct v90_pcm_symbol symbol;
	enum v90_downstream_rrn_rx_result result;

	if (receiver == NULL)
		return V90_DOWNSTREAM_RRN_RX_INVALID_ARGUMENT;
	if (!v90_downstream_rrn_rx_is_valid(receiver) ||
	    receiver->stage == V90_DOWNSTREAM_RRN_RX_COMPLETE)
		return V90_DOWNSTREAM_RRN_RX_INVALID_STATE;
	if (!v90_pcm_symbol_parse(
		    &symbol, receiver->mapping.law, network_octet))
		return V90_DOWNSTREAM_RRN_RX_INVALID_PARAMETERS;
	next = *receiver;
	if (next.stage == V90_DOWNSTREAM_RRN_RX_SEARCH)
		result = search(&next, &symbol);
	else if (next.stage == V90_DOWNSTREAM_RRN_RX_RD)
		result = receive_locked_rd(&next, &symbol);
	else
		result = receive_rd_bar(&next, &symbol);
	if (result == V90_DOWNSTREAM_RRN_RX_INTERNAL_ERROR)
		return result;
	if (!v90_downstream_rrn_rx_is_valid(&next))
		return V90_DOWNSTREAM_RRN_RX_INTERNAL_ERROR;
	*receiver = next;
	return result;
}

const char *v90_downstream_rrn_rx_result_name(
	enum v90_downstream_rrn_rx_result result)
{
	switch (result) {
	case V90_DOWNSTREAM_RRN_RX_OK:
		return "ok";
	case V90_DOWNSTREAM_RRN_RX_BUFFERING:
		return "buffering";
	case V90_DOWNSTREAM_RRN_RX_RD_DETECTED:
		return "Rd detected";
	case V90_DOWNSTREAM_RRN_RX_RD_BAR_STARTED:
		return "Rd-bar started";
	case V90_DOWNSTREAM_RRN_RX_RD_BAR_COMPLETE:
		return "Rd-bar complete";
	case V90_DOWNSTREAM_RRN_RX_SEQUENCE_MISMATCH:
		return "sequence mismatch";
	case V90_DOWNSTREAM_RRN_RX_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_DOWNSTREAM_RRN_RX_INVALID_PARAMETERS:
		return "invalid parameters";
	case V90_DOWNSTREAM_RRN_RX_INVALID_STATE:
		return "invalid state";
	case V90_DOWNSTREAM_RRN_RX_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}
