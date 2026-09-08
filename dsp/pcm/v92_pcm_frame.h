// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_PCM_FRAME_H
#define V92_PCM_FRAME_H

#include "v34_framing.h"
#include "v34_trellis.h"
#include "v92_pcm_mapping.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V92_B1U_FRAMES = 48,
	V92_B1U_SYMBOLS = V92_B1U_FRAMES * V92_PCM_MAPPING_FRAME_SYMBOLS,
};

enum v92_pcm_frame_result {
	V92_PCM_FRAME_OK,
	V92_PCM_FRAME_INVALID_ARGUMENT,
	V92_PCM_FRAME_INVALID_PARAMETERS,
	V92_PCM_FRAME_OUT_OF_RANGE,
};

struct v92_pcm_frame_filter_state {
	/* Active u, x, and v histories partition this one bounded store. Their
	 * combined extent cannot exceed CPd's total-coefficient limit. */
	int32_t history[V92_CPD_MAX_TOTAL_COEFFICIENTS];
};

struct v92_pcm_frame_tx_state {
	struct v34_scrambler scrambler;
	struct v34_convolutional_encoder trellis;
	struct v92_pcm_frame_filter_state filter;
	uint8_t differential;
};

/* The mapping is owned by the transmitter. Dynamic state is kept separate so
 * one rejected frame can be transactional without copying the CPd tables. */
struct v92_pcm_frame_tx {
	struct v92_pcm_mapping mapping;
	struct v92_pcm_frame_tx_state state;
};

struct v92_pcm_frame {
	uint8_t digit[V92_PCM_MAPPING_FRAME_SYMBOLS];
	uint8_t y0[V92_PCM_MAPPING_FRAME_SYMBOLS];
	int16_t signed_index[V92_PCM_MAPPING_FRAME_SYMBOLS];
	int16_t u[V92_PCM_MAPPING_FRAME_SYMBOLS];
	int32_t x[V92_PCM_MAPPING_FRAME_SYMBOLS];
	int32_t v[V92_PCM_MAPPING_FRAME_SYMBOLS];
	uint8_t frame_sign;
};

enum v92_pcm_frame_result v92_pcm_frame_tx_init(
	struct v92_pcm_frame_tx *transmitter,
	const struct v92_pcm_mapping *mapping);
bool v92_pcm_frame_tx_is_valid(
	const struct v92_pcm_frame_tx *transmitter);

/* B1u starts with zero scrambler, differential, trellis, and filter state. */
enum v92_pcm_frame_result v92_pcm_frame_tx_reset_b1u(
	struct v92_pcm_frame_tx *transmitter);

/* Input bits are LSB-first, as in the mixed-radix mapping. */
enum v92_pcm_frame_result v92_pcm_frame_encode(
	struct v92_pcm_frame_tx *transmitter,
	const uint8_t *bits, size_t bit_count,
	struct v92_pcm_frame *frame);

/* Apply CPd's 4G and the trained upstream level to one prefilter output. */
enum v92_pcm_frame_result v92_pcm_frame_scale(
	const struct v92_pcm_mapping *mapping, int16_t upstream_level,
	int32_t prefilter_output, int16_t *sample);

const char *v92_pcm_frame_result_name(
	enum v92_pcm_frame_result result);

#endif /* V92_PCM_FRAME_H */
