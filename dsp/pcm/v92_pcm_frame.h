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
	/* Receiver-only split; the wire still carries all 48 training frames. */
	V92_B1U_ACQUISITION_FRAMES = V92_B1U_FRAMES / 2,
	V92_B1U_SYMBOLS = V92_B1U_FRAMES * V92_PCM_MAPPING_FRAME_SYMBOLS,
	V92_B1U_VALIDATION_SYMBOLS = (V92_B1U_FRAMES - V92_B1U_ACQUISITION_FRAMES) *
		V92_PCM_MAPPING_FRAME_SYMBOLS,
	/* 120-symbol (15 ms) traceback with bounded competing histories. */
	V92_PCM_RX_PATHS = 32,
	V92_PCM_RX_DELAY_FRAMES = 10,
	/* Qualify 240T inside the 384T request, before its 24T prime tail. */
	V92_PCM_RM_QUALIFY_FRAMES = 20,
	/* Includes the enlarged final (clipped) positive PCMU cell. */
	V92_PCM_LEVEL_FIT_POINTS = 2048,
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
	uint16_t nondata_frames;
	uint8_t differential;
	uint8_t rm_frames;
	uint8_t rm_prime_frames;
};

enum v92_pcm_frame_rm {
	V92_PCM_FRAME_RM_NONE,
	V92_PCM_FRAME_RM_SIGNAL,
	V92_PCM_FRAME_RM_PRIME,
	V92_PCM_FRAME_RM_COMPLETE,
};

enum v92_pcm_frame_observation {
	V92_PCM_FRAME_FILTER_OUTPUT,
	V92_PCM_FRAME_CONSTELLATION,
};

/* The B1u interval fits the amplitude-dependent slope of residual variance in
 * recovered constellation coordinates. Keep slope/mean_error after training,
 * with a one-sample variance floor; payload never trains it. The caller must
 * validate the decoded B1u bits before trusting this trained data path. */
struct v92_pcm_frame_noise {
	double signal_energy, signal_fourth;
	double error_energy, error_signal_energy;
	double relative_variance;
	uint8_t frames;
};

struct v92_pcm_frame_rx {
	struct v92_pcm_mapping mapping;
	struct v92_pcm_frame_rx_path paths[V92_PCM_RX_PATHS];
	struct v92_pcm_frame_noise noise;
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
	/* Current direct-path low-margin disagreement. Unlike an ambiguity flag,
	 * this is soft quality evidence: selected bits and coding are unchanged.
	 * It does not persist into later frames or label bits as an erasure. */
	bool uncertain;
	bool bits_ready;
	/* Unresolved histories disagree about the delivered payload. */
	bool ambiguous;
	/* Current-frame request evidence, independent of delayed bits. A single
	 * in-range RM-like word remains payload. Qualified RM and its two prime
	 * frames are signaling; the caller owns the response/control transition. */
	enum v92_pcm_frame_rm rm;
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

/* Encode one RM or RM-prime frame (V.92 8.7.4) in the active data mapping.
 * Trellis, precoder and prefilter histories continue. Scrambler/differential
 * state is untouched; these signals specify modulus-output digits directly,
 * so frame_sign is zero. The caller owns repetition and the control reset.
 * Rejected calls leave state/output unchanged; they must not overlap. */
enum v92_pcm_frame_result v92_pcm_frame_encode_rm(
	struct v92_pcm_frame_tx *transmitter, bool prime, struct v92_pcm_frame *frame);

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
 * delay before supplying frames. The latter half of B1u measures additive
 * and amplitude-dependent residual noise for weighting payload decisions.
 * These observations are not scaled prefilter outputs and must not undergo
 * another G.711 quantization. Coding, traceback,
 * and reconstructed filter histories use the same decoder as above. */
enum v92_pcm_frame_result v92_pcm_frame_rx_init_constellation(
	struct v92_pcm_frame_rx *receiver,
	const struct v92_pcm_mapping *mapping);
bool v92_pcm_frame_rx_is_valid(const struct v92_pcm_frame_rx *receiver);

/* One zero-initialized fit per B1u mapping epoch. Scores cover the ORIGINAL
 * training cell; an early frame cannot permanently exclude a level supported
 * by later training. The line owner discards this scratch after B1u. */
struct v92_pcm_level_fit {
	uint64_t error[V92_PCM_LEVEL_FIT_POINTS];
	int16_t minimum, maximum;
	uint8_t frames;
};

/* Accumulate known B1u evidence and retain the contiguous equal-cost interval
 * around its best level. Ordinary data never refines this fit. Coding histories
 * are unchanged; the line owner still qualifies unrounded error. Only
 * filter-output receivers have a receive-level interval. Rejection changes
 * neither receiver nor fit; calls beyond the 48 B1u frames are rejected. */
enum v92_pcm_frame_result v92_pcm_frame_rx_refine_level(
	struct v92_pcm_frame_rx *receiver, struct v92_pcm_level_fit *fit,
	const struct v92_pcm_frame *frame,
	const int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS]);

