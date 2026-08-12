// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PCM_MAPPING_H
#define V90_PCM_MAPPING_H

#include "v90_cp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum v90_pcm_mapping_plane {
	V90_PCM_MAPPING_TRANSMIT,
	V90_PCM_MAPPING_CODEC,
};

enum v90_pcm_mapping_result {
	V90_PCM_MAPPING_OK,
	V90_PCM_MAPPING_INVALID_ARGUMENT,
	V90_PCM_MAPPING_INVALID_PARAMETERS,
	V90_PCM_MAPPING_OUT_OF_RANGE,
};

/* Owned six-interval PCM mapping derived from one accepted CPt or CP. Masks
 * remain packed because label lookup is infrequent during six-symbol frame
 * construction and this avoids a 768-byte expanded level table per plane. */
struct v90_pcm_mapping {
	struct v90_cp_constellation
		transmit[V90_CP_FRAME_INTERVALS];
	struct v90_cp_constellation
		codec[V90_CP_FRAME_INTERVALS];
	enum v90_cp_kind kind;
	enum g711_law law;
	uint8_t modulus[V90_CP_FRAME_INTERVALS];
	uint8_t frame_bits;
	uint8_t modulus_bits;
	uint8_t sign_bits;
	uint8_t sign_redundancy;
	uint8_t spectral_lookahead;
	int8_t shaping_a1_q1_6;
	int8_t shaping_a2_q1_6;
	int8_t shaping_b1_q1_6;
	int8_t shaping_b2_q1_6;
	bool codec_plane_differs;
};

enum v90_pcm_mapping_result v90_pcm_mapping_init(
	struct v90_pcm_mapping *mapping,
	const struct v90_cp_parameters *parameters);
bool v90_pcm_mapping_is_valid(const struct v90_pcm_mapping *mapping);

/* Map K LSB-first modulus bits into labels K0..K5. Rejected calls leave the
 * destination unchanged. */
enum v90_pcm_mapping_result v90_pcm_mapping_encode_modulus(
	const struct v90_pcm_mapping *mapping,
	const uint8_t *bits, size_t bit_count,
	uint8_t labels[V90_CP_FRAME_INTERVALS]);

/* Recover K LSB-first modulus bits from labels K0..K5. Label tuples outside
 * the 2^K input range are unused and rejected. Rejected calls leave the
 * destination unchanged. */
enum v90_pcm_mapping_result v90_pcm_mapping_decode_modulus(
	const struct v90_pcm_mapping *mapping,
	const uint8_t labels[V90_CP_FRAME_INTERVALS],
	uint8_t *bits, size_t bit_count);

/* Resolve a label using the descending-Ucode ordering required by the PCM
 * mapper. Codec masks with incompatible cardinality fall back atomically to
 * the transmitter plane during initialization. */
enum v90_pcm_mapping_result v90_pcm_mapping_ucode(
	const struct v90_pcm_mapping *mapping,
	enum v90_pcm_mapping_plane plane, size_t interval,
	uint8_t label, uint8_t *ucode);

/* Reverse a Ucode to its descending-order constellation label. Ucodes absent
 * from the selected interval and plane are rejected without changing label. */
enum v90_pcm_mapping_result v90_pcm_mapping_label(
	const struct v90_pcm_mapping *mapping,
	enum v90_pcm_mapping_plane plane, size_t interval,
	uint8_t ucode, uint8_t *label);

const char *v90_pcm_mapping_result_name(
	enum v90_pcm_mapping_result result);

#endif /* V90_PCM_MAPPING_H */
