// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase3_sd_receiver.h"

#include "v90_info1.h"
#include "v90_phase3_sd.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static bool law_is_valid(enum g711_law law)
{
	return law == G711_LAW_PCMU || law == G711_LAW_PCMA;
}

static bool stage_is_valid(enum v90_phase3_sd_receiver_stage stage)
{
	return stage >= V90_PHASE3_SD_RECEIVER_SEARCH &&
	       stage <= V90_PHASE3_SD_RECEIVER_COMPLETE;
}

bool v90_phase3_sd_receiver_is_valid(
	const struct v90_phase3_sd_receiver *receiver)
{
	if (receiver == NULL || !law_is_valid(receiver->law) ||
	    !stage_is_valid(receiver->stage) ||
	    receiver->training_ucode < V90_INFO1A_MIN_TRAINING_UCODE ||
	    receiver->training_ucode > V90_INFO1A_MAX_TRAINING_UCODE ||
	    receiver->history_count > V90_PHASE3_SD_ACQUISITION_SYMBOLS ||
	    receiver->history_cursor >= V90_PHASE3_SD_ACQUISITION_SYMBOLS ||
	    receiver->next_phase >= V90_PHASE3_SD_PATTERN_SYMBOLS)
		return false;

	switch (receiver->stage) {
	case V90_PHASE3_SD_RECEIVER_SEARCH:
		return (receiver->history_count ==
				V90_PHASE3_SD_ACQUISITION_SYMBOLS ||
			receiver->history_cursor == receiver->history_count) &&
		       receiver->next_phase == 0 &&
		       receiver->bar_candidate_symbols == 0 &&
		       receiver->bar_symbols == 0 && receiver->bar_errors == 0;
	case V90_PHASE3_SD_RECEIVER_SD:
		return receiver->history_count ==
			       V90_PHASE3_SD_ACQUISITION_SYMBOLS &&
		       receiver->bar_candidate_symbols <
			       V90_PHASE3_SD_BAR_QUALIFY_SYMBOLS &&
		       receiver->bar_symbols == 0 && receiver->bar_errors == 0;
	case V90_PHASE3_SD_RECEIVER_SD_BAR:
		return receiver->history_count ==
			       V90_PHASE3_SD_ACQUISITION_SYMBOLS &&
		       receiver->bar_candidate_symbols == 0 &&
		       receiver->bar_symbols >=
			       V90_PHASE3_SD_BAR_QUALIFY_SYMBOLS &&
		       receiver->bar_symbols < V90_PHASE3_SD_BAR_SYMBOLS &&
		       receiver->bar_errors <= V90_PHASE3_SD_BAR_MAX_ERRORS;
	case V90_PHASE3_SD_RECEIVER_COMPLETE:
		return receiver->history_count ==
			       V90_PHASE3_SD_ACQUISITION_SYMBOLS &&
		       receiver->bar_candidate_symbols == 0 &&
		       receiver->bar_symbols == V90_PHASE3_SD_BAR_SYMBOLS &&
		       receiver->bar_errors <= V90_PHASE3_SD_BAR_MAX_ERRORS;
	}
	return false;
}

bool v90_phase3_sd_receiver_is_complete(
	const struct v90_phase3_sd_receiver *receiver)
{
	return v90_phase3_sd_receiver_is_valid(receiver) &&
	       receiver->stage == V90_PHASE3_SD_RECEIVER_COMPLETE;
}

enum v90_phase3_sd_receiver_result v90_phase3_sd_receiver_init(
	struct v90_phase3_sd_receiver *receiver, enum g711_law law,
	uint8_t training_ucode)
{
	struct v90_phase3_sd_receiver initialized = {
		.law = law,
		.stage = V90_PHASE3_SD_RECEIVER_SEARCH,
		.training_ucode = training_ucode,
	};

	if (receiver == NULL || !law_is_valid(law) ||
	    training_ucode < V90_INFO1A_MIN_TRAINING_UCODE ||
	    training_ucode > V90_INFO1A_MAX_TRAINING_UCODE)
		return V90_PHASE3_SD_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return V90_PHASE3_SD_RECEIVER_OK;
}

static bool symbol_matches(
	const struct v90_phase3_sd_receiver *receiver,
	const struct v90_pcm_symbol *symbol, uint8_t phase, bool inverted)
{
	uint8_t expected_ucode = phase == 1 || phase == 4 ? 0 :
		(uint8_t)((receiver->training_ucode +
			   V90_PHASE3_SD_TRAINING_OFFSET) & UINT8_C(0x7f));
	bool expected_positive = (phase < 3) != inverted;

	return symbol->ucode == expected_ucode &&
	       symbol->positive == expected_positive;
}

static void push_history(
	struct v90_phase3_sd_receiver *receiver, uint8_t network_octet)
{
	receiver->history[receiver->history_cursor] = network_octet;
	receiver->history_cursor++;
	if (receiver->history_cursor == V90_PHASE3_SD_ACQUISITION_SYMBOLS)
		receiver->history_cursor = 0;
	if (receiver->history_count < V90_PHASE3_SD_ACQUISITION_SYMBOLS)
		receiver->history_count++;
}

