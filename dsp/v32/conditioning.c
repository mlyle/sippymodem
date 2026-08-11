// SPDX-License-Identifier: BSD-3-Clause

#include "conditioning.h"

#include <stddef.h>

enum {
	V32_SIGNAL_A = 0,
	V32_SIGNAL_B = 1,
	V32_SIGNAL_C = 2,
	V32_SIGNAL_D = 3,
	V32_CONDITIONING_FIXED_SYMBOLS =
		SIPPYMODEM_V32_CONDITIONING_S_SYMBOLS +
		SIPPYMODEM_V32_CONDITIONING_S_BAR_SYMBOLS,
};

static bool role_is_valid(enum sippymodem_v32_transmitter_role role)
{
	return role == SIPPYMODEM_V32_CALLING_TRANSMITTER ||
	       role == SIPPYMODEM_V32_ANSWERING_TRANSMITTER;
}

static uint32_t total_symbols(
	const struct sippymodem_v32_conditioning_transmitter *transmitter)
{
	return V32_CONDITIONING_FIXED_SYMBOLS + transmitter->trn_symbols;
}

bool sippymodem_v32_conditioning_transmitter_is_valid(
	const struct sippymodem_v32_conditioning_transmitter *transmitter)
{
	if (transmitter == NULL ||
	    transmitter->trn_symbols <
		    SIPPYMODEM_V32_CONDITIONING_TRN_MIN_SYMBOLS ||
	    transmitter->trn_symbols >
		    SIPPYMODEM_V32_CONDITIONING_TRN_MAX_SYMBOLS ||
	    transmitter->emitted_symbols > total_symbols(transmitter) ||
	    transmitter->trn_encoder.phase_quadrant >=
		    SIPPYMODEM_QPSK_QUADRANT_COUNT ||
	    !sippymodem_v32_scrambler_is_valid(
		    &transmitter->trn_encoder.scrambler))
		return false;
	if (transmitter->emitted_symbols <= V32_CONDITIONING_FIXED_SYMBOLS)
		return transmitter->trn_encoder.scrambler.history == 0 &&
		       transmitter->trn_encoder.phase_quadrant == V32_SIGNAL_A;
	return true;
}

bool sippymodem_v32_conditioning_transmitter_is_complete(
	const struct sippymodem_v32_conditioning_transmitter *transmitter)
{
	return sippymodem_v32_conditioning_transmitter_is_valid(transmitter) &&
	       transmitter->emitted_symbols == total_symbols(transmitter);
}

enum sippymodem_v32_conditioning_result
sippymodem_v32_conditioning_transmitter_init(
	struct sippymodem_v32_conditioning_transmitter *transmitter,
	enum sippymodem_v32_transmitter_role role, uint16_t trn_symbols)
{
	struct sippymodem_v32_conditioning_transmitter initialized = {
		.trn_symbols = trn_symbols,
	};

	if (transmitter == NULL || !role_is_valid(role) ||
	    trn_symbols < SIPPYMODEM_V32_CONDITIONING_TRN_MIN_SYMBOLS ||
	    trn_symbols > SIPPYMODEM_V32_CONDITIONING_TRN_MAX_SYMBOLS ||
	    sippymodem_v32_symbol_encoder_init(
		    &initialized.trn_encoder, role, V32_SIGNAL_A) !=
		    SIPPYMODEM_V32_FRAMING_OK ||
	    !sippymodem_v32_conditioning_transmitter_is_valid(&initialized))
		return SIPPYMODEM_V32_CONDITIONING_INVALID_ARGUMENT;
	*transmitter = initialized;
	return SIPPYMODEM_V32_CONDITIONING_OK;
}

