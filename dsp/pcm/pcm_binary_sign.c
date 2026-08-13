// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_binary_sign.h"

#include <stddef.h>
#include <stdint.h>

bool pcm_binary_sign_is_valid(const struct pcm_binary_sign *state)
{
	return state != NULL && state->previous_sign <= 1u &&
	       v34_scrambler_is_valid(&state->scrambler);
}

enum pcm_binary_sign_result pcm_binary_sign_init(
	struct pcm_binary_sign *state,
	enum v34_scrambler_polynomial polynomial)
{
	struct pcm_binary_sign initialized = { 0 };

	if (state == NULL ||
	    v34_scrambler_init(&initialized.scrambler, polynomial) !=
		    V34_FRAMING_OK ||
	    !pcm_binary_sign_is_valid(&initialized))
		return PCM_BINARY_SIGN_INVALID_ARGUMENT;
	*state = initialized;
	return PCM_BINARY_SIGN_OK;
}

enum pcm_binary_sign_result pcm_binary_sign_init_from_scrambler(
	struct pcm_binary_sign *state,
	const struct v34_scrambler *scrambler, uint8_t previous_sign)
{
	struct pcm_binary_sign initialized = { 0 };

	if (state == NULL || !v34_scrambler_is_valid(scrambler) ||
	    previous_sign > 1u)
		return PCM_BINARY_SIGN_INVALID_ARGUMENT;
	initialized.scrambler = *scrambler;
	initialized.previous_sign = previous_sign;
	if (!pcm_binary_sign_is_valid(&initialized))
		return PCM_BINARY_SIGN_INVALID_ARGUMENT;
	*state = initialized;
	return PCM_BINARY_SIGN_OK;
}

enum pcm_binary_sign_result pcm_binary_sign_track_scrambler_output(
	struct pcm_binary_sign *state, uint8_t sign)
{
	struct pcm_binary_sign next;
	uint8_t ignored;

	if (state == NULL || sign > 1u)
		return PCM_BINARY_SIGN_INVALID_ARGUMENT;
	if (!pcm_binary_sign_is_valid(state))
		return PCM_BINARY_SIGN_INVALID_STATE;
	next = *state;
	if (v34_descramble_group(
		    &next.scrambler, sign, 1u, &ignored) != V34_FRAMING_OK)
		return PCM_BINARY_SIGN_INVALID_STATE;
	next.previous_sign = sign;
	if (!pcm_binary_sign_is_valid(&next))
		return PCM_BINARY_SIGN_INVALID_STATE;
	*state = next;
	return PCM_BINARY_SIGN_OK;
}

enum pcm_binary_sign_result pcm_binary_sign_encode(
	struct pcm_binary_sign *state, uint8_t bit, uint8_t *sign)
{
	struct pcm_binary_sign next;
	uint8_t scrambled;
	uint8_t encoded;

	if (state == NULL || sign == NULL || bit > 1u)
		return PCM_BINARY_SIGN_INVALID_ARGUMENT;
	if (!pcm_binary_sign_is_valid(state))
		return PCM_BINARY_SIGN_INVALID_STATE;
	next = *state;
	if (v34_scramble_group(
		    &next.scrambler, bit, 1u, &scrambled) != V34_FRAMING_OK)
		return PCM_BINARY_SIGN_INVALID_STATE;
	encoded = next.previous_sign ^ scrambled;
	next.previous_sign = encoded;
	if (!pcm_binary_sign_is_valid(&next))
		return PCM_BINARY_SIGN_INVALID_STATE;
	*state = next;
	*sign = encoded;
	return PCM_BINARY_SIGN_OK;
}

enum pcm_binary_sign_result pcm_binary_sign_decode(
	struct pcm_binary_sign *state, uint8_t sign, uint8_t *bit)
{
	struct pcm_binary_sign next;
	uint8_t scrambled;
	uint8_t decoded;

	if (state == NULL || bit == NULL || sign > 1u)
		return PCM_BINARY_SIGN_INVALID_ARGUMENT;
	if (!pcm_binary_sign_is_valid(state))
		return PCM_BINARY_SIGN_INVALID_STATE;
	next = *state;
	scrambled = sign ^ next.previous_sign;
	next.previous_sign = sign;
	if (v34_descramble_group(
		    &next.scrambler, scrambled, 1u, &decoded) !=
		    V34_FRAMING_OK ||
	    !pcm_binary_sign_is_valid(&next))
		return PCM_BINARY_SIGN_INVALID_STATE;
	*state = next;
	*bit = decoded;
	return PCM_BINARY_SIGN_OK;
}

const char *pcm_binary_sign_result_name(
	enum pcm_binary_sign_result result)
{
	switch (result) {
	case PCM_BINARY_SIGN_OK:
		return "ok";
	case PCM_BINARY_SIGN_INVALID_ARGUMENT:
		return "invalid argument";
	case PCM_BINARY_SIGN_INVALID_STATE:
		return "invalid state";
	}
	return "unknown";
}
