// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_r.h"

#include "v90_pcm_symbol.h"
#include "pcm_level_mean.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

static bool mapping_matches_signal(const struct v90_pcm_mapping *mapping,
	enum pcm_r_signal signal)
{
	return (signal == PCM_R_RD || signal == PCM_R_RT || signal == PCM_R_RF) &&
		v90_pcm_mapping_is_valid(mapping) &&
		mapping->kind == (signal == PCM_R_RT ? V90_CP_TRAINING : V90_CP_DATA);
}

static unsigned signal_period(enum pcm_r_signal signal)
{
	return signal == PCM_R_RF ? 12u : V90_CP_FRAME_INTERVALS;
}

static bool signal_positive(enum pcm_r_signal signal, unsigned phase)
{
	return signal == PCM_R_RF ? phase % 4u < 2u :
		phase % V90_CP_FRAME_INTERVALS < V90_CP_FRAME_INTERVALS / 2u;
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
		if (!isfinite(receiver->mean_error[index]) ||
		    fabs(receiver->mean_error[index]) > 1.0 ||
		    ((must_be_clear ?
		      !(receiver->locked_hypotheses & (UINT16_C(1) << index)) :
		      receiver->candidate_runs[index] == 0) &&
		     receiver->mean_error[index] != 0.0))
			return false;
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
	    !mapping_matches_signal(&receiver->mapping, receiver->signal) ||
	    !v90_pcm_mapping_plane_is_valid(receiver->decision_plane) ||
	    !stage_is_valid(receiver->stage) ||
	    receiver->phase >= signal_period(receiver->signal) ||
	    (receiver->locked_hypotheses &
	     ~((UINT16_C(1) << PCM_R_RX_HYPOTHESES) - 1u)) !=
		    0 ||
	    receiver->bar_symbols >
		    PCM_R_RX_BAR_SYMBOLS)
		return false;
	if (receiver->stage == PCM_R_RX_SEARCH)
		return receiver->locked_hypotheses == 0 &&
		       receiver->bar_symbols == 0 &&
		       candidates_are_valid(receiver, false);
	if (receiver->locked_hypotheses == 0 ||
	    !candidates_are_valid(receiver, true))
		return false;
	if (receiver->stage == PCM_R_RX_SIGNAL)
		return receiver->bar_symbols == 0;
	if (receiver->stage == PCM_R_RX_BAR)
		return receiver->bar_symbols > 0 &&
		       receiver->bar_symbols <
			       PCM_R_RX_BAR_SYMBOLS;
	return receiver->bar_symbols ==
	       PCM_R_RX_BAR_SYMBOLS;
}

enum pcm_r_rx_result pcm_r_rx_init(
	struct pcm_r_rx *receiver,
	const struct v90_pcm_mapping *mapping,
	enum v90_pcm_mapping_plane decision_plane, enum pcm_r_signal signal)
{
	struct pcm_r_rx initialized = { 0 };

