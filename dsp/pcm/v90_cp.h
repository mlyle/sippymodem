// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_CP_H
#define V90_CP_H

#include "g711.h"
#include "pcm_constellation.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V90_CP_FRAME_INTERVALS = PCM_CONSTELLATION_FRAME_INTERVALS,
	V90_CP_MAX_CONSTELLATIONS = PCM_CONSTELLATION_MAX_ROWS,
	V90_CP_CONSTELLATION_UCODES = PCM_CONSTELLATION_UCODES,
	V90_CP_CONSTELLATION_MASK_BYTES =
		PCM_CONSTELLATION_MASK_BYTES,
	V90_CP_HEADER_BITS = 136,
	V90_CP_CONSTELLATION_BITS = 136,
	V90_CP_TRAILER_BITS = 20,
	V90_CP_MIN_FRAME_BITS =
		V90_CP_HEADER_BITS + V90_CP_CONSTELLATION_BITS +
		V90_CP_TRAILER_BITS,
	V90_CP_MAX_FRAME_BITS =
		V90_CP_HEADER_BITS + 2 * V90_CP_MAX_CONSTELLATIONS *
		V90_CP_CONSTELLATION_BITS + V90_CP_TRAILER_BITS,
	V90_CP_MAX_FRAME_BYTES = (V90_CP_MAX_FRAME_BITS + 7) / 8,
	V90_CP_RATE_INDEX_MAX = 22,
	V90_CP_UPSTREAM_RATE_BITS = 13,
	V90_CP_UPSTREAM_RATE_MASK =
		(1u << V90_CP_UPSTREAM_RATE_BITS) - 1u,
};

enum v90_cp_kind {
	V90_CP_TRAINING,
	V90_CP_DATA,
};

enum v90_cp_result {
	V90_CP_OK,
	V90_CP_INVALID_ARGUMENT,
	V90_CP_INVALID_PARAMETERS,
	V90_CP_INVALID_FRAME,
	V90_CP_CRC_MISMATCH,
};

/* Exact Table 14 values. Fixed-point fields remain in their transmitted
 * integer representation so encode/decode does not introduce rounding.
 * A selected_rate_index of zero denotes cleardown; 1 through 22 select a
 * rate relative to the CPt or CP base. */
struct v90_cp_parameters {
	struct pcm_constellation
		transmit_constellations[V90_CP_MAX_CONSTELLATIONS];
	struct pcm_constellation
		codec_constellations[V90_CP_MAX_CONSTELLATIONS];
	enum v90_cp_kind kind;
	enum g711_law law; /* Table 14 codec type, not network-law negotiation. */
	uint16_t upstream_rate_mask;
	uint16_t trn1d_ratio_q3_13;
	int8_t shaping_a1_q1_6;
	int8_t shaping_a2_q1_6;
	int8_t shaping_b1_q1_6;
	int8_t shaping_b2_q1_6;
	uint8_t selected_rate_index;
	uint8_t sign_redundancy;
	uint8_t spectral_lookahead;
	uint8_t constellation_count;
	uint8_t frame_constellation[V90_CP_FRAME_INTERVALS];
	bool request_silence;
	bool acknowledges_peer;
	bool codec_constellations_differ;
};

bool v90_cp_parameters_are_valid(
	const struct v90_cp_parameters *parameters);

/* Ordinary data parameters exclude CPt, cleardown and silence requests.
 * CPs uses a separate exchange (V.90 9.6.1.2); acknowledgment is independent. */
static inline bool v90_cp_is_data(const struct v90_cp_parameters *cp)
{
	return v90_cp_parameters_are_valid(cp) && cp->kind == V90_CP_DATA &&
	       cp->selected_rate_index != 0 && !cp->request_silence;
}

/* The ordinary E/B1 handoff requires CP prime (V.90 9.4.2.3/4). */
static inline bool v90_cp_is_final_data(const struct v90_cp_parameters *cp)
{
	return v90_cp_is_data(cp) && cp->acknowledges_peer;
}

/* Compare encoded fields, including acknowledgment. Reject invalid records;
 * ignore struct padding, undeclared rows and an absent codec constellation set. */
bool v90_cp_parameters_are_equal(
	const struct v90_cp_parameters *left,
	const struct v90_cp_parameters *right);

/* Return zero for invalid parameters. The final octet may contain unused
 * low-order storage bits; frame_bits is the authoritative wire length. */
size_t v90_cp_frame_bits(const struct v90_cp_parameters *parameters);
size_t v90_cp_frame_bytes(const struct v90_cp_parameters *parameters);

/* Pack bits in transmission order, most-significant bit first within each
 * storage octet. Rejected calls leave every output unchanged. */
enum v90_cp_result v90_cp_encode(
	const struct v90_cp_parameters *parameters,
	uint8_t *frame, size_t frame_capacity,
	size_t *frame_bit_count);

/* frame_bit_count is the exact wire length, not the rounded storage size.
 * Decode accepts no unused constellation records or nonzero storage padding. */
enum v90_cp_result v90_cp_decode(
	const uint8_t *frame, size_t frame_capacity,
	size_t frame_bit_count,
	struct v90_cp_parameters *parameters);

#endif /* V90_CP_H */
