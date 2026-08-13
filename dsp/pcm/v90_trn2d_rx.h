// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_TRN2D_RX_H
#define V90_TRN2D_RX_H

#include "v34_framing.h"
#include "v90_pcm_frame_decoder.h"
#include "v90_phase4.h"

#include <stdbool.h>
#include <stdint.h>

enum v90_trn2d_rx_result {
	V90_TRN2D_RX_OK,
	V90_TRN2D_RX_BUFFERING,
	V90_TRN2D_RX_FRAME_READY,
	V90_TRN2D_RX_INVALID_ARGUMENT,
	V90_TRN2D_RX_INVALID_PARAMETERS,
	V90_TRN2D_RX_INVALID_STATE,
	V90_TRN2D_RX_INVALID_SYMBOL,
	V90_TRN2D_RX_OUT_OF_RANGE,
	V90_TRN2D_RX_INTERNAL_ERROR,
};

/* Descrambled bits are in transmission order. training_bit_errors counts
 * departures from the binary ones sent during TRN2d. Inferred shaping rules
 * are retained as a compact recording seam. */
struct v90_trn2d_frame {
	uint8_t bits[V90_PCM_FRAME_MAX_BITS];
	enum v90_spectral_rule rules[3];
	uint8_t bit_count;
	uint8_t rule_count;
	uint8_t training_bit_errors;
};

/* Own one phase-aligned six-symbol stream, the inverse PCM coder histories,
 * and the fresh GPC descrambler epoch that begins after Ri-bar. */
struct v90_trn2d_rx {
	struct v90_pcm_frame_decoder decoder;
	struct v34_scrambler descrambler;
	uint8_t pending_ucodes[V90_CP_FRAME_INTERVALS];
	uint8_t pending_signs[V90_CP_FRAME_INTERVALS];
	uint64_t received_symbols;
	uint64_t received_frames;
	uint64_t received_bits;
	uint64_t training_bit_errors;
	uint8_t pending_count;
};

enum v90_trn2d_rx_result v90_trn2d_rx_init(
	struct v90_trn2d_rx *receiver,
	const struct v90_pcm_mapping *training_mapping,
	enum v90_pcm_mapping_plane decision_plane);
bool v90_trn2d_rx_is_valid(const struct v90_trn2d_rx *receiver);
bool v90_trn2d_rx_minimum_received(
	const struct v90_trn2d_rx *receiver);

/* Consume one hard decision at the known mapper phase. BUFFERING accepts the
 * decision but leaves frame unchanged. A rejected call changes neither state
 * nor output. */
enum v90_trn2d_rx_result v90_trn2d_rx_receive_decision(
	struct v90_trn2d_rx *receiver, uint8_t ucode, bool positive,
	struct v90_trn2d_frame *frame);

/* Parse one authoritative G.711 network character, preserving the complete
 * PCMU and PCMA code spaces, then use the same hard-decision boundary. */
enum v90_trn2d_rx_result v90_trn2d_rx_receive_octet(
	struct v90_trn2d_rx *receiver, uint8_t network_octet,
	struct v90_trn2d_frame *frame);

const char *v90_trn2d_rx_result_name(
	enum v90_trn2d_rx_result result);

#endif /* V90_TRN2D_RX_H */
