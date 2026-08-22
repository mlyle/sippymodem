// SPDX-License-Identifier: BSD-3-Clause

#ifndef V32_CONDITIONING_H
#define V32_CONDITIONING_H

#include "framing.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V32_CONDITIONING_S_SYMBOLS = 256,
	V32_CONDITIONING_S_BAR_SYMBOLS = 16,
	V32_CONDITIONING_TRN_PREFIX_SYMBOLS = 256,
	V32_CONDITIONING_TRN_MIN_SYMBOLS = 1280,
	V32_CONDITIONING_TRN_MAX_SYMBOLS = 8192,
	V32_ANSWERING_SECOND_TRN_DATA_SYMBOLS = 7100,
	V32_ANSWERING_SECOND_TRN_SYMBOLS =
		V32_CONDITIONING_TRN_PREFIX_SYMBOLS +
		V32_ANSWERING_SECOND_TRN_DATA_SYMBOLS,
};

enum v32_conditioning_result {
	V32_CONDITIONING_OK,
	V32_CONDITIONING_COMPLETE,
	V32_CONDITIONING_INVALID_ARGUMENT,
};

struct v32_conditioning_transmitter {
	struct v32_symbol_encoder trn_encoder;
	uint16_t emitted_symbols;
	uint16_t trn_symbols;
};

enum v32_conditioning_result
v32_conditioning_transmitter_init(
	struct v32_conditioning_transmitter *transmitter,
	enum v32_transmitter_role role, uint16_t trn_symbols);
bool v32_conditioning_transmitter_is_valid(
	const struct v32_conditioning_transmitter *transmitter);
bool v32_conditioning_transmitter_is_complete(
	const struct v32_conditioning_transmitter *transmitter);

/*
 * Emit one A/B/C/D quadrant from S, S-bar, and TRN. During the first 256
 * symbols of TRN, only the first scrambled bit selects A or C; subsequent
 * scrambled dibits use the ordinary direct TRN mapping. Rejected and complete
 * calls leave both the transmitter and output unchanged.
 */
enum v32_conditioning_result
v32_conditioning_transmitter_next(
	struct v32_conditioning_transmitter *transmitter,
	uint8_t *quadrant);

/* The final TRN state initializes differential coding of the rate signal. */
enum v32_conditioning_result
v32_conditioning_transmitter_framing_state(
	const struct v32_conditioning_transmitter *transmitter,
	struct v32_symbol_encoder *encoder);

#endif
