// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_SIGN_DECODER_H
#define V90_SIGN_DECODER_H

#include "v90_spectral_shaper.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum v90_sign_decoder_result {
	V90_SIGN_DECODER_OK,
	V90_SIGN_DECODER_INVALID_ARGUMENT,
	V90_SIGN_DECODER_INVALID_STATE,
	V90_SIGN_DECODER_INVALID_SIGNS,
};

/* The active prefix of bits contains S recovered sign-input bits. Rules are
 * present only when spectral shaping is enabled, one per shaping frame. */
struct v90_sign_decode_output {
	uint8_t bits[V90_CP_FRAME_INTERVALS];
	enum v90_spectral_rule rules[3];
	uint8_t bit_count;
	uint8_t rule_count;
};

/* Own the inverse differential histories and spectral-shaper trellis state.
 * All histories begin at zero for TRN2d and each newly initialized data mode.
 */
struct v90_sign_decoder {
	uint8_t previous_shaping_signs[V90_CP_FRAME_INTERVALS];
	uint8_t previous_odd;
	uint8_t previous_pcm_sign;
	uint8_t sign_redundancy;
	bool trellis_state;
};

enum v90_sign_decoder_result v90_sign_decoder_init(
	struct v90_sign_decoder *decoder, uint8_t sign_redundancy);
bool v90_sign_decoder_is_valid(
	const struct v90_sign_decoder *decoder);

/* Consume the six final PCM signs from one data frame. The first sign in each
 * shaping frame identifies the only allowed inversion rule relative to the
 * retained t(0) history. Rejected calls change neither state nor output. */
enum v90_sign_decoder_result v90_sign_decoder_decode(
	struct v90_sign_decoder *decoder,
	const uint8_t signs[V90_CP_FRAME_INTERVALS], size_t sign_count,
	struct v90_sign_decode_output *output);

const char *v90_sign_decoder_result_name(
	enum v90_sign_decoder_result result);

#endif /* V90_SIGN_DECODER_H */