static bool find_sd_phase(
	const struct v90_phase3_sd_receiver *receiver, uint8_t *phase_out)
{
	unsigned int best_errors = V90_PHASE3_SD_ACQUISITION_SYMBOLS + 1u;
	unsigned int best_count = 0;
	uint8_t best_phase = 0;

	for (uint8_t phase = 0; phase < V90_PHASE3_SD_PATTERN_SYMBOLS;
	     ++phase) {
		unsigned int errors = 0;

		for (uint8_t offset = 0;
		     offset < V90_PHASE3_SD_ACQUISITION_SYMBOLS; ++offset) {
			struct v90_pcm_symbol symbol;
			uint8_t index = (uint8_t)(receiver->history_cursor + offset);

			if (index >= V90_PHASE3_SD_ACQUISITION_SYMBOLS)
				index -= V90_PHASE3_SD_ACQUISITION_SYMBOLS;
			if (!v90_pcm_symbol_parse(
				    &symbol, receiver->law,
				    receiver->history[index]) ||
			    !symbol_matches(
				    receiver, &symbol,
				    (uint8_t)((phase + offset) %
					      V90_PHASE3_SD_PATTERN_SYMBOLS),
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
	    best_errors > V90_PHASE3_SD_ACQUISITION_MAX_ERRORS)
		return false;
	*phase_out = best_phase;
	return true;
}

static enum v90_phase3_sd_receiver_result receive_search(
	struct v90_phase3_sd_receiver *receiver, uint8_t network_octet)
{
	uint8_t phase;

	push_history(receiver, network_octet);
	if (receiver->history_count < V90_PHASE3_SD_ACQUISITION_SYMBOLS ||
	    !find_sd_phase(receiver, &phase))
		return V90_PHASE3_SD_RECEIVER_OK;
	receiver->stage = V90_PHASE3_SD_RECEIVER_SD;
	receiver->next_phase = phase;
	return V90_PHASE3_SD_RECEIVER_SD_DETECTED;
}

static void advance_phase(struct v90_phase3_sd_receiver *receiver)
{
	receiver->next_phase++;
	if (receiver->next_phase == V90_PHASE3_SD_PATTERN_SYMBOLS)
		receiver->next_phase = 0;
}

static enum v90_phase3_sd_receiver_result receive_sd(
	struct v90_phase3_sd_receiver *receiver,
	const struct v90_pcm_symbol *symbol)
{
	if (symbol_matches(
		    receiver, symbol, receiver->next_phase, true))
		receiver->bar_candidate_symbols++;
	else
		receiver->bar_candidate_symbols = 0;
	advance_phase(receiver);
	if (receiver->bar_candidate_symbols <
	    V90_PHASE3_SD_BAR_QUALIFY_SYMBOLS)
		return V90_PHASE3_SD_RECEIVER_OK;
	receiver->stage = V90_PHASE3_SD_RECEIVER_SD_BAR;
	receiver->bar_symbols = receiver->bar_candidate_symbols;
	receiver->bar_candidate_symbols = 0;
	return V90_PHASE3_SD_RECEIVER_SD_BAR_DETECTED;
}

static enum v90_phase3_sd_receiver_result receive_sd_bar(
	struct v90_phase3_sd_receiver *receiver,
	const struct v90_pcm_symbol *symbol)
{
	if (!symbol_matches(
		    receiver, symbol, receiver->next_phase, true)) {
		if (receiver->bar_errors == V90_PHASE3_SD_BAR_MAX_ERRORS)
			return V90_PHASE3_SD_RECEIVER_INVALID_SEQUENCE;
		receiver->bar_errors++;
	}
	receiver->bar_symbols++;
	advance_phase(receiver);
	if (receiver->bar_symbols < V90_PHASE3_SD_BAR_SYMBOLS)
		return V90_PHASE3_SD_RECEIVER_OK;
	receiver->stage = V90_PHASE3_SD_RECEIVER_COMPLETE;
	return V90_PHASE3_SD_RECEIVER_SEQUENCE_COMPLETE;
}

enum v90_phase3_sd_receiver_result v90_phase3_sd_receiver_receive(
	struct v90_phase3_sd_receiver *receiver, uint8_t network_octet)
{
	struct v90_phase3_sd_receiver next;
	struct v90_pcm_symbol symbol;
	enum v90_phase3_sd_receiver_result result;

	if (receiver == NULL)
		return V90_PHASE3_SD_RECEIVER_INVALID_ARGUMENT;
	if (!v90_phase3_sd_receiver_is_valid(receiver))
		return V90_PHASE3_SD_RECEIVER_INVALID_STATE;
	if (receiver->stage == V90_PHASE3_SD_RECEIVER_COMPLETE)
		return V90_PHASE3_SD_RECEIVER_SEQUENCE_COMPLETE;
	next = *receiver;
	if (next.stage == V90_PHASE3_SD_RECEIVER_SEARCH)
		result = receive_search(&next, network_octet);
	else {
		if (!v90_pcm_symbol_parse(&symbol, next.law, network_octet))
			return V90_PHASE3_SD_RECEIVER_INVALID_STATE;
		result = next.stage == V90_PHASE3_SD_RECEIVER_SD ?
			 receive_sd(&next, &symbol) :
			 receive_sd_bar(&next, &symbol);
	}
	if (result != V90_PHASE3_SD_RECEIVER_INVALID_SEQUENCE)
		*receiver = next;
	return result;
}

const char *v90_phase3_sd_receiver_result_name(
	enum v90_phase3_sd_receiver_result result)
{
	switch (result) {
	case V90_PHASE3_SD_RECEIVER_OK:
		return "ok";
	case V90_PHASE3_SD_RECEIVER_SD_DETECTED:
		return "Sd detected";
	case V90_PHASE3_SD_RECEIVER_SD_BAR_DETECTED:
		return "S-bar detected";
	case V90_PHASE3_SD_RECEIVER_SEQUENCE_COMPLETE:
		return "sequence complete";
	case V90_PHASE3_SD_RECEIVER_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_PHASE3_SD_RECEIVER_INVALID_STATE:
		return "invalid state";
	case V90_PHASE3_SD_RECEIVER_INVALID_SEQUENCE:
		return "invalid sequence";
	}
	return "unknown";
}
