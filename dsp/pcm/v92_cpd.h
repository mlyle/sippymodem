// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_CPD_H
#define V92_CPD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V92_CPD_FRAME_INTERVALS = 6,
	V92_CPD_MODULUS_COUNT = 12,
	V92_CPD_MAX_CONSTELLATIONS = 6,
	V92_CPD_MAX_CONSTELLATION_POINTS = 128,
	V92_CPD_MAX_SECTION_COEFFICIENTS = 320,
	V92_CPD_MAX_TOTAL_COEFFICIENTS = 384,
	V92_CPD_DOWNSTREAM_FRAME_MIN_BITS = 9,
	V92_CPD_DOWNSTREAM_FRAME_MAX_BITS = 30,
	V92_CPD_FIXED_BITS = 51,
	V92_CPD_MODULUS_BITS = 102,
	V92_CPD_FILTER_HEADER_BITS = 68,
	V92_CPD_CONSTELLATION_HEADER_BITS = 85,
	V92_CPD_WORD_BITS = 17,
	V92_CPD_TRAILER_BITS = 18,
	V92_CPD_MAX_PAYLOAD_BITS =
		V92_CPD_FIXED_BITS + V92_CPD_MODULUS_BITS +
		V92_CPD_FILTER_HEADER_BITS +
		V92_CPD_MAX_TOTAL_COEFFICIENTS * V92_CPD_WORD_BITS +
		V92_CPD_CONSTELLATION_HEADER_BITS +
		V92_CPD_MAX_CONSTELLATIONS *
			V92_CPD_MAX_CONSTELLATION_POINTS * V92_CPD_WORD_BITS,
	V92_CPD_MAX_FRAME_BITS =
		V92_CPD_MAX_PAYLOAD_BITS + V92_CPD_TRAILER_BITS +
		V92_CPD_DOWNSTREAM_FRAME_MAX_BITS - 1,
	V92_CPD_MAX_FRAME_BYTES = (V92_CPD_MAX_FRAME_BITS + 7) / 8,
	V92_CPD_MAX_DATA_RATE_NUMBER = 19,
};

enum v92_cpd_trellis {
	V92_CPD_TRELLIS_16_STATE,
	V92_CPD_TRELLIS_32_STATE,
	V92_CPD_TRELLIS_64_STATE,
};

enum v92_cpd_filter_section {
	V92_CPD_PRECODER_FEED_FORWARD,
	V92_CPD_PRECODER_FEEDBACK,
	V92_CPD_PREFILTER_FEED_FORWARD,
	V92_CPD_PREFILTER_FEEDBACK,
	V92_CPD_FILTER_SECTION_COUNT,
};

enum v92_cpd_result {
	V92_CPD_OK,
	V92_CPD_INVALID_ARGUMENT,
	V92_CPD_INVALID_PARAMETERS,
	V92_CPD_INVALID_ALIGNMENT,
	V92_CPD_OUTPUT_TOO_SMALL,
	V92_CPD_INVALID_FRAME,
	V92_CPD_CRC_MISMATCH,
	V92_CPD_INCOMPLETE,
};

struct v92_cpd_filter {
	int16_t coefficient[V92_CPD_MAX_SECTION_COEFFICIENTS];
	uint16_t count;
};

struct v92_cpd_constellation {
	int16_t positive_linear[V92_CPD_MAX_CONSTELLATION_POINTS];
	uint8_t count;
};

/* V.92 Table 30 fields. Optional sections use a canonical representation:
 * absent sections and unused array entries are zero. Filter sections are z1,
 * p1, z2, and p2 in wire order. Constellation levels are positive, strictly
 * increasing linear PCM values; conversion to the negotiated G.711 law is a
 * later mapping concern. */
struct v92_cpd_parameters {
	struct v92_cpd_filter filter[V92_CPD_FILTER_SECTION_COUNT];
	struct v92_cpd_constellation
		constellation[V92_CPD_MAX_CONSTELLATIONS];
	uint16_t four_times_gain_q0_16;
	uint8_t modulus[V92_CPD_MODULUS_COUNT];
	uint8_t frame_constellation[V92_CPD_FRAME_INTERVALS];
	uint8_t data_rate_number;
	enum v92_cpd_trellis trellis;
	bool extend_e2u;
	bool acknowledges_cpu;
	bool modulus_present;
	bool filters_present;
	bool constellations_present;
};

bool v92_cpd_parameters_are_valid(
	const struct v92_cpd_parameters *parameters);

/* Return zero for invalid parameters or alignment. frame_bits includes the
 * CRC and zero fill through an integral six-symbol downstream mapping frame. */
size_t v92_cpd_payload_bits(
	const struct v92_cpd_parameters *parameters);
size_t v92_cpd_frame_bits(
	const struct v92_cpd_parameters *parameters,
	unsigned int training_frame_bits);
size_t v92_cpd_frame_bytes(
	const struct v92_cpd_parameters *parameters,
	unsigned int training_frame_bits);

/* Infer the exact frame extent once all variable-length counts have arrived.
 * INCOMPLETE is not a wire failure: more packed input is needed to reach a
 * count field. Rejected calls leave frame_bit_count unchanged. */
enum v92_cpd_result v92_cpd_probe_frame_bits(
	const uint8_t *frame, size_t frame_capacity,
	size_t received_bit_count, unsigned int training_frame_bits,
	size_t *frame_bit_count);

/* Bits are packed in transmission order, most-significant bit first in each
 * octet. Rejected calls leave frame and frame_bit_count unchanged. */
enum v92_cpd_result v92_cpd_encode(
	const struct v92_cpd_parameters *parameters,
	unsigned int training_frame_bits,
	uint8_t *frame, size_t frame_capacity,
	size_t *frame_bit_count);

/* Reserved fields are CRC-covered but not interpreted. Fixed row starts,
 * CRC/fill, exact mapping-frame extent, and storage padding are strict.
 * Rejected calls leave parameters unchanged. */
enum v92_cpd_result v92_cpd_decode(
	const uint8_t *frame, size_t frame_capacity,
	size_t frame_bit_count, unsigned int training_frame_bits,
	struct v92_cpd_parameters *parameters);

const char *v92_cpd_result_name(enum v92_cpd_result result);

#endif /* V92_CPD_H */
