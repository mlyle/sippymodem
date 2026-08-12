// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_DIL_H
#define V90_DIL_H

#include <stddef.h>
#include <stdint.h>

enum {
	V90_DIL_FRAME_SYNC_BITS = 17,
	V90_DIL_ZERO_DESCRIPTOR_BITS = 240,
	V90_DIL_ZERO_DESCRIPTOR_BYTES = V90_DIL_ZERO_DESCRIPTOR_BITS / 8,
};

enum v90_dil_result {
	V90_DIL_OK,
	V90_DIL_INVALID_ARGUMENT,
	V90_DIL_INVALID_FRAME,
	V90_DIL_CRC_MISMATCH,
	V90_DIL_NONZERO_DESCRIPTOR,
};

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
