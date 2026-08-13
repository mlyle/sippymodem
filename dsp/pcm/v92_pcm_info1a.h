// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_PCM_INFO1A_H
#define V92_PCM_INFO1A_H

#include "v90_info1.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V92_PCM_INFO1A_PAYLOAD_BITS = V90_INFO1A_PAYLOAD_BITS,
	V92_PCM_INFO1A_PAYLOAD_BYTES = V90_INFO1A_PAYLOAD_BYTES,
	V92_PCM_INFO1A_MIN_TOTAL_COEFFICIENTS = 192,
	V92_PCM_INFO1A_MAX_TOTAL_COEFFICIENTS = 384,
	V92_PCM_INFO1A_MIN_SECTION_COEFFICIENTS = 128,
	V92_PCM_INFO1A_MAX_SECTION_COEFFICIENTS = 320,
	V92_PCM_INFO1A_COEFFICIENT_STEP = 64,
	V92_PCM_INFO1A_MAX_MD_LENGTH_UNITS = 127,
};

enum v92_pcm_filter_sections {
	V92_PCM_FILTER_P1_Z2,
	V92_PCM_FILTER_Z1_P1_Z2,
	V92_PCM_FILTER_P1_P2_Z2,
	V92_PCM_FILTER_Z1_P1_P2_Z2,
};

enum v92_pcm_info1a_result {
	V92_PCM_INFO1A_OK,
	V92_PCM_INFO1A_INVALID_ARGUMENT,
	V92_PCM_INFO1A_INVALID_SELECTION,
	V92_PCM_INFO1A_INVALID_PAYLOAD,
};

/* V.92 Amendment 1 Table 18. Both symbol-rate fields are fixed at 8000;
 * their shared value is the wire discriminator from conventional V.34
 * upstream. Coefficient limits are represented as actual counts rather than
 * their two-bit wire codes. */
struct v92_pcm_info1a {
	enum v92_pcm_filter_sections filter_sections;
	uint16_t maximum_total_coefficients;
	uint16_t maximum_section_coefficients;
	uint8_t local_md_length_276_symbol_units;
	uint8_t training_ucode;
};

bool v92_pcm_info1a_is_valid(const struct v92_pcm_info1a *info1a);

enum v92_pcm_info1a_result v92_pcm_info1a_encode(
	const struct v92_pcm_info1a *info1a, uint8_t *payload,
	size_t payload_capacity);
enum v92_pcm_info1a_result v92_pcm_info1a_decode(
	const uint8_t *payload, size_t payload_capacity,
	size_t payload_bit_count, struct v92_pcm_info1a *info1a);

const char *v92_pcm_info1a_result_name(
	enum v92_pcm_info1a_result result);

#endif /* V92_PCM_INFO1A_H */
