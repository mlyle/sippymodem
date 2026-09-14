// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_s_pattern_rx.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { MIN_SAMPLED_LEVEL = 128, MAX_SAMPLED_LEVEL = 32768 };

static bool stage_is_valid(enum pcm_s_pattern_rx_stage stage)
{
	return stage >= PCM_S_PATTERN_RX_SEARCH &&
	       stage <= PCM_S_PATTERN_RX_COMPLETE;
}

bool pcm_s_pattern_rx_is_valid(
	const struct pcm_s_pattern_rx *receiver)
{
	if (receiver == NULL || !g711_law_is_valid(receiver->law) ||
	    !stage_is_valid(receiver->stage) ||
	    receiver->signal_ucode > 127u ||
	    receiver->history_count > PCM_S_PATTERN_ACQUISITION_SYMBOLS ||
	    receiver->history_cursor >= PCM_S_PATTERN_ACQUISITION_SYMBOLS ||
	    receiver->next_phase >= PCM_S_PATTERN_SYMBOLS ||
	    (receiver->level != 0 &&
	     (receiver->level < MIN_SAMPLED_LEVEL ||
	      receiver->level > MAX_SAMPLED_LEVEL)))
		return false;

	switch (receiver->stage) {
	case PCM_S_PATTERN_RX_SEARCH:
		return (receiver->history_count ==
				PCM_S_PATTERN_ACQUISITION_SYMBOLS ||
			receiver->history_cursor == receiver->history_count) &&
		       receiver->next_phase == 0 &&
		       receiver->bar_candidate_symbols == 0 &&
		       receiver->bar_symbols == 0 && receiver->bar_errors == 0;
	case PCM_S_PATTERN_RX_LOCKED:
		return receiver->history_count ==
			       PCM_S_PATTERN_ACQUISITION_SYMBOLS &&
		       receiver->bar_candidate_symbols <
			       PCM_S_PATTERN_BAR_QUALIFY_SYMBOLS &&
		       receiver->bar_symbols == 0 && receiver->bar_errors == 0;
	case PCM_S_PATTERN_RX_REVERSED:
		return receiver->history_count ==
			       PCM_S_PATTERN_ACQUISITION_SYMBOLS &&
		       receiver->bar_candidate_symbols == 0 &&
		       receiver->bar_symbols >=
			       PCM_S_PATTERN_BAR_QUALIFY_SYMBOLS &&
		       receiver->bar_symbols < PCM_S_PATTERN_BAR_SYMBOLS &&
		       receiver->bar_errors <= PCM_S_PATTERN_BAR_MAX_ERRORS;
	case PCM_S_PATTERN_RX_COMPLETE:
		return receiver->history_count ==
			       PCM_S_PATTERN_ACQUISITION_SYMBOLS &&
		       receiver->bar_candidate_symbols == 0 &&
		       receiver->bar_symbols == PCM_S_PATTERN_BAR_SYMBOLS &&
		       receiver->bar_errors <= PCM_S_PATTERN_BAR_MAX_ERRORS;
	}
	return false;
}

bool pcm_s_pattern_rx_is_complete(
	const struct pcm_s_pattern_rx *receiver)
{
	return pcm_s_pattern_rx_is_valid(receiver) &&
	       receiver->stage == PCM_S_PATTERN_RX_COMPLETE;
}

enum pcm_s_pattern_rx_result pcm_s_pattern_rx_init(
	struct pcm_s_pattern_rx *receiver, enum g711_law law,
	uint8_t signal_ucode)
{
	struct pcm_s_pattern_rx initialized = {
		.law = law,
		.stage = PCM_S_PATTERN_RX_SEARCH,
		.signal_ucode = signal_ucode,
	};

	if (receiver == NULL || !g711_law_is_valid(law) ||
	    signal_ucode > 127u)
		return PCM_S_PATTERN_RX_INVALID_ARGUMENT;
	*receiver = initialized;
	return PCM_S_PATTERN_RX_OK;
}

