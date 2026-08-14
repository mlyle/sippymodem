// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_CPU_H
#define V92_CPU_H

#include "g711.h"
#include "pcm_constellation.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V92_CPU_FRAME_INTERVALS = 6,
	V92_CPU_MAX_CONSTELLATIONS = 6,
	V92_CPU_HEADER_BITS = 136,
	V92_CPU_CONSTELLATION_BITS = 136,
	V92_CPU_MIN_TRAILER_BITS = 18,
	V92_CPU_SYMBOLS_PER_FRAME = 12,
	V92_CPU_MIN_BITS_PER_SYMBOL = 2,
	V92_CPU_MAX_BITS_PER_SYMBOL = 4,
	V92_CPU_MIN_FRAME_BITS =
		(V92_CPU_HEADER_BITS + V92_CPU_CONSTELLATION_BITS +
		 V92_CPU_MIN_TRAILER_BITS +
		 V92_CPU_MIN_BITS_PER_SYMBOL * V92_CPU_SYMBOLS_PER_FRAME - 1) /
		(V92_CPU_MIN_BITS_PER_SYMBOL * V92_CPU_SYMBOLS_PER_FRAME) *
		(V92_CPU_MIN_BITS_PER_SYMBOL * V92_CPU_SYMBOLS_PER_FRAME),
	V92_CPU_MAX_FRAME_BITS =
		(V92_CPU_HEADER_BITS + 2 * V92_CPU_MAX_CONSTELLATIONS *
		 V92_CPU_CONSTELLATION_BITS + V92_CPU_MIN_TRAILER_BITS +
		 V92_CPU_MAX_BITS_PER_SYMBOL * V92_CPU_SYMBOLS_PER_FRAME - 1) /
		(V92_CPU_MAX_BITS_PER_SYMBOL * V92_CPU_SYMBOLS_PER_FRAME) *
		(V92_CPU_MAX_BITS_PER_SYMBOL * V92_CPU_SYMBOLS_PER_FRAME),
	V92_CPU_MAX_FRAME_BYTES = (V92_CPU_MAX_FRAME_BITS + 7) / 8,
	V92_CPU_MAX_DATA_RATE_NUMBER = 22,
};

enum v92_cpu_result {
	V92_CPU_OK,
	V92_CPU_INVALID_ARGUMENT,
	V92_CPU_INVALID_PARAMETERS,
	V92_CPU_OUTPUT_TOO_SMALL,
	V92_CPU_INVALID_FRAME,
	V92_CPU_CRC_MISMATCH,
};

/* Long CPu fields from Table 23. Fixed-point values remain in their wire
 * representation. A data_rate_number of zero requests cleardown; 1 through
 * 22 select (data_rate_number + 20) * 8000/6 bit/s. */
struct v92_cpu_parameters {
	struct pcm_constellation
		transmit_constellations[V92_CPU_MAX_CONSTELLATIONS];
	struct pcm_constellation
		codec_constellations[V92_CPU_MAX_CONSTELLATIONS];
	enum g711_law law;
	uint16_t trn1d_ratio_q3_13;
	int8_t shaping_a1_q1_6;
	int8_t shaping_a2_q1_6;
	int8_t shaping_b1_q1_6;
	int8_t shaping_b2_q1_6;
	uint8_t data_rate_number;
	uint8_t sign_redundancy;
	uint8_t spectral_lookahead_frames;
	uint8_t constellation_count;
	uint8_t frame_constellation[V92_CPU_FRAME_INTERVALS];
	bool acknowledges_cpd;
	bool codec_constellations_differ;
};

bool v92_cpu_parameters_are_valid(
	const struct v92_cpu_parameters *parameters);

/* bits_per_symbol is the modulation width carrying the record. The result is
 * an integral number of 12-symbol frames, or zero for invalid inputs. */
size_t v92_cpu_frame_bits(
	const struct v92_cpu_parameters *parameters,
	unsigned int bits_per_symbol);
size_t v92_cpu_frame_bytes(
	const struct v92_cpu_parameters *parameters,
	unsigned int bits_per_symbol);

/* Pack bits in transmission order, most-significant bit first within each
 * storage octet. Start, reserved, fill, and storage-padding bits are zero.
 * Rejected calls leave every output unchanged. */
enum v92_cpu_result v92_cpu_encode(
	const struct v92_cpu_parameters *parameters,
	unsigned int bits_per_symbol,
	uint8_t *frame, size_t frame_capacity,
	size_t *frame_bit_count);

/* frame_bit_count is the exact wire length. Reserved information bits are
 * covered by the CRC but otherwise ignored. Fixed, start, fill, and storage-
 * padding bits are strict. Rejected calls leave parameters unchanged. */
enum v92_cpu_result v92_cpu_decode(
	const uint8_t *frame, size_t frame_capacity,
	size_t frame_bit_count, unsigned int bits_per_symbol,
	struct v92_cpu_parameters *parameters);

const char *v92_cpu_result_name(enum v92_cpu_result result);

#endif /* V92_CPU_H */
