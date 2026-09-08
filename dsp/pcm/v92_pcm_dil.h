// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_PCM_DIL_H
#define V92_PCM_DIL_H

#include "v90_dil.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V92_PCM_DIL_MIN_DATA_RATE_NUMBER = 1,
	V92_PCM_DIL_MAX_DATA_RATE_NUMBER = 19,
	V92_PCM_DIL_RATE_MASK_ALL =
		(UINT32_C(1) << V92_PCM_DIL_MAX_DATA_RATE_NUMBER) - 1u,
	V92_PCM_DIL_ZERO_DESCRIPTOR_BITS = 276,
	V92_PCM_DIL_ZERO_DESCRIPTOR_BYTES =
		(V92_PCM_DIL_ZERO_DESCRIPTOR_BITS + 7) / 8,
	V92_PCM_DIL_MAX_DESCRIPTOR_BITS = PCM_DIL_DESCRIPTOR_MAX_BITS,
	V92_PCM_DIL_MAX_DESCRIPTOR_BYTES =
		(V92_PCM_DIL_MAX_DESCRIPTOR_BITS + 7) / 8,
};

enum v92_pcm_dil_result {
	V92_PCM_DIL_OK,
	V92_PCM_DIL_INVALID_ARGUMENT,
	V92_PCM_DIL_INVALID_FRAME,
	V92_PCM_DIL_CRC_MISMATCH,
	V92_PCM_DIL_NONZERO_DESCRIPTOR,
	V92_PCM_DIL_INCOMPLETE,
};

struct v92_pcm_dil_parameters {
	struct v90_dil_parameters downstream;
	uint32_t upstream_rate_mask;
};

/* Mask bit drn-1 advertises upstream data rate
 * (drn + 17) * 8000/6 bit/s. A zero mask is syntactically valid; the owning
 * startup controller decides whether it provides a usable intersection. */
bool v92_pcm_dil_rate_mask_is_valid(uint32_t rate_mask);
bool v92_pcm_dil_rate_is_enabled(
	uint32_t rate_mask, uint8_t data_rate_number);
bool v92_pcm_dil_rate_mask_through(
	uint8_t maximum_data_rate_number, uint32_t *rate_mask);

/* V.92 Table 20 extends the V.90 learning parameters with two rate-mask
 * words and pads to a 12-bit boundary. Recover the complete length from the
 * common 50-bit header; received_bits may include a partial body. */
enum v92_pcm_dil_result v92_pcm_dil_layout(
	const uint8_t *descriptor, size_t descriptor_capacity,
	size_t received_bits, struct pcm_dil_descriptor_layout *layout);

/* Complete bounded descriptors, including nonzero DIL requests. Encoding
 * zeros reserved/pad bits. Decoding validates framing and CRC before
 * interpreting fields; reserved mask bits are ignored. Outputs are unchanged
 * on rejection. */
enum v92_pcm_dil_result v92_pcm_dil_encode(
	const struct v92_pcm_dil_parameters *parameters,
	uint8_t *descriptor, size_t descriptor_capacity,
	size_t *descriptor_bits);
enum v92_pcm_dil_result v92_pcm_dil_decode(
	const uint8_t *descriptor, size_t descriptor_capacity,
	size_t descriptor_bits, struct v92_pcm_dil_parameters *parameters);

/* Encode or decode the fixed N=0 descriptor used when no digital impairment
 * learning is requested. Bits are packed in transmission order, most-
 * significant bit first within each octet. */
enum v92_pcm_dil_result v92_pcm_dil_encode_zero(
	uint32_t upstream_rate_mask,
	uint8_t *descriptor, size_t descriptor_capacity);
enum v92_pcm_dil_result v92_pcm_dil_decode_zero(
	const uint8_t *descriptor, size_t descriptor_capacity,
	uint32_t *upstream_rate_mask);

const char *v92_pcm_dil_result_name(enum v92_pcm_dil_result result);

#endif /* V92_PCM_DIL_H */
