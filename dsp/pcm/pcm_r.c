// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_r.h"

#include "v90_pcm_symbol.h"

#include <stddef.h>
#include <string.h>

static bool plane_is_valid(enum v90_pcm_mapping_plane plane)
{
	return plane == V90_PCM_MAPPING_TRANSMIT ||
	       plane == V90_PCM_MAPPING_CODEC;
}

static bool stage_is_valid(enum pcm_r_rx_stage stage)
{
	return stage >= PCM_R_RX_SEARCH &&
	       stage <= PCM_R_RX_COMPLETE;
}

static bool candidates_are_valid(
	const struct pcm_r_rx *receiver, bool must_be_clear)
{
	for (size_t index = 0;
	     index < PCM_R_RX_HYPOTHESES; ++index) {
		if (receiver->candidate_runs[index] >
		    PCM_R_RX_MIN_SYMBOLS)
			return false;
		if (must_be_clear && receiver->candidate_runs[index] != 0)
			return false;
	}
	return true;
}

bool pcm_r_rx_is_valid(
	const struct pcm_r_rx *receiver)
{
	if (receiver == NULL ||
	    !v90_pcm_mapping_is_valid(&receiver->mapping) ||
	    receiver->mapping.kind != V90_CP_DATA ||
	    !plane_is_valid(receiver->decision_plane) ||
	    !stage_is_valid(receiver->stage) ||
	    receiver->phase >= V90_CP_FRAME_INTERVALS ||
	    (receiver->locked_hypotheses &
	     ~((UINT16_C(1) << PCM_R_RX_HYPOTHESES) - 1u)) !=
		    0 ||
	    receiver->rd_bar_symbols >
		    PCM_R_RX_BAR_SYMBOLS)
		return false;
	if (receiver->stage == PCM_R_RX_SEARCH)
		return receiver->locked_hypotheses == 0 &&
		       receiver->rd_bar_symbols == 0 &&
		       candidates_are_valid(receiver, false);
	if (receiver->locked_hypotheses == 0 ||
	    !candidates_are_valid(receiver, true))
		return false;
	if (receiver->stage == PCM_R_RX_SIGNAL)
		return receiver->rd_bar_symbols == 0;
	if (receiver->stage == PCM_R_RX_BAR)
		return receiver->rd_bar_symbols > 0 &&
		       receiver->rd_bar_symbols <
			       PCM_R_RX_BAR_SYMBOLS;
	return receiver->rd_bar_symbols ==
	       PCM_R_RX_BAR_SYMBOLS;
}

enum pcm_r_rx_result pcm_r_rx_init(
	struct pcm_r_rx *receiver,
	const struct v90_pcm_mapping *current_data_mapping,
	enum v90_pcm_mapping_plane decision_plane)
{
	struct pcm_r_rx initialized = { 0 };

	if (receiver == NULL || current_data_mapping == NULL)
		return PCM_R_RX_INVALID_ARGUMENT;
	if (!v90_pcm_mapping_is_valid(current_data_mapping) ||
	    current_data_mapping->kind != V90_CP_DATA ||
	    !plane_is_valid(decision_plane))
		return PCM_R_RX_INVALID_PARAMETERS;
	initialized.mapping = *current_data_mapping;
	initialized.decision_plane = decision_plane;
	initialized.stage = PCM_R_RX_SEARCH;
	if (!pcm_r_rx_is_valid(&initialized))
		return PCM_R_RX_INTERNAL_ERROR;
	*receiver = initialized;
	return PCM_R_RX_OK;
}

bool pcm_r_rx_signal_detected(
	const struct pcm_r_rx *receiver)
{
	return pcm_r_rx_is_valid(receiver) &&
	       receiver->stage != PCM_R_RX_SEARCH;
}

bool pcm_r_rx_is_complete(
	const struct pcm_r_rx *receiver)
{
	return pcm_r_rx_is_valid(receiver) &&
	       receiver->stage == PCM_R_RX_COMPLETE;
}

