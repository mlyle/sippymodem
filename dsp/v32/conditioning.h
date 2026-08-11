// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V32_CONDITIONING_H
#define SIPPYMODEM_V32_CONDITIONING_H

#include "framing.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V32_CONDITIONING_S_SYMBOLS = 256,
	SIPPYMODEM_V32_CONDITIONING_S_BAR_SYMBOLS = 16,
	SIPPYMODEM_V32_CONDITIONING_TRN_PREFIX_SYMBOLS = 256,
	SIPPYMODEM_V32_CONDITIONING_TRN_MIN_SYMBOLS = 1280,
	SIPPYMODEM_V32_CONDITIONING_TRN_MAX_SYMBOLS = 8192,
};

enum sippymodem_v32_conditioning_result {
	SIPPYMODEM_V32_CONDITIONING_OK,
	SIPPYMODEM_V32_CONDITIONING_COMPLETE,
	SIPPYMODEM_V32_CONDITIONING_INVALID_ARGUMENT,
};

struct sippymodem_v32_conditioning_transmitter {
	struct sippymodem_v32_symbol_encoder trn_encoder;
	uint16_t emitted_symbols;
	uint16_t trn_symbols;
};

enum sippymodem_v32_conditioning_result
sippymodem_v32_conditioning_transmitter_init(
	struct sippymodem_v32_conditioning_transmitter *transmitter,
	enum sippymodem_v32_transmitter_role role, uint16_t trn_symbols);
bool sippymodem_v32_conditioning_transmitter_is_valid(
	const struct sippymodem_v32_conditioning_transmitter *transmitter);
bool sippymodem_v32_conditioning_transmitter_is_complete(
	const struct sippymodem_v32_conditioning_transmitter *transmitter);

/*
 * Emit one A/B/C/D quadrant from S, S-bar, and TRN. During the first 256
 * symbols of TRN, only the first scrambled bit selects A or C; subsequent
 * scrambled dibits use the ordinary direct TRN mapping. Rejected and complete
 * calls leave both the transmitter and output unchanged.
 */
enum sippymodem_v32_conditioning_result
sippymodem_v32_conditioning_transmitter_next(
	struct sippymodem_v32_conditioning_transmitter *transmitter,
	uint8_t *quadrant);

/* The final TRN state initializes differential coding of the rate signal. */
enum sippymodem_v32_conditioning_result
sippymodem_v32_conditioning_transmitter_framing_state(
	const struct sippymodem_v32_conditioning_transmitter *transmitter,
	struct sippymodem_v32_symbol_encoder *encoder);

const char *sippymodem_v32_conditioning_result_name(
	enum sippymodem_v32_conditioning_result result);

#endif
