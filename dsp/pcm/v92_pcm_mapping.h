// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_PCM_MAPPING_H
#define V92_PCM_MAPPING_H

#include "g711.h"
#include "v92_cpd.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V92_PCM_MAPPING_FRAME_SYMBOLS = V92_CPD_MODULUS_COUNT,
	V92_PCM_MAPPING_PHASES = V92_CPD_FRAME_INTERVALS,
	V92_PCM_MAPPING_RATE_INDEX_BASE = 17,
	V92_PCM_MAPPING_MIN_BITS = 36,
	V92_PCM_MAPPING_MAX_BITS = 72,
};

enum v92_pcm_mapping_result {
	V92_PCM_MAPPING_OK,
	V92_PCM_MAPPING_INVALID_ARGUMENT,
	V92_PCM_MAPPING_INVALID_PARAMETERS,
	V92_PCM_MAPPING_OUT_OF_RANGE,
};

/* Owned initial PCM-upstream mapping. An initial CPd must provide every
 * variable section; later parameter-mode updates may inherit those sections
 * from this owner instead of constructing a second mapping representation. */
struct v92_pcm_mapping {
	struct v92_cpd_parameters cpd;
	enum g711_law law;
	uint8_t data_bits_per_frame;
};

/* CPd rate number zero is cleardown; unsupported values return zero. */
uint8_t v92_pcm_mapping_data_bits(uint8_t data_rate_number);

enum v92_pcm_mapping_result v92_pcm_mapping_init(
	struct v92_pcm_mapping *mapping, enum g711_law law,
	const struct v92_cpd_parameters *cpd);
bool v92_pcm_mapping_is_valid(const struct v92_pcm_mapping *mapping);
bool v92_pcm_mapping_equal(
	const struct v92_pcm_mapping *left,
	const struct v92_pcm_mapping *right);

/* Convert one LSB-first upstream frame through the differential mixed-radix
 * mapper. State and output change together only after a valid operation. */
enum v92_pcm_mapping_result v92_pcm_mapping_encode(
	const struct v92_pcm_mapping *mapping,
	const uint8_t *bits, size_t bit_count,
	uint8_t *differential_state,
	uint8_t digits[V92_PCM_MAPPING_FRAME_SYMBOLS],
	uint8_t *frame_sign);
enum v92_pcm_mapping_result v92_pcm_mapping_decode(
	const struct v92_pcm_mapping *mapping,
	const uint8_t digits[V92_PCM_MAPPING_FRAME_SYMBOLS],
	uint8_t *differential_state,
	uint8_t *bits, size_t bit_count);

/* Internal frame-owner path: mapping must already be validated and remain
 * unchanged for the operation. Skip only the immutable CPd/geometry scan;
 * null arguments, digit ranges, frame length, differential state and spare
 * codewords retain the checked decoder's transactional rejection contract. */
enum v92_pcm_mapping_result v92_pcm_mapping_decode_owned(
	const struct v92_pcm_mapping *mapping,
	const uint8_t digits[V92_PCM_MAPPING_FRAME_SYMBOLS],
	uint8_t *differential_state,
	uint8_t *bits, size_t bit_count);

/* Resolve the positive half-constellation selected for one frame symbol.
 * Signed point selection and trellis equivalence remain the frame coder's
 * responsibility. Rejected calls leave level unchanged. */
enum v92_pcm_mapping_result v92_pcm_mapping_positive_level(
	const struct v92_pcm_mapping *mapping, size_t symbol,
	uint8_t ordinal, int16_t *level);

const char *v92_pcm_mapping_result_name(
	enum v92_pcm_mapping_result result);

#endif /* V92_PCM_MAPPING_H */
