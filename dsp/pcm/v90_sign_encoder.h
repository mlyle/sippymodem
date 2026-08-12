// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_SIGN_ENCODER_H
#define V90_SIGN_ENCODER_H

#include "v90_cp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum v90_sign_encoder_result {
	V90_SIGN_ENCODER_OK,
	V90_SIGN_ENCODER_INVALID_ARGUMENT,
	V90_SIGN_ENCODER_INVALID_STATE,
	V90_SIGN_ENCODER_INVALID_BITS,
};

/* Own both differential-coding stages used to turn S sign input bits into
 * six initial PCM signs. Spectral inversion is a separate mapping step. */
struct v90_sign_encoder {
	uint8_t previous_shaping_signs[V90_CP_FRAME_INTERVALS];
	uint8_t previous_odd;
	uint8_t previous_pcm_sign;
	uint8_t sign_redundancy;
};

enum v90_sign_encoder_result v90_sign_encoder_init(
	struct v90_sign_encoder *encoder, uint8_t sign_redundancy);
bool v90_sign_encoder_is_valid(
	const struct v90_sign_encoder *encoder);

/* Consume exactly S = 6 - Sr bits in transmission order. With Sr zero the
 * returned signs are final; otherwise they are the initial t assignments
 * that the spectral shaper may invert. Rejected calls change neither object. */
enum v90_sign_encoder_result v90_sign_encoder_encode(
	struct v90_sign_encoder *encoder,
	const uint8_t *bits, size_t bit_count,
	uint8_t signs[V90_CP_FRAME_INTERVALS]);

const char *v90_sign_encoder_result_name(
	enum v90_sign_encoder_result result);

#endif /* V90_SIGN_ENCODER_H */