static bool symbol_matches(
	const struct pcm_s_pattern_rx *receiver,
	const struct v90_pcm_symbol *symbol, uint8_t phase, bool inverted)
{
	uint8_t expected_ucode = phase == 1 || phase == 4 ? 0 :
		receiver->signal_ucode;
	bool expected_positive = (phase < 3) != inverted;

	if (receiver->level != 0) {
		int32_t expected = phase == 1 || phase == 4 ? 0 :
			(expected_positive ? receiver->level : -receiver->level);
		int32_t error = symbol->sample - expected;

		/* A quarter-level window admits quantization and modest ISI,
		 * including residual energy in the zero slots. The six-phase
		 * template still rejects constant levels and single tones. */
		return error >= -receiver->level / 4 &&
		       error <= receiver->level / 4;
	}
	return symbol->ucode == expected_ucode &&
	       symbol->positive == expected_positive;
}

static int32_t acquired_level(const struct pcm_s_pattern_rx *receiver)
{
	int32_t magnitude = 0;

	for (size_t i = 0; i < PCM_S_PATTERN_ACQUISITION_SYMBOLS; ++i) {
		int32_t sample = receiver->law == G711_LAW_PCMU ?
			g711_decode_pcmu(receiver->history[i]) :
			g711_decode_pcma(receiver->history[i]);

		magnitude += sample < 0 ? -sample : sample;
	}
	/* Four of each six samples carry W. Averaging across three periods
	 * makes the level independent of the entry phase. Keep it fixed through
	 * S-bar so reversal, silence, and unrelated data cannot retune the test. */
	return magnitude / (PCM_S_PATTERN_ACQUISITION_SYMBOLS * 4 / 6);
}

static void push_history(
	struct pcm_s_pattern_rx *receiver, uint8_t network_octet)
{
	receiver->history[receiver->history_cursor] = network_octet;
	receiver->history_cursor++;
	if (receiver->history_cursor == PCM_S_PATTERN_ACQUISITION_SYMBOLS)
		receiver->history_cursor = 0;
	if (receiver->history_count < PCM_S_PATTERN_ACQUISITION_SYMBOLS)
		receiver->history_count++;
}

static bool find_phase(
	const struct pcm_s_pattern_rx *receiver, uint8_t *phase_out)
{
	unsigned int best_errors = PCM_S_PATTERN_ACQUISITION_SYMBOLS + 1u;
	unsigned int best_count = 0;
	uint8_t best_phase = 0;

	for (uint8_t phase = 0; phase < PCM_S_PATTERN_SYMBOLS;
	     ++phase) {
		unsigned int errors = 0;

		for (uint8_t offset = 0;
		     offset < PCM_S_PATTERN_ACQUISITION_SYMBOLS; ++offset) {
			struct v90_pcm_symbol symbol;
			uint8_t index = (uint8_t)(receiver->history_cursor + offset);

			if (index >= PCM_S_PATTERN_ACQUISITION_SYMBOLS)
				index -= PCM_S_PATTERN_ACQUISITION_SYMBOLS;
			if (!v90_pcm_symbol_parse(
				    &symbol, receiver->law,
				    receiver->history[index]) ||
			    !symbol_matches(
				    receiver, &symbol,
				    (uint8_t)((phase + offset) %
					      PCM_S_PATTERN_SYMBOLS),
				    false))
				errors++;
		}
		if (errors < best_errors) {
			best_errors = errors;
			best_phase = phase;
			best_count = 1;
		} else if (errors == best_errors) {
			best_count++;
		}
	}
	if (best_count != 1 ||
	    best_errors > PCM_S_PATTERN_ACQUISITION_MAX_ERRORS)
		return false;
	*phase_out = best_phase;
	return true;
}

static enum pcm_s_pattern_rx_result receive_search(
	struct pcm_s_pattern_rx *receiver, uint8_t network_octet)
{
	uint8_t phase;

	push_history(receiver, network_octet);
	if (receiver->history_count < PCM_S_PATTERN_ACQUISITION_SYMBOLS)
		return PCM_S_PATTERN_RX_OK;
	receiver->level = acquired_level(receiver);
	if (receiver->level > MAX_SAMPLED_LEVEL) {
		receiver->level = 0;
		return PCM_S_PATTERN_RX_OK;
	}
	if (receiver->level < MIN_SAMPLED_LEVEL)
		receiver->level = 0;
	if (!find_phase(receiver, &phase))
		return PCM_S_PATTERN_RX_OK;
	receiver->stage = PCM_S_PATTERN_RX_LOCKED;
	receiver->next_phase = phase;
	return PCM_S_PATTERN_RX_ACQUIRED;
}

