// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_BINARY_SIGN_H
#define PCM_BINARY_SIGN_H

#include "v34_framing.h"

#include <stdbool.h>
#include <stdint.h>

enum pcm_binary_sign_result {
	PCM_BINARY_SIGN_OK,
	PCM_BINARY_SIGN_INVALID_ARGUMENT,
	PCM_BINARY_SIGN_INVALID_STATE,
};

/* Own a self-synchronizing scrambler and the modulo-two differential memory
 * used by binary PCM sign sequences. The same representation is valid on
 * transmit and receive paths. */
struct pcm_binary_sign {
	struct v34_scrambler scrambler;
	uint8_t previous_sign;
};

enum pcm_binary_sign_result pcm_binary_sign_init(
	struct pcm_binary_sign *state,
	enum v34_scrambler_polynomial polynomial);
enum pcm_binary_sign_result pcm_binary_sign_init_from_scrambler(
	struct pcm_binary_sign *state,
	const struct v34_scrambler *scrambler, uint8_t previous_sign);
bool pcm_binary_sign_is_valid(const struct pcm_binary_sign *state);

/* Advance across a preceding scrambled but non-differential training sign.
 * Encoding and decoding begin from the resulting history. */
enum pcm_binary_sign_result pcm_binary_sign_track_scrambler_output(
	struct pcm_binary_sign *state, uint8_t sign);

/* Output must not overlap state; rejected calls leave both unchanged. */
enum pcm_binary_sign_result pcm_binary_sign_encode(
	struct pcm_binary_sign *state, uint8_t bit, uint8_t *sign);
enum pcm_binary_sign_result pcm_binary_sign_decode(
	struct pcm_binary_sign *state, uint8_t sign, uint8_t *bit);

const char *pcm_binary_sign_result_name(
	enum pcm_binary_sign_result result);

#endif /* PCM_BINARY_SIGN_H */
