// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PCM_FRAME_ENCODER_H
#define V90_PCM_FRAME_ENCODER_H

#include "v90_pcm_mapping.h"
#include "v90_sign_encoder.h"
#include "v90_spectral_shaper.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum v90_pcm_frame_encoder_result {
	V90_PCM_FRAME_ENCODER_OK,
	V90_PCM_FRAME_ENCODER_SYMBOLS_READY,
	V90_PCM_FRAME_ENCODER_BUFFERING,
	V90_PCM_FRAME_ENCODER_COMPLETE,
	V90_PCM_FRAME_ENCODER_INVALID_ARGUMENT,
	V90_PCM_FRAME_ENCODER_INVALID_PARAMETERS,
	V90_PCM_FRAME_ENCODER_INVALID_STATE,
	V90_PCM_FRAME_ENCODER_INVALID_BITS,
	V90_PCM_FRAME_ENCODER_NUMERIC_FAILURE,
	V90_PCM_FRAME_ENCODER_INTERNAL_ERROR,
};

/* A process call emits at most six ordered symbols. With look-ahead, a chunk
 * may straddle input data-frame boundaries and contain as many as three
 * shaping frames. The selected rules form a compact recording seam;
 * rule_count is zero when spectral shaping is disabled. */
struct v90_pcm_frame_output {
	struct v90_pcm_symbol symbols[V90_CP_FRAME_INTERVALS];
	enum v90_spectral_rule rules[3];
	uint8_t symbol_count;
	uint8_t rule_count;
};

/* Owned encoder state for one negotiated mapping. Input scrambling and the
 * choice of training or data bits remain policy owned by the caller. */
struct v90_pcm_frame_encoder {
	struct v90_pcm_mapping mapping;
	struct v90_sign_encoder signs;
	struct v90_spectral_shaper shaper;
	bool finishing;
};

enum v90_pcm_frame_encoder_result v90_pcm_frame_encoder_init(
	struct v90_pcm_frame_encoder *encoder,
	const struct v90_pcm_mapping *mapping);
bool v90_pcm_frame_encoder_is_valid(
	const struct v90_pcm_frame_encoder *encoder);

/* Consume exactly D binary bits in transmission order. The first S bits feed
 * sign assignment and the remaining K bits feed the LSB-first modulus map.
 * BUFFERING accepts the frame but leaves output unchanged. */
enum v90_pcm_frame_encoder_result v90_pcm_frame_encoder_process(
	struct v90_pcm_frame_encoder *encoder,
	const uint8_t *bits, size_t bit_count,
	struct v90_pcm_frame_output *output);

/* Drain delayed shaping frames one at a time. Once called, process is no
 * longer valid. An unshaped encoder completes immediately. */
enum v90_pcm_frame_encoder_result v90_pcm_frame_encoder_finish(
	struct v90_pcm_frame_encoder *encoder,
	struct v90_pcm_frame_output *output);

const char *v90_pcm_frame_encoder_result_name(
	enum v90_pcm_frame_encoder_result result);

#endif /* V90_PCM_FRAME_ENCODER_H */
