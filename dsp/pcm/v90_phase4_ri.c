// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase4_ri.h"

#include "v90_info1.h"
#include "v90_pcm_symbol.h"

#include <stddef.h>

static bool ucode_is_valid(uint8_t ucode)
{
	return ucode >= V90_INFO1A_MIN_TRAINING_UCODE &&
	       ucode <= V90_INFO1A_MAX_TRAINING_UCODE;
}

static bool common_state_is_valid(
	enum v90_phase4_ri_stage stage, enum g711_law law,
	uint16_t ri_symbols, uint8_t ri_bar_symbols,
	uint8_t phase, uint8_t training_ucode)
{
	if (!g711_law_is_valid(law) || !ucode_is_valid(training_ucode) ||
	    stage < V90_PHASE4_RI || stage > V90_PHASE4_RI_COMPLETE ||
	    ri_symbols > V90_PHASE4_RI_MIN_SYMBOLS ||
	    ri_bar_symbols > V90_PHASE4_RI_BAR_SYMBOLS ||
	    phase >= V90_PHASE4_RI_PERIOD)
		return false;
	if (stage == V90_PHASE4_RI)
		return ri_bar_symbols == 0 &&
		       (ri_symbols == V90_PHASE4_RI_MIN_SYMBOLS ||
			ri_symbols % V90_PHASE4_RI_PERIOD == phase);
	if (stage == V90_PHASE4_RI_BAR)
		return ri_symbols == V90_PHASE4_RI_MIN_SYMBOLS &&
		       ri_bar_symbols > 0 &&
		       ri_bar_symbols < V90_PHASE4_RI_BAR_SYMBOLS &&
		       ri_bar_symbols % V90_PHASE4_RI_PERIOD == phase;
	return ri_symbols == V90_PHASE4_RI_MIN_SYMBOLS &&
	       ri_bar_symbols == V90_PHASE4_RI_BAR_SYMBOLS && phase == 0;
}

bool v90_phase4_ri_tx_is_valid(const struct v90_phase4_ri_tx *tx)
{
	return tx != NULL &&
	       common_state_is_valid(
		       tx->stage, tx->law, tx->ri_symbols,
		       tx->ri_bar_symbols, tx->phase,
		       tx->training_ucode) &&
	       (tx->stage == V90_PHASE4_RI || tx->finish_requested);
}

bool v90_phase4_ri_tx_is_complete(const struct v90_phase4_ri_tx *tx)
{
	return v90_phase4_ri_tx_is_valid(tx) &&
	       tx->stage == V90_PHASE4_RI_COMPLETE;
}

enum v90_phase4_ri_result v90_phase4_ri_tx_init(
	struct v90_phase4_ri_tx *tx, enum g711_law law,
	uint8_t training_ucode)
{
	struct v90_phase4_ri_tx initialized = {
		.stage = V90_PHASE4_RI,
		.law = law,
		.training_ucode = training_ucode,
	};

	if (tx == NULL || !v90_phase4_ri_tx_is_valid(&initialized))
		return V90_PHASE4_RI_INVALID_ARGUMENT;
	*tx = initialized;
	return V90_PHASE4_RI_OK;
}

enum v90_phase4_ri_result v90_phase4_ri_tx_request_finish(
	struct v90_phase4_ri_tx *tx)
{
	if (!v90_phase4_ri_tx_is_valid(tx))
		return V90_PHASE4_RI_INVALID_ARGUMENT;
	if (v90_phase4_ri_tx_is_complete(tx))
		return V90_PHASE4_RI_DONE;
	tx->finish_requested = true;
	return V90_PHASE4_RI_OK;
}

static bool nominal_sign(enum v90_phase4_ri_stage stage, uint8_t phase)
{
	return stage == V90_PHASE4_RI ? phase < 3u : phase >= 3u;
}

static void advance_phase(uint8_t *phase)
{
	*phase = (uint8_t)((*phase + 1u) % V90_PHASE4_RI_PERIOD);
}

enum v90_phase4_ri_result v90_phase4_ri_tx_next(
	struct v90_phase4_ri_tx *tx, uint8_t *network_octet)
{
	struct v90_phase4_ri_tx next;
	struct v90_pcm_symbol symbol;

	if (!v90_phase4_ri_tx_is_valid(tx) || network_octet == NULL)
		return V90_PHASE4_RI_INVALID_ARGUMENT;
	if (v90_phase4_ri_tx_is_complete(tx))
		return V90_PHASE4_RI_DONE;
	next = *tx;
	if (next.stage == V90_PHASE4_RI && next.finish_requested &&
	    next.ri_symbols == V90_PHASE4_RI_MIN_SYMBOLS &&
	    next.phase == 0)
		next.stage = V90_PHASE4_RI_BAR;
	if (!v90_pcm_symbol_make(
		    &symbol, next.law, next.training_ucode,
		    nominal_sign(next.stage, next.phase)))
		return V90_PHASE4_RI_INVALID_STATE;
	advance_phase(&next.phase);
	if (next.stage == V90_PHASE4_RI) {
		if (next.ri_symbols < V90_PHASE4_RI_MIN_SYMBOLS)
			next.ri_symbols++;
	} else {
		next.ri_bar_symbols++;
		if (next.ri_bar_symbols == V90_PHASE4_RI_BAR_SYMBOLS)
			next.stage = V90_PHASE4_RI_COMPLETE;
	}
	if (!v90_phase4_ri_tx_is_valid(&next))
		return V90_PHASE4_RI_INVALID_STATE;
	*tx = next;
	*network_octet = symbol.octet;
	return V90_PHASE4_RI_OK;
}

