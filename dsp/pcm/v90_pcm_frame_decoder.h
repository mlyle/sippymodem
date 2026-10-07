// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PCM_FRAME_DECODER_H
#define V90_PCM_FRAME_DECODER_H

#include "v90_pcm_mapping.h"
#include "v90_sign_decoder.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum v90_pcm_frame_decoder_result {
	V90_PCM_FRAME_DECODER_OK,
	V90_PCM_FRAME_DECODER_INVALID_ARGUMENT,
	V90_PCM_FRAME_DECODER_INVALID_PARAMETERS,
	V90_PCM_FRAME_DECODER_INVALID_STATE,
	V90_PCM_FRAME_DECODER_INVALID_SYMBOLS,
	V90_PCM_FRAME_DECODER_INTERNAL_ERROR,
};

/* One recovered D-bit data frame in transmission order: S sign bits followed
 * by K LSB-first modulus bits. Inferred shaping rules form a compact recording
 * seam and are absent when Sr is zero. */
struct v90_pcm_frame_decode_output {
	uint8_t bits[PCM_FRAME_MAX_BITS];
	enum v90_spectral_rule rules[3];
	uint8_t bit_count;
	uint8_t rule_count;
};

/* Own the negotiated mapping and sign histories. The decision plane says
 * whether hard-decision Ucodes represent the transmitter or codec D/A
 * constellations signalled in CPt/CP. */
struct v90_pcm_frame_decoder {
	struct v90_pcm_mapping mapping;
	struct v90_sign_decoder signs;
	enum v90_pcm_mapping_plane decision_plane;
};

enum v90_pcm_frame_decoder_result v90_pcm_frame_decoder_init(
	struct v90_pcm_frame_decoder *decoder,
	const struct v90_pcm_mapping *mapping,
	enum v90_pcm_mapping_plane decision_plane);
bool v90_pcm_frame_decoder_is_valid(
	const struct v90_pcm_frame_decoder *decoder);

/* Decode exactly six phase-aligned hard decisions. A Ucode absent from its
 * interval or a non-binary sign is invalid. Unused mixed-radix tuples retain
 * their low K bits so channel errors do not freeze coding histories. Every
 * rejected call leaves state and output unchanged. */
enum v90_pcm_frame_decoder_result v90_pcm_frame_decoder_decode(
	struct v90_pcm_frame_decoder *decoder,
	const uint8_t ucodes[V90_CP_FRAME_INTERVALS],
	const uint8_t signs[V90_CP_FRAME_INTERVALS], size_t symbol_count,
	struct v90_pcm_frame_decode_output *output);

#endif /* V90_PCM_FRAME_DECODER_H */