/* Receive the matching control boundary. Reset descrambler and differential
 * state on EVERY surviving path, retaining metrics, ambiguity, filters,
 * trellis, and pending payload. Already decoded bits still emerge after the
 * usual lookahead; the caller must route them by their original frame, not
 * by the current input phase. Rejected resets leave state unchanged. */
enum v92_pcm_frame_result v92_pcm_frame_rx_reset_control(
	struct v92_pcm_frame_rx *receiver);

/* Decode one aligned twelve-symbol frame in the initialized observation
 * coordinates: decoded G.711 filter output, or recovered linear u(n).
 * Predicted samples and squared error use those same coordinates and remain
 * unweighted even when the sequence search uses learned noise weights or
 * pre-codec distance to the received G.711 input cell.
 * A bounded sequence search retains competing histories across frames.
 * Bits become ready after V92_PCM_RX_DELAY_FRAMES lookahead frames; while
 * buffering or delivering a non-payload slot, bits remain untouched. RM's
 * exact spare modulus words are signaling, not invalid payload codewords.
 * Qualified RM also clamps its pending in-range words without discarding
 * preceding data. Non-payload tags follow each competing delayed history.
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

struct v92_observation;
struct v92_pcm_frame_channel {
	/* Borrowed, frozen for this coding epoch. Acquisition is caller-owned. */
	const struct v92_observation *model;
	/* Optional aligned decoded wire levels before host gain. Each present
	 * level must be a G.711 reconstruction value. NULL infers bins from
	 * samples and the model's host gain. Arrays contain twelve symbols. */
	const int16_t *levels;
	/* Optional independent affine host corrections, in sample coordinates.
	 * Requires levels; see v92_observation_training. All twelve must be
	 * finite, including erased positions (whose quality is still reported). */
	const double *offsets;
	/* NULL means all received; false ignores the corresponding sample and
	 * level in likelihood scoring. Erasures still advance coding history. */
	const bool *received;
};

struct v92_pcm_frame_likelihood {
	uint64_t cost;
	/* Model predictions with the independently supplied host correction.
	 * Never fitted to the received values used to select the coding path. */
	double predicted_samples[V92_PCM_MAPPING_FRAME_SYMBOLS];
};

/* Decode calibrated samples using the frozen codec/channel likelihood.
 * Requires a constellation-coordinate receiver with two x-history entries.
 * The channel and likelihood are required. Cost is the winning increment
 * before path-metric normalization; compare it only within a frozen epoch.
 * Near-tied merged histories retain disputed payload as ambiguous.
 * The decision's integer predictions/residual retain their linear u(n)
 * meaning; use likelihood.predicted_samples for channel-quality checks.
 * Receiver, bits, decision and likelihood commit together, only after all
 * predictions are finite. All inputs, outputs and state must be disjoint. */
enum v92_pcm_frame_result v92_pcm_frame_decode_observed(
	struct v92_pcm_frame_rx *receiver,
	const int16_t samples[V92_PCM_MAPPING_FRAME_SYMBOLS], bool b1u,
	uint8_t *bits, size_t bit_count,
	struct v92_pcm_frame_decision *decision,
	const struct v92_pcm_frame_channel *channel,
	struct v92_pcm_frame_likelihood *likelihood);

const char *v92_pcm_frame_result_name(
	enum v92_pcm_frame_result result);

#endif /* V92_PCM_FRAME_H */