static enum sippymodem_v32_conditioning_result emit_trn_symbol(
	struct sippymodem_v32_conditioning_transmitter *transmitter,
	uint16_t trn_symbol, uint8_t *quadrant)
{
	if (trn_symbol < SIPPYMODEM_V32_CONDITIONING_TRN_PREFIX_SYMBOLS) {
		uint8_t scrambled;

		if (sippymodem_v32_scramble_dibit(
			    &transmitter->trn_encoder.scrambler, 3,
			    &scrambled) != SIPPYMODEM_V32_FRAMING_OK)
			return SIPPYMODEM_V32_CONDITIONING_INVALID_ARGUMENT;
		*quadrant = (scrambled & 2u) != 0 ? V32_SIGNAL_C : V32_SIGNAL_A;
		transmitter->trn_encoder.phase_quadrant = *quadrant;
		return SIPPYMODEM_V32_CONDITIONING_OK;
	}
	return sippymodem_v32_trn_symbol_encode(
		       &transmitter->trn_encoder, 3, quadrant) ==
		       SIPPYMODEM_V32_FRAMING_OK ?
		       SIPPYMODEM_V32_CONDITIONING_OK :
		       SIPPYMODEM_V32_CONDITIONING_INVALID_ARGUMENT;
}

enum sippymodem_v32_conditioning_result
sippymodem_v32_conditioning_transmitter_next(
	struct sippymodem_v32_conditioning_transmitter *transmitter,
	uint8_t *quadrant)
{
	struct sippymodem_v32_conditioning_transmitter next;
	uint8_t next_quadrant;

	if (!sippymodem_v32_conditioning_transmitter_is_valid(transmitter) ||
	    quadrant == NULL)
		return SIPPYMODEM_V32_CONDITIONING_INVALID_ARGUMENT;
	if (sippymodem_v32_conditioning_transmitter_is_complete(transmitter))
		return SIPPYMODEM_V32_CONDITIONING_COMPLETE;
	next = *transmitter;
	if (next.emitted_symbols < SIPPYMODEM_V32_CONDITIONING_S_SYMBOLS) {
		next_quadrant = (next.emitted_symbols & 1u) != 0 ?
					V32_SIGNAL_B : V32_SIGNAL_A;
	} else if (next.emitted_symbols < V32_CONDITIONING_FIXED_SYMBOLS) {
		uint16_t symbol = (uint16_t)(
			next.emitted_symbols -
			SIPPYMODEM_V32_CONDITIONING_S_SYMBOLS);

		next_quadrant = (symbol & 1u) != 0 ?
					V32_SIGNAL_D : V32_SIGNAL_C;
	} else {
		uint16_t symbol = (uint16_t)(
			next.emitted_symbols - V32_CONDITIONING_FIXED_SYMBOLS);

		if (emit_trn_symbol(&next, symbol, &next_quadrant) !=
		    SIPPYMODEM_V32_CONDITIONING_OK)
			return SIPPYMODEM_V32_CONDITIONING_INVALID_ARGUMENT;
	}
	next.emitted_symbols++;
	if (!sippymodem_v32_conditioning_transmitter_is_valid(&next))
		return SIPPYMODEM_V32_CONDITIONING_INVALID_ARGUMENT;
	*transmitter = next;
	*quadrant = next_quadrant;
	return SIPPYMODEM_V32_CONDITIONING_OK;
}

enum sippymodem_v32_conditioning_result
sippymodem_v32_conditioning_transmitter_framing_state(
	const struct sippymodem_v32_conditioning_transmitter *transmitter,
	struct sippymodem_v32_symbol_encoder *encoder)
{
	if (!sippymodem_v32_conditioning_transmitter_is_complete(transmitter) ||
	    encoder == NULL)
		return SIPPYMODEM_V32_CONDITIONING_INVALID_ARGUMENT;
	*encoder = transmitter->trn_encoder;
	return SIPPYMODEM_V32_CONDITIONING_OK;
}

const char *sippymodem_v32_conditioning_result_name(
	enum sippymodem_v32_conditioning_result result)
{
	switch (result) {
	case SIPPYMODEM_V32_CONDITIONING_OK:
		return "ok";
	case SIPPYMODEM_V32_CONDITIONING_COMPLETE:
		return "complete";
	case SIPPYMODEM_V32_CONDITIONING_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