static void advance_phase(struct pcm_s_pattern_rx *receiver)
{
	receiver->next_phase++;
	if (receiver->next_phase == PCM_S_PATTERN_SYMBOLS)
		receiver->next_phase = 0;
}

static enum pcm_s_pattern_rx_result receive_locked(
	struct pcm_s_pattern_rx *receiver,
	const struct v90_pcm_symbol *symbol)
{
	if (symbol_matches(
		    receiver, symbol, receiver->next_phase, true))
		receiver->bar_candidate_symbols++;
	else
		receiver->bar_candidate_symbols = 0;
	advance_phase(receiver);
	if (receiver->bar_candidate_symbols <
	    PCM_S_PATTERN_BAR_QUALIFY_SYMBOLS)
		return PCM_S_PATTERN_RX_OK;
	receiver->stage = PCM_S_PATTERN_RX_REVERSED;
	receiver->bar_symbols = receiver->bar_candidate_symbols;
	receiver->bar_candidate_symbols = 0;
	return PCM_S_PATTERN_RX_REVERSAL_DETECTED;
}

static enum pcm_s_pattern_rx_result receive_reversed(
	struct pcm_s_pattern_rx *receiver,
	const struct v90_pcm_symbol *symbol)
{
	if (!symbol_matches(
		    receiver, symbol, receiver->next_phase, true)) {
		if (receiver->bar_errors == PCM_S_PATTERN_BAR_MAX_ERRORS)
			return PCM_S_PATTERN_RX_INVALID_SEQUENCE;
		receiver->bar_errors++;
	}
	receiver->bar_symbols++;
	advance_phase(receiver);
	if (receiver->bar_symbols < PCM_S_PATTERN_BAR_SYMBOLS)
		return PCM_S_PATTERN_RX_OK;
	receiver->stage = PCM_S_PATTERN_RX_COMPLETE;
	return PCM_S_PATTERN_RX_SEQUENCE_COMPLETE;
}

enum pcm_s_pattern_rx_result pcm_s_pattern_rx_receive(
	struct pcm_s_pattern_rx *receiver, uint8_t network_octet)
{
	struct pcm_s_pattern_rx next;
	struct v90_pcm_symbol symbol;
	enum pcm_s_pattern_rx_result result;

	if (receiver == NULL)
		return PCM_S_PATTERN_RX_INVALID_ARGUMENT;
	if (!pcm_s_pattern_rx_is_valid(receiver))
		return PCM_S_PATTERN_RX_INVALID_STATE;
	if (receiver->stage == PCM_S_PATTERN_RX_COMPLETE)
		return PCM_S_PATTERN_RX_SEQUENCE_COMPLETE;
	next = *receiver;
	if (next.stage == PCM_S_PATTERN_RX_SEARCH)
		result = receive_search(&next, network_octet);
	else {
		if (!v90_pcm_symbol_parse(&symbol, next.law, network_octet))
			return PCM_S_PATTERN_RX_INVALID_STATE;
		result = next.stage == PCM_S_PATTERN_RX_LOCKED ?
			 receive_locked(&next, &symbol) :
			 receive_reversed(&next, &symbol);
	}
	if (result != PCM_S_PATTERN_RX_INVALID_SEQUENCE)
		*receiver = next;
	return result;
}

const char *pcm_s_pattern_rx_result_name(
	enum pcm_s_pattern_rx_result result)
{
	switch (result) {
	case PCM_S_PATTERN_RX_OK:
		return "ok";
	case PCM_S_PATTERN_RX_ACQUIRED:
		return "pattern acquired";
	case PCM_S_PATTERN_RX_REVERSAL_DETECTED:
		return "reversal detected";
	case PCM_S_PATTERN_RX_SEQUENCE_COMPLETE:
		return "sequence complete";
	case PCM_S_PATTERN_RX_INVALID_ARGUMENT:
		return "invalid argument";
	case PCM_S_PATTERN_RX_INVALID_STATE:
		return "invalid state";
	case PCM_S_PATTERN_RX_INVALID_SEQUENCE:
		return "invalid sequence";
	}
	return "unknown";
}
