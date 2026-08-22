// SPDX-License-Identifier: BSD-3-Clause

#include "conditioning.h"

#include <stddef.h>

enum {
	V32_SIGNAL_A = 0,
	V32_SIGNAL_B = 1,
	V32_SIGNAL_C = 2,
	V32_SIGNAL_D = 3,
	V32_CONDITIONING_FIXED_SYMBOLS =
		V32_CONDITIONING_S_SYMBOLS +
		V32_CONDITIONING_S_BAR_SYMBOLS,
};

static bool role_is_valid(enum v32_transmitter_role role)
{
	return role == V32_CALLING_TRANSMITTER ||
	       role == V32_ANSWERING_TRANSMITTER;
}

static uint32_t total_symbols(
	const struct v32_conditioning_transmitter *transmitter)
{
	return V32_CONDITIONING_FIXED_SYMBOLS + transmitter->trn_symbols;
}

bool v32_conditioning_transmitter_is_valid(
	const struct v32_conditioning_transmitter *transmitter)
{
	if (transmitter == NULL ||
	    transmitter->trn_symbols <
		    V32_CONDITIONING_TRN_MIN_SYMBOLS ||
	    transmitter->trn_symbols >
		    V32_CONDITIONING_TRN_MAX_SYMBOLS ||
	    transmitter->emitted_symbols > total_symbols(transmitter) ||
	    transmitter->trn_encoder.phase_quadrant >=
		    QPSK_QUADRANT_COUNT ||
	    !v32_scrambler_is_valid(
		    &transmitter->trn_encoder.scrambler))
		return false;
	if (transmitter->emitted_symbols <= V32_CONDITIONING_FIXED_SYMBOLS)
		return transmitter->trn_encoder.scrambler.history == 0 &&
		       transmitter->trn_encoder.phase_quadrant == V32_SIGNAL_A;
	return true;
}

bool v32_conditioning_transmitter_is_complete(
	const struct v32_conditioning_transmitter *transmitter)
{
	return v32_conditioning_transmitter_is_valid(transmitter) &&
	       transmitter->emitted_symbols == total_symbols(transmitter);
}

enum v32_conditioning_result
v32_conditioning_transmitter_init(
	struct v32_conditioning_transmitter *transmitter,
	enum v32_transmitter_role role, uint16_t trn_symbols)
{
	struct v32_conditioning_transmitter initialized = {
		.trn_symbols = trn_symbols,
	};

	if (transmitter == NULL || !role_is_valid(role) ||
	    trn_symbols < V32_CONDITIONING_TRN_MIN_SYMBOLS ||
	    trn_symbols > V32_CONDITIONING_TRN_MAX_SYMBOLS ||
	    v32_symbol_encoder_init(
		    &initialized.trn_encoder, role, V32_SIGNAL_A) !=
		    V32_FRAMING_OK ||
	    !v32_conditioning_transmitter_is_valid(&initialized))
		return V32_CONDITIONING_INVALID_ARGUMENT;
	*transmitter = initialized;
	return V32_CONDITIONING_OK;
}

static enum v32_conditioning_result emit_trn_symbol(
	struct v32_conditioning_transmitter *transmitter,
	uint16_t trn_symbol, uint8_t *quadrant)
{
	if (trn_symbol < V32_CONDITIONING_TRN_PREFIX_SYMBOLS) {
		uint8_t scrambled;

		if (v32_scramble_dibit(
			    &transmitter->trn_encoder.scrambler, 3,
			    &scrambled) != V32_FRAMING_OK)
			return V32_CONDITIONING_INVALID_ARGUMENT;
		*quadrant = (scrambled & 2u) != 0 ? V32_SIGNAL_C : V32_SIGNAL_A;
		transmitter->trn_encoder.phase_quadrant = *quadrant;
		return V32_CONDITIONING_OK;
	}
	return v32_trn_symbol_encode(
		       &transmitter->trn_encoder, 3, quadrant) ==
		       V32_FRAMING_OK ?
		       V32_CONDITIONING_OK :
		       V32_CONDITIONING_INVALID_ARGUMENT;
}

enum v32_conditioning_result
v32_conditioning_transmitter_next(
	struct v32_conditioning_transmitter *transmitter,
	uint8_t *quadrant)
{
	struct v32_conditioning_transmitter next;
	uint8_t next_quadrant;

	if (!v32_conditioning_transmitter_is_valid(transmitter) ||
	    quadrant == NULL)
		return V32_CONDITIONING_INVALID_ARGUMENT;
	if (v32_conditioning_transmitter_is_complete(transmitter))
		return V32_CONDITIONING_COMPLETE;
	next = *transmitter;
	if (next.emitted_symbols < V32_CONDITIONING_S_SYMBOLS) {
		next_quadrant = (next.emitted_symbols & 1u) != 0 ?
					V32_SIGNAL_B : V32_SIGNAL_A;
	} else if (next.emitted_symbols < V32_CONDITIONING_FIXED_SYMBOLS) {
		uint16_t symbol = (uint16_t)(
			next.emitted_symbols -
			V32_CONDITIONING_S_SYMBOLS);

		next_quadrant = (symbol & 1u) != 0 ?
					V32_SIGNAL_D : V32_SIGNAL_C;
	} else {
		uint16_t symbol = (uint16_t)(
			next.emitted_symbols - V32_CONDITIONING_FIXED_SYMBOLS);

		if (emit_trn_symbol(&next, symbol, &next_quadrant) !=
		    V32_CONDITIONING_OK)
			return V32_CONDITIONING_INVALID_ARGUMENT;
	}
	next.emitted_symbols++;
	if (!v32_conditioning_transmitter_is_valid(&next))
		return V32_CONDITIONING_INVALID_ARGUMENT;
	*transmitter = next;
	*quadrant = next_quadrant;
	return V32_CONDITIONING_OK;
}

enum v32_conditioning_result
v32_conditioning_transmitter_framing_state(
	const struct v32_conditioning_transmitter *transmitter,
	struct v32_symbol_encoder *encoder)
{
	if (!v32_conditioning_transmitter_is_complete(transmitter) ||
	    encoder == NULL)
		return V32_CONDITIONING_INVALID_ARGUMENT;
	*encoder = transmitter->trn_encoder;
	return V32_CONDITIONING_OK;
}