	if (receiver == NULL || mapping == NULL)
		return PCM_R_RX_INVALID_ARGUMENT;
	if (!mapping_matches_signal(mapping, signal) || !v90_pcm_mapping_plane_is_valid(decision_plane))
		return PCM_R_RX_INVALID_PARAMETERS;
	initialized.mapping = *mapping;
	initialized.decision_plane = decision_plane;
	initialized.signal = signal;
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
	unsigned hypothesis, bool bar, bool *matched, double *mean_error)
{
	uint8_t expected_ucode;
	const struct pcm_constellation *plane =
		receiver->decision_plane == V90_PCM_MAPPING_CODEC ?
			receiver->mapping.codec : receiver->mapping.transmit;
	bool expected_positive = signal_positive(receiver->signal, interval);

	/* The caller validated the mapping before testing any hypotheses. */
	if (!pcm_constellation_descending_select(
		    &plane[interval % V90_CP_FRAME_INTERVALS], 0,
		    &expected_ucode))
		return false;
	if (hypothesis >= V90_CP_FRAME_INTERVALS)
		expected_positive = !expected_positive;
	if (bar)
		expected_positive = !expected_positive;
	*matched = false;
	*mean_error = 0.0;
	if (symbol->positive != expected_positive)
		return true;
	/* Anchor each new hypothesis at the mapped level, then average ripple.
	 * Also keep the first reversed character exact: it anchors the frame
	 * phase and has the preceding R character's polarity, without an edge. */
	if (symbol->ucode != expected_ucode &&
	    ((receiver->stage == PCM_R_RX_SEARCH &&
	      receiver->candidate_runs[hypothesis] == 0) ||
	     (bar && receiver->stage == PCM_R_RX_SIGNAL)))
		return true;
	if (symbol->ucode == expected_ucode &&
	    receiver->mean_error[hypothesis] == 0.0) {
		*matched = true;
		return true;
	}
	struct v90_pcm_symbol expected;
	struct g711_interval cell;
	if (!v90_pcm_symbol_make(&expected, receiver->mapping.law,
		    expected_ucode, expected_positive) ||
	    !g711_octet_interval(receiver->mapping.law, expected.octet, &cell))
		return false;
	/* Normalize by the expected cell width, never by an observed gain.
	 * The zero prior is the fixed mapping target. Wrong polarity, amplitude
	 * failure and discarded hypotheses clear it; silence cannot train it.
	 * Constant adjacent levels still fail the full 240-symbol requirement. */
	double polarity = expected_positive ? 1.0 : -1.0;
	double width = (double)cell.upper - cell.lower + 1.0;
	double error = polarity * ((double)symbol->sample - expected.sample) / width;
	double lower = (expected_positive ? cell.lower - (double)expected.sample :
		expected.sample - (double)cell.upper) / width;
	double upper = (expected_positive ? cell.upper - (double)expected.sample :
		expected.sample - (double)cell.lower) / width;
	*mean_error = pcm_level_mean(receiver->mean_error[hypothesis], error);
	*matched = *mean_error >= lower && *mean_error <= upper;
	return true;
}

static void retain_hypotheses(struct pcm_r_rx *receiver, uint16_t mask)
{
	receiver->locked_hypotheses = mask;
	for (unsigned i = 0; i < PCM_R_RX_HYPOTHESES; ++i)
		if (!(mask & (UINT16_C(1) << i)))
			receiver->mean_error[i] = 0.0;
}

static void reset_search(struct pcm_r_rx *receiver)
{
	memset(receiver->candidate_runs, 0,
	       sizeof(receiver->candidate_runs));
	memset(receiver->mean_error, 0, sizeof(receiver->mean_error));
	receiver->stage = PCM_R_RX_SEARCH;
	receiver->locked_hypotheses = 0;
	receiver->phase = 0;
	receiver->bar_symbols = 0;
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
				  signal_period(receiver->signal));
		bool matched;
		double mean_error;

		if (!symbol_matches(
			    receiver, symbol, interval,
			    hypothesis, false, &matched, &mean_error))
			return PCM_R_RX_INTERNAL_ERROR;
		if (!matched) {
			receiver->candidate_runs[hypothesis] = 0;
			receiver->mean_error[hypothesis] = 0.0;
			continue;
		}
		receiver->mean_error[hypothesis] = mean_error;
		if (receiver->candidate_runs[hypothesis] <
		    PCM_R_RX_MIN_SYMBOLS)
			receiver->candidate_runs[hypothesis]++;
		if (receiver->candidate_runs[hypothesis] ==
		    PCM_R_RX_MIN_SYMBOLS)
			saturated |= (uint16_t)(UINT16_C(1) << hypothesis);
	}
	receiver->phase =
		(uint8_t)((phase + 1u) % signal_period(receiver->signal));
	if (saturated == 0)
		return PCM_R_RX_BUFFERING;
	retain_hypotheses(receiver, saturated);
	receiver->stage = PCM_R_RX_SIGNAL;
	memset(receiver->candidate_runs, 0,
	       sizeof(receiver->candidate_runs));
	return PCM_R_RX_SIGNAL_DETECTED;
}