static bool symbol_matches(
	const struct pcm_r_rx *receiver,
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

static void reset_search(struct pcm_r_rx *receiver)
{
	memset(receiver->candidate_runs, 0,
	       sizeof(receiver->candidate_runs));
	receiver->stage = PCM_R_RX_SEARCH;
	receiver->locked_hypotheses = 0;
	receiver->phase = 0;
	receiver->rd_bar_symbols = 0;
}

static enum pcm_r_rx_result search(
	struct pcm_r_rx *receiver,
	const struct v90_pcm_symbol *symbol)
{
	uint16_t saturated = 0;
	uint8_t phase = receiver->phase;

	for (size_t hypothesis = 0;
	     hypothesis < PCM_R_RX_HYPOTHESES;
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
			return PCM_R_RX_INTERNAL_ERROR;
		if (!matched) {
			receiver->candidate_runs[hypothesis] = 0;
			continue;
		}
		if (receiver->candidate_runs[hypothesis] <
		    PCM_R_RX_MIN_SYMBOLS)
			receiver->candidate_runs[hypothesis]++;
		if (receiver->candidate_runs[hypothesis] ==
		    PCM_R_RX_MIN_SYMBOLS)
			saturated |= (uint16_t)(UINT16_C(1) << hypothesis);
	}
	receiver->phase =
		(uint8_t)((phase + 1u) % V90_CP_FRAME_INTERVALS);
	if (saturated == 0)
		return PCM_R_RX_BUFFERING;
	receiver->locked_hypotheses = saturated;
	receiver->stage = PCM_R_RX_SIGNAL;
	memset(receiver->candidate_runs, 0,
	       sizeof(receiver->candidate_runs));
	return PCM_R_RX_SIGNAL_DETECTED;
}

static void advance_phase(struct pcm_r_rx *receiver)
{
	receiver->phase =
		(uint8_t)((receiver->phase + 1u) %
			  V90_CP_FRAME_INTERVALS);
}

static enum pcm_r_rx_result restart_after_mismatch(
	struct pcm_r_rx *receiver,
	const struct v90_pcm_symbol *symbol)
{
	reset_search(receiver);
	if (search(receiver, symbol) == PCM_R_RX_INTERNAL_ERROR)
		return PCM_R_RX_INTERNAL_ERROR;
	return PCM_R_RX_SEQUENCE_MISMATCH;
}

static enum pcm_r_rx_result receive_locked_rd(
	struct pcm_r_rx *receiver,
	const struct v90_pcm_symbol *symbol)
{
	uint16_t rd = 0;
	uint16_t rd_bar = 0;

	for (size_t hypothesis = 0;
	     hypothesis < PCM_R_RX_HYPOTHESES;
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
			return PCM_R_RX_INTERNAL_ERROR;
		if (matched)
			rd |= bit;
		if (interval != 0)
			continue;
		if (!symbol_matches(
			    receiver, symbol, interval,
			    hypothesis >= V90_CP_FRAME_INTERVALS,
			    true, &matched))
			return PCM_R_RX_INTERNAL_ERROR;
		if (matched)
			rd_bar |= bit;
	}
	if (rd_bar != 0) {
		receiver->locked_hypotheses = rd_bar;
		receiver->stage = PCM_R_RX_BAR;
		receiver->rd_bar_symbols = 1;
		advance_phase(receiver);
		return PCM_R_RX_BAR_STARTED;
	}
	if (rd != 0) {
		receiver->locked_hypotheses = rd;
		advance_phase(receiver);
		return PCM_R_RX_BUFFERING;
	}
	return restart_after_mismatch(receiver, symbol);
}

static enum pcm_r_rx_result receive_rd_bar(
	struct pcm_r_rx *receiver,
	const struct v90_pcm_symbol *symbol)
{
	uint16_t matched_hypotheses = 0;

	for (size_t hypothesis = 0;
	     hypothesis < PCM_R_RX_HYPOTHESES;
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
			return PCM_R_RX_INTERNAL_ERROR;
		if (matched)
			matched_hypotheses |= bit;
	}
	if (matched_hypotheses == 0)
		return restart_after_mismatch(receiver, symbol);
	receiver->locked_hypotheses = matched_hypotheses;
	receiver->rd_bar_symbols++;
	advance_phase(receiver);
	if (receiver->rd_bar_symbols ==
	    PCM_R_RX_BAR_SYMBOLS) {
		receiver->stage = PCM_R_RX_COMPLETE;
		return PCM_R_RX_BAR_COMPLETE;
	}
	return PCM_R_RX_BUFFERING;
}

enum pcm_r_rx_result pcm_r_rx_receive(
	struct pcm_r_rx *receiver, uint8_t network_octet)
{
	struct pcm_r_rx next;
	struct v90_pcm_symbol symbol;
	enum pcm_r_rx_result result;

	if (receiver == NULL)
		return PCM_R_RX_INVALID_ARGUMENT;
	if (!pcm_r_rx_is_valid(receiver) ||
	    receiver->stage == PCM_R_RX_COMPLETE)
		return PCM_R_RX_INVALID_STATE;
	if (!v90_pcm_symbol_parse(
		    &symbol, receiver->mapping.law, network_octet))
		return PCM_R_RX_INVALID_PARAMETERS;
	next = *receiver;
	if (next.stage == PCM_R_RX_SEARCH)
		result = search(&next, &symbol);
	else if (next.stage == PCM_R_RX_SIGNAL)
		result = receive_locked_rd(&next, &symbol);
	else
		result = receive_rd_bar(&next, &symbol);
	if (result == PCM_R_RX_INTERNAL_ERROR)
		return result;
	if (!pcm_r_rx_is_valid(&next))
		return PCM_R_RX_INTERNAL_ERROR;
	*receiver = next;
	return result;
}

const char *pcm_r_rx_result_name(
	enum pcm_r_rx_result result)
{
	switch (result) {
	case PCM_R_RX_OK:
		return "ok";
	case PCM_R_RX_BUFFERING:
		return "buffering";
	case PCM_R_RX_SIGNAL_DETECTED:
		return "Rd detected";
	case PCM_R_RX_BAR_STARTED:
		return "Rd-bar started";
	case PCM_R_RX_BAR_COMPLETE:
		return "Rd-bar complete";
	case PCM_R_RX_SEQUENCE_MISMATCH:
		return "sequence mismatch";
	case PCM_R_RX_INVALID_ARGUMENT:
		return "invalid argument";
	case PCM_R_RX_INVALID_PARAMETERS:
		return "invalid parameters";
	case PCM_R_RX_INVALID_STATE:
		return "invalid state";
	case PCM_R_RX_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}

bool pcm_r_tx_is_valid(const struct pcm_r_tx *transmitter)
{
	return transmitter != NULL &&
	       v90_pcm_mapping_is_valid(&transmitter->mapping) &&
	       transmitter->transmitted_symbols <= PCM_R_SEQUENCE_SYMBOLS;
}

bool pcm_r_tx_is_complete(const struct pcm_r_tx *transmitter)
{
	return pcm_r_tx_is_valid(transmitter) &&
	       transmitter->transmitted_symbols == PCM_R_SEQUENCE_SYMBOLS;
}

static enum pcm_r_tx_result r_tx_init(
	struct pcm_r_tx *transmitter,
	const struct v90_pcm_mapping *mapping, enum v90_cp_kind kind)
{
	struct pcm_r_tx initialized = { 0 };

	if (transmitter == NULL || mapping == NULL)
		return PCM_R_TX_INVALID_ARGUMENT;
	if (!v90_pcm_mapping_is_valid(mapping) || mapping->kind != kind)
		return PCM_R_TX_INVALID_PARAMETERS;
	initialized.mapping = *mapping;
	if (!pcm_r_tx_is_valid(&initialized))
		return PCM_R_TX_INTERNAL_ERROR;
	*transmitter = initialized;
	return PCM_R_TX_OK;
}

enum pcm_r_tx_result pcm_rd_tx_init(
	struct pcm_r_tx *transmitter,
	const struct v90_pcm_mapping *current_data_mapping)
{
	return r_tx_init(transmitter, current_data_mapping, V90_CP_DATA);
}

enum pcm_r_tx_result pcm_rt_tx_init(
	struct pcm_r_tx *transmitter,
	const struct v90_pcm_mapping *training_mapping)
{
	return r_tx_init(transmitter, training_mapping, V90_CP_TRAINING);
}

enum pcm_r_tx_result pcm_r_tx_next(
	struct pcm_r_tx *transmitter, struct v90_pcm_symbol *symbol)
{
	struct pcm_r_tx next;
	struct v90_pcm_symbol generated;
	size_t interval;
	uint8_t ucode;
	bool positive;

	if (!pcm_r_tx_is_valid(transmitter) || symbol == NULL)
		return PCM_R_TX_INVALID_ARGUMENT;
	if (pcm_r_tx_is_complete(transmitter))
		return PCM_R_TX_COMPLETE;
	next = *transmitter;
	interval = next.transmitted_symbols % V90_CP_FRAME_INTERVALS;
	if (v90_pcm_mapping_ucode(
		    &next.mapping, V90_PCM_MAPPING_TRANSMIT,
		    interval, 0, &ucode) != V90_PCM_MAPPING_OK)
		return PCM_R_TX_INTERNAL_ERROR;
	/* Descending-Ucode mapping makes label zero the normative
	 * highest-power constellation member used by Rd and Rt. */
	positive = interval < V90_CP_FRAME_INTERVALS / 2u;
	if (next.transmitted_symbols >= PCM_R_SYMBOLS)
		positive = !positive;
	if (!v90_pcm_symbol_make(
		    &generated, next.mapping.law, ucode, positive))
		return PCM_R_TX_INTERNAL_ERROR;
	next.transmitted_symbols++;
	if (!pcm_r_tx_is_valid(&next))
		return PCM_R_TX_INTERNAL_ERROR;
	*transmitter = next;
	*symbol = generated;
	return PCM_R_TX_OK;
}
