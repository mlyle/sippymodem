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
	/* 120-symbol (15 ms) traceback with bounded competing histories. */
	V92_PCM_RX_PATHS = 32,
	V92_PCM_RX_DELAY_FRAMES = 10,
};

enum v92_pcm_frame_result {
	V92_PCM_FRAME_OK,
	V92_PCM_FRAME_INVALID_ARGUMENT,
	V92_PCM_FRAME_INVALID_PARAMETERS,
	V92_PCM_FRAME_OUT_OF_RANGE,
	V92_PCM_FRAME_NO_PATH,
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

struct v92_pcm_frame_rx_path {
	struct v34_scrambler descrambler;
	struct v34_convolutional_encoder trellis;
	struct v92_pcm_frame_filter_state filter;
	uint64_t metric;
	uint8_t pending[V92_PCM_RX_DELAY_FRAMES][V92_PCM_MAPPING_MAX_BITS];
	uint16_t ambiguous_frames;
	uint8_t differential;
};

enum v92_pcm_frame_observation {
	V92_PCM_FRAME_FILTER_OUTPUT,
	V92_PCM_FRAME_CONSTELLATION,
};

struct v92_pcm_frame_rx {
	struct v92_pcm_mapping mapping;
	struct v92_pcm_frame_rx_path paths[V92_PCM_RX_PATHS];
	enum v92_pcm_frame_observation observation;
	/* Zero in constellation coordinates; the caller has removed the gain. */
	int16_t upstream_level;
	int16_t minimum_level;
	int16_t maximum_level;
	uint8_t path_count;
	uint8_t pending_frames;
};

struct v92_pcm_frame_decision {
	/* Current received frame and its residual; payload bits are delayed. */
	struct v92_pcm_frame frame;
	int16_t predicted_samples[V92_PCM_MAPPING_FRAME_SYMBOLS];
	uint64_t squared_error;
	bool bits_ready;
	/* Equal-distance histories disagree about the delivered payload. */
	bool ambiguous;
};

enum v92_pcm_frame_result v92_pcm_frame_tx_init(
	struct v92_pcm_frame_tx *transmitter,
	const struct v92_pcm_mapping *mapping);
bool v92_pcm_frame_tx_is_valid(
	const struct v92_pcm_frame_tx *transmitter);

/* B1u starts with zero scrambler, differential, trellis, and filter state. */
enum v92_pcm_frame_result v92_pcm_frame_tx_reset_b1u(
	struct v92_pcm_frame_tx *transmitter);

/* Start fast-exchange control on the preceding data mapping (V.92 9.9.2).
 * Reset only scrambler and differential coding; trellis and both filter
 * histories continue. The caller owns the aligned control boundary. */
enum v92_pcm_frame_result v92_pcm_frame_tx_reset_control(
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

/* Initialize at B1u with the accepted CPd and trained LU. The continuous
 * line equalizer and timing remain owned by the caller. */
enum v92_pcm_frame_result v92_pcm_frame_rx_init(
	struct v92_pcm_frame_rx *receiver,
	const struct v92_pcm_mapping *mapping, int16_t upstream_level);

/* Initialize at B1u for recovered linear constellation coordinates u(n).
 * The caller must compensate the channel, remove gain, and align its decision
 * delay before supplying frames. These observations are not scaled prefilter
 * outputs and must not undergo another G.711 quantization. Coding, traceback,
 * and reconstructed filter histories use the same decoder as above. */
enum v92_pcm_frame_result v92_pcm_frame_rx_init_constellation(
	struct v92_pcm_frame_rx *receiver,
	const struct v92_pcm_mapping *mapping);
bool v92_pcm_frame_rx_is_valid(const struct v92_pcm_frame_rx *receiver);

/* Refine the receive level against known training, retaining the contiguous
 * LU interval that predicts the same quantized waveform as the best estimate.
 * Feed that interval into subsequent refinement calls. Ordinary data uses its
 * nearest possible codeword without changing the trained interval. Coding
 * histories are unchanged; the line owner still qualifies unrounded error.
 * Only filter-output receivers have a receive-level interval. */
enum v92_pcm_frame_result v92_pcm_frame_rx_refine_level(
	struct v92_pcm_frame_rx *receiver, const struct v92_pcm_frame *frame,
	const int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS],
	int16_t minimum_level, int16_t maximum_level);

/* Receive the matching control boundary. Reset descrambler and differential
 * state on EVERY surviving path, retaining metrics, ambiguity, filters,
 * trellis, and pending payload. Already decoded bits still emerge after the
 * usual lookahead; the caller must route them by their original frame, not
 * by the current input phase. Rejected resets leave state unchanged. */
enum v92_pcm_frame_result v92_pcm_frame_rx_reset_control(
	struct v92_pcm_frame_rx *receiver);

/* Decode one aligned twelve-symbol frame in the initialized observation
 * coordinates: decoded G.711 filter output, or recovered linear u(n).
 * Predicted samples and squared error use those same coordinates.
 * A bounded sequence search retains competing histories across frames.
 * Bits become ready after V92_PCM_RX_DELAY_FRAMES lookahead frames; before
 * that, bits remain untouched while state and decision advance normally.
 * B1u constrains source bits to ones; its residual is training evidence, not
 * an automatic pass. Ordinary data has no payload-dependent constraints.
 * Ambiguity and residuals are quality evidence for the call owner, not a
 * guarantee of correct payload. CPd points can coincide after quantization.
 * State, bits and decision commit together; rejected calls change none.
 * The input, outputs and receiver must not overlap. */
enum v92_pcm_frame_result v92_pcm_frame_decode(
	struct v92_pcm_frame_rx *receiver,
	const int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS], bool b1u,
	uint8_t *bits, size_t bit_count,
	struct v92_pcm_frame_decision *decision);

const char *v92_pcm_frame_result_name(
	enum v92_pcm_frame_result result);

#endif /* V92_PCM_FRAME_H */
