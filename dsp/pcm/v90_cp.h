// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_CP_H
#define V90_CP_H

#include "g711.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V90_CP_FRAME_INTERVALS = 6,
	V90_CP_MAX_CONSTELLATIONS = 6,
	V90_CP_CONSTELLATION_UCODES = 128,
	V90_CP_CONSTELLATION_MASK_BYTES =
		V90_CP_CONSTELLATION_UCODES / 8,
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

/* Bit n represents Ucode n. The transmitted order is Ucode 0 through 127;
 * this packed form avoids a 128-entry level table. */
struct v90_cp_constellation {
	uint8_t ucode_mask[V90_CP_CONSTELLATION_MASK_BYTES];
};

/* Exact Table 14 values. Fixed-point fields remain in their transmitted
 * integer representation so encode/decode does not introduce rounding.
 * A selected_rate_index of zero denotes cleardown; 1 through 22 select a
 * rate relative to the CPt or CP base. */
struct v90_cp_parameters {
	struct v90_cp_constellation
		transmit_constellations[V90_CP_MAX_CONSTELLATIONS];
	struct v90_cp_constellation
		codec_constellations[V90_CP_MAX_CONSTELLATIONS];
	enum v90_cp_kind kind;
	enum g711_law law;
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

bool v90_cp_constellation_contains(
	const struct v90_cp_constellation *constellation,
	uint8_t ucode);
bool v90_cp_parameters_are_valid(
	const struct v90_cp_parameters *parameters);

/* Return zero for invalid parameters. The final octet may contain unused
 * low-order storage bits; frame_bits is the authoritative wire length. */
size_t v90_cp_frame_bits(const struct v90_cp_parameters *parameters);
size_t v90_cp_frame_bytes(const struct v90_cp_parameters *parameters);

#endif /* V90_CP_H */
