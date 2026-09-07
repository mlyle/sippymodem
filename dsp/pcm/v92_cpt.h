// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_CPT_H
#define V92_CPT_H

#include "g711.h"
#include "pcm_constellation.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V92_CPT_FRAME_INTERVALS = PCM_CONSTELLATION_FRAME_INTERVALS,
	V92_CPT_MAX_CONSTELLATIONS = PCM_CONSTELLATION_MAX_ROWS,
	V92_CPT_HEADER_BITS = 136,
	V92_CPT_CONSTELLATION_BITS = 136,
	V92_CPT_MIN_TRAILER_BITS = 18,
	V92_CPT_SYMBOLS_PER_FRAME = 12,
	V92_CPT_MIN_FRAME_BITS =
		(V92_CPT_HEADER_BITS + V92_CPT_CONSTELLATION_BITS +
		 V92_CPT_MIN_TRAILER_BITS + V92_CPT_SYMBOLS_PER_FRAME - 1) /
		V92_CPT_SYMBOLS_PER_FRAME * V92_CPT_SYMBOLS_PER_FRAME,
	V92_CPT_MAX_FRAME_BITS =
		(V92_CPT_HEADER_BITS + 2 * V92_CPT_MAX_CONSTELLATIONS *
		 V92_CPT_CONSTELLATION_BITS + V92_CPT_MIN_TRAILER_BITS +
		 V92_CPT_SYMBOLS_PER_FRAME - 1) /
		V92_CPT_SYMBOLS_PER_FRAME * V92_CPT_SYMBOLS_PER_FRAME,
	V92_CPT_MAX_FRAME_BYTES =
		(V92_CPT_MAX_FRAME_BITS + 7) / 8,
	V92_CPT_RATE_INDEX_MAX = 22,
};

enum v92_cpt_result {
	V92_CPT_OK,
	V92_CPT_INVALID_ARGUMENT,
	V92_CPT_INVALID_PARAMETERS,
	V92_CPT_OUTPUT_TOO_SMALL,
	V92_CPT_INVALID_FRAME,
	V92_CPT_CRC_MISMATCH,
};

/* Exact Table 23 fields owned by CPt. Reserved and fixed fields are generated
 * by the codec. Fixed-point values remain in their transmitted form. */
struct v92_cpt_parameters {
	struct pcm_constellation
		transmit_constellations[V92_CPT_MAX_CONSTELLATIONS];
	struct pcm_constellation
		codec_constellations[V92_CPT_MAX_CONSTELLATIONS];
	enum g711_law law;
	uint16_t trn1d_ratio_q3_13;
	int8_t shaping_a1_q1_6;
	int8_t shaping_a2_q1_6;
	int8_t shaping_b1_q1_6;
	int8_t shaping_b2_q1_6;
	uint8_t selected_rate_index;
	uint8_t sign_redundancy;
	uint8_t spectral_lookahead;
	uint8_t constellation_count;
	uint8_t frame_constellation[V92_CPT_FRAME_INTERVALS];
	bool acknowledges_peer;
	bool codec_constellations_differ;
};

bool v92_cpt_parameters_are_valid(
	const struct v92_cpt_parameters *parameters);

/* CPt carries one binary bit per TRN1u symbol, so the complete sequence,
 * including CRC and zero fill, is an integral number of 12-symbol frames. */
size_t v92_cpt_frame_bits(
	const struct v92_cpt_parameters *parameters);
size_t v92_cpt_frame_bytes(
	const struct v92_cpt_parameters *parameters);

/* Pack bits in transmission order, most-significant bit first within each
 * storage octet. Rejected calls leave both outputs unchanged. */
enum v92_cpt_result v92_cpt_encode(
	const struct v92_cpt_parameters *parameters,
	uint8_t *frame, size_t frame_capacity,
	size_t *frame_bit_count);

/* frame_bit_count is the exact wire length, not the rounded storage size.
 * Reserved fields are covered by the CRC but otherwise ignored. Decode
 * accepts no unused constellation records, nonzero fill, or nonzero storage
 * padding. Rejected calls leave parameters unchanged. */
enum v92_cpt_result v92_cpt_decode(
	const uint8_t *frame, size_t frame_capacity,
	size_t frame_bit_count,
	struct v92_cpt_parameters *parameters);

const char *v92_cpt_result_name(enum v92_cpt_result result);

#endif /* V92_CPT_H */
