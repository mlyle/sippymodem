// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_DIL_H
#define V90_DIL_H

#include "pcm_dil_descriptor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V90_DIL_FRAME_SYNC_BITS = PCM_DIL_FRAME_SYNC_BITS,
	V90_DIL_HEADER_BITS = 50,
	V90_DIL_CHORDS = 8,
	V90_DIL_MAX_PATTERN_BITS = 128,
	V90_DIL_PATTERN_BYTES = V90_DIL_MAX_PATTERN_BITS / 8,
	V90_DIL_MAX_SEGMENTS = 255,
	V90_DIL_MAX_DESCRIPTOR_BITS = 2654,
	V90_DIL_MAX_DESCRIPTOR_BYTES =
		(V90_DIL_MAX_DESCRIPTOR_BITS + 7) / 8,
	V90_DIL_ZERO_DESCRIPTOR_BITS = 240,
	V90_DIL_ZERO_DESCRIPTOR_BYTES = V90_DIL_ZERO_DESCRIPTOR_BITS / 8,
	V90_DIL_SEGMENT_INTERVALS = 6,
};

_Static_assert((int)V90_DIL_MAX_DESCRIPTOR_BITS <=
		       (int)PCM_DIL_DESCRIPTOR_MAX_BITS,
	"V.90 descriptors must fit the common PCM framing bound");

/* Patterns are packed least-significant bit first within each octet. The
 * first bit of each pattern applies to the first symbol of every segment. */
struct v90_dil_parameters {
	uint8_t sign_pattern[V90_DIL_PATTERN_BYTES];
	uint8_t training_pattern[V90_DIL_PATTERN_BYTES];
	uint8_t segment_length_codes[V90_DIL_CHORDS];
	uint8_t reference_ucodes[V90_DIL_CHORDS];
	uint8_t training_ucodes[V90_DIL_MAX_SEGMENTS];
	uint8_t segment_count;
	uint8_t sign_pattern_bits;
	uint8_t training_pattern_bits;
};

enum v90_dil_result {
	V90_DIL_OK,
	V90_DIL_INCOMPLETE,
	V90_DIL_INVALID_ARGUMENT,
	V90_DIL_INVALID_FRAME,
	V90_DIL_CRC_MISMATCH,
	V90_DIL_NONZERO_DESCRIPTOR,
};

enum v90_dil_profile {
	V90_DIL_PROFILE_NONE,
	V90_DIL_PROFILE_CONVENTIONAL,
};

bool v90_dil_parameters_are_valid(
	const struct v90_dil_parameters *parameters);
bool v90_dil_profile_is_valid(enum v90_dil_profile profile);

/* Generate one reviewed profile without storing its 144-entry level walk.
 * The conventional profile covers the PCM levels and phase patterns used to
 * learn the downstream line; NONE retains the standards-defined N=0 path. */
enum v90_dil_result v90_dil_profile_init(
	struct v90_dil_parameters *parameters,
	enum v90_dil_profile profile);

/* Return the exact 8000-character/s duration of one descriptor-selected DIL
 * traversal. N=0 has no DIL and reports zero. Rejected calls leave `symbols`
 * unchanged. */
bool v90_dil_training_symbol_count(
	const struct v90_dil_parameters *parameters, uint32_t *symbols);

/* Recover the full descriptor layout as soon as its fixed 50-bit header has
 * arrived. `received_bits` may exceed the header; it must not exceed the
 * supplied storage. Rejected calls leave `layout` unchanged. */
enum v90_dil_result v90_dil_descriptor_layout(
	const uint8_t *descriptor, size_t descriptor_capacity,
	size_t received_bits, struct pcm_dil_descriptor_layout *layout);

/* Encode or decode one complete variable-length descriptor. Encoding is
 * canonical: reserved and pad bits are zero. Both operations are
 * transactional, and `descriptor_bits` is the exact even wire length. */
enum v90_dil_result v90_dil_encode(
	const struct v90_dil_parameters *parameters,
	uint8_t *descriptor, size_t descriptor_capacity,
	size_t *descriptor_bits);
enum v90_dil_result v90_dil_decode(
	const uint8_t *descriptor, size_t descriptor_capacity,
	size_t descriptor_bits, struct v90_dil_parameters *parameters);

/* Encode the canonical N=0 descriptor used when no digital impairment
 * learning is requested. Bits are packed in transmission order, most-
 * significant bit first within each octet. */
enum v90_dil_result v90_dil_encode_zero(
	uint8_t *descriptor, size_t descriptor_capacity);

/* Validate one complete N=0 descriptor. Reserved and unused parameter values
 * are not interpreted, but remain covered by the CRC. */
enum v90_dil_result v90_dil_decode_zero(
	const uint8_t *descriptor, size_t descriptor_capacity);

const char *v90_dil_result_name(enum v90_dil_result result);

#endif /* V90_DIL_H */