static void advance_phase(struct pcm_r_rx *receiver)
{
	receiver->phase =
		(uint8_t)((receiver->phase + 1u) %
			  signal_period(receiver->signal));
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

static enum pcm_r_rx_result receive_locked_r(
	struct pcm_r_rx *receiver,
	const struct v90_pcm_symbol *symbol)
{
	uint16_t r = 0;
	uint16_t bar = 0;

	for (size_t hypothesis = 0;
	     hypothesis < PCM_R_RX_HYPOTHESES;
	     ++hypothesis) {
		uint16_t bit = (uint16_t)(UINT16_C(1) << hypothesis);
		uint8_t interval;
		bool matched;
		double observed, mean_error = 0.0;

		if ((receiver->locked_hypotheses & bit) == 0)
			continue;
		interval = (uint8_t)((receiver->phase +
				      hypothesis % V90_CP_FRAME_INTERVALS) %
				     signal_period(receiver->signal));
		if (!symbol_matches(
			    receiver, symbol, interval,
			    hypothesis, false, &matched, &observed))
			return PCM_R_RX_INTERNAL_ERROR;
		if (matched) {
			r |= bit;
			mean_error = observed;
		}
		if (interval % V90_CP_FRAME_INTERVALS == 0) {
			if (!symbol_matches(receiver, symbol, interval,
				    hypothesis, true, &matched, &observed))
				return PCM_R_RX_INTERNAL_ERROR;
			if (matched) {
				bar |= bit;
				mean_error = observed;
			}
		}
		receiver->mean_error[hypothesis] = mean_error;
	}
	if (bar != 0) {
		retain_hypotheses(receiver, bar);
		receiver->stage = PCM_R_RX_BAR;
		receiver->bar_symbols = 1;
		advance_phase(receiver);
		return PCM_R_RX_BAR_STARTED;
	}
	if (r != 0) {
		retain_hypotheses(receiver, r);
		advance_phase(receiver);
		return PCM_R_RX_BUFFERING;
	}
	return restart_after_mismatch(receiver, symbol);
}

static enum pcm_r_rx_result receive_r_bar(
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
		double mean_error;

		if ((receiver->locked_hypotheses & bit) == 0)
			continue;
		interval = (uint8_t)((receiver->phase +
				      hypothesis % V90_CP_FRAME_INTERVALS) %
				     signal_period(receiver->signal));
		if (!symbol_matches(
			    receiver, symbol, interval,
			    hypothesis, true, &matched, &mean_error))
			return PCM_R_RX_INTERNAL_ERROR;
		if (matched) {
			matched_hypotheses |= bit;
			receiver->mean_error[hypothesis] = mean_error;
		}
	}
	if (matched_hypotheses == 0)
		return restart_after_mismatch(receiver, symbol);
	retain_hypotheses(receiver, matched_hypotheses);
	receiver->bar_symbols++;
	advance_phase(receiver);
	if (receiver->bar_symbols ==
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
		result = receive_locked_r(&next, &symbol);
	else
		result = receive_r_bar(&next, &symbol);
	if (result == PCM_R_RX_INTERNAL_ERROR)
		return result;
	if (!pcm_r_rx_is_valid(&next))
		return PCM_R_RX_INTERNAL_ERROR;
	*receiver = next;
	return result;
}

bool pcm_r_tx_is_valid(const struct pcm_r_tx *transmitter)
{
	return transmitter != NULL &&
	       mapping_matches_signal(&transmitter->mapping, transmitter->signal) &&
	       transmitter->transmitted_symbols <= PCM_R_SEQUENCE_SYMBOLS;
}

bool pcm_r_tx_is_complete(const struct pcm_r_tx *transmitter)
{
	return pcm_r_tx_is_valid(transmitter) &&
	       transmitter->transmitted_symbols == PCM_R_SEQUENCE_SYMBOLS;
}

enum pcm_r_tx_result pcm_r_tx_init(
	struct pcm_r_tx *transmitter,
	const struct v90_pcm_mapping *mapping, enum pcm_r_signal signal)
{
	struct pcm_r_tx initialized = { 0 };

	if (transmitter == NULL || mapping == NULL)
		return PCM_R_TX_INVALID_ARGUMENT;
	if (!mapping_matches_signal(mapping, signal))
		return PCM_R_TX_INVALID_PARAMETERS;
	initialized.mapping = *mapping;
	initialized.signal = signal;
	if (!pcm_r_tx_is_valid(&initialized))
		return PCM_R_TX_INTERNAL_ERROR;
	*transmitter = initialized;
	return PCM_R_TX_OK;
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
	/* The transmitter was validated at entry. */
	if (!pcm_constellation_descending_select(
		    &next.mapping.transmit[interval], 0, &ucode))
		return PCM_R_TX_INTERNAL_ERROR;
	/* Descending-Ucode mapping makes label zero the normative
	 * highest-power constellation member used by every R signal. */
	positive = signal_positive(next.signal, next.transmitted_symbols);
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