bool v90_phase4_ri_rx_is_valid(const struct v90_phase4_ri_rx *rx)
{
	if (rx == NULL ||
	    !common_state_is_valid(
		    rx->stage, rx->law, rx->ri_symbols,
		    rx->ri_bar_symbols, rx->phase,
		    rx->training_ucode))
		return false;
	if (!rx->polarity_known)
		return rx->stage == V90_PHASE4_RI && rx->ri_symbols == 0 &&
		       rx->phase == 0 && !rx->inverted;
	return rx->ri_symbols != 0;
}

bool v90_phase4_ri_rx_is_complete(const struct v90_phase4_ri_rx *rx)
{
	return v90_phase4_ri_rx_is_valid(rx) &&
	       rx->stage == V90_PHASE4_RI_COMPLETE;
}

enum v90_phase4_ri_result v90_phase4_ri_rx_init(
	struct v90_phase4_ri_rx *rx, enum g711_law law,
	uint8_t training_ucode)
{
	struct v90_phase4_ri_rx initialized = {
		.stage = V90_PHASE4_RI,
		.law = law,
		.training_ucode = training_ucode,
	};

	if (rx == NULL || !v90_phase4_ri_rx_is_valid(&initialized))
		return V90_PHASE4_RI_INVALID_ARGUMENT;
	*rx = initialized;
	return V90_PHASE4_RI_OK;
}

static bool expected_sign(
	const struct v90_phase4_ri_rx *rx,
	enum v90_phase4_ri_stage stage)
{
	return nominal_sign(stage, rx->phase) != rx->inverted;
}

static enum v90_phase4_ri_result receive_ri(
	struct v90_phase4_ri_rx *rx, bool positive)
{
	if (!rx->polarity_known) {
		rx->inverted = positive != nominal_sign(V90_PHASE4_RI, 0);
		rx->polarity_known = true;
	}
	if (positive == expected_sign(rx, V90_PHASE4_RI)) {
		advance_phase(&rx->phase);
		if (rx->ri_symbols < V90_PHASE4_RI_MIN_SYMBOLS)
			rx->ri_symbols++;
		return V90_PHASE4_RI_OK;
	}
	if (rx->ri_symbols != V90_PHASE4_RI_MIN_SYMBOLS ||
	    rx->phase != 0 ||
	    positive != expected_sign(rx, V90_PHASE4_RI_BAR))
		return V90_PHASE4_RI_INVALID_SEQUENCE;
	rx->stage = V90_PHASE4_RI_BAR;
	rx->ri_bar_symbols = 1;
	advance_phase(&rx->phase);
	return V90_PHASE4_RI_TRANSITION_DETECTED;
}

static enum v90_phase4_ri_result receive_ri_bar(
	struct v90_phase4_ri_rx *rx, bool positive)
{
	if (positive != expected_sign(rx, V90_PHASE4_RI_BAR))
		return V90_PHASE4_RI_INVALID_SEQUENCE;
	rx->ri_bar_symbols++;
	advance_phase(&rx->phase);
	if (rx->ri_bar_symbols < V90_PHASE4_RI_BAR_SYMBOLS)
		return V90_PHASE4_RI_OK;
	rx->stage = V90_PHASE4_RI_COMPLETE;
	return V90_PHASE4_RI_DONE;
}

enum v90_phase4_ri_result v90_phase4_ri_rx_receive(
	struct v90_phase4_ri_rx *rx, uint8_t network_octet)
{
	struct v90_phase4_ri_rx next;
	struct v90_pcm_symbol symbol;
	enum v90_phase4_ri_result result;

	if (!v90_phase4_ri_rx_is_valid(rx))
		return V90_PHASE4_RI_INVALID_ARGUMENT;
	if (v90_phase4_ri_rx_is_complete(rx))
		return V90_PHASE4_RI_DONE;
	if (!v90_pcm_symbol_parse(&symbol, rx->law, network_octet) ||
	    symbol.ucode != rx->training_ucode)
		return V90_PHASE4_RI_INVALID_SEQUENCE;
	next = *rx;
	result = next.stage == V90_PHASE4_RI ?
			 receive_ri(&next, symbol.positive) :
			 receive_ri_bar(&next, symbol.positive);
	if (result == V90_PHASE4_RI_INVALID_SEQUENCE)
		return result;
	if (!v90_phase4_ri_rx_is_valid(&next))
		return V90_PHASE4_RI_INVALID_STATE;
	*rx = next;
	return result;
}

const char *v90_phase4_ri_result_name(enum v90_phase4_ri_result result)
{
	switch (result) {
	case V90_PHASE4_RI_OK:
		return "ok";
	case V90_PHASE4_RI_TRANSITION_DETECTED:
		return "Ri-bar detected";
	case V90_PHASE4_RI_DONE:
		return "done";
	case V90_PHASE4_RI_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_PHASE4_RI_INVALID_STATE:
		return "invalid state";
	case V90_PHASE4_RI_INVALID_SEQUENCE:
		return "invalid sequence";
	}
	return "unknown";
}
