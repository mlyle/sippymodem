// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_CPU_H
#define V92_CPU_H

#include "g711.h"
#include "pcm_constellation.h"
#include "v92_control_frame.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V92_CPU_FRAME_INTERVALS = PCM_CONSTELLATION_FRAME_INTERVALS,
	V92_CPU_MAX_CONSTELLATIONS = PCM_CONSTELLATION_MAX_ROWS,
	V92_CPU_HEADER_BITS = 136,
	V92_CPU_CONSTELLATION_BITS = 136,
	V92_CPU_MIN_TRAILER_BITS = 18,
	V92_CPU_SYMBOLS_PER_FRAME = 12,
	V92_CPU_MIN_FRAME_BITS =
		V92_CPU_HEADER_BITS + V92_CPU_CONSTELLATION_BITS +
		V92_CPU_MIN_TRAILER_BITS,
	V92_CPU_MAX_FRAME_BITS =
		V92_CPU_HEADER_BITS + 2 * V92_CPU_MAX_CONSTELLATIONS *
		V92_CPU_CONSTELLATION_BITS + V92_CPU_MIN_TRAILER_BITS +
		V92_CONTROL_UPSTREAM_DATA_MAX_BITS - 1,
	V92_CPU_MAX_FRAME_BYTES = (V92_CPU_MAX_FRAME_BITS + 7) / 8,
	V92_CPU_MAX_DATA_RATE_NUMBER = 22,
	/* A 52-bit short record rounds to two 50-bit data mapping frames. */
	V92_CPUS_MAX_FRAME_BITS = 100,
	V92_CPUS_MAX_FRAME_BYTES = (V92_CPUS_MAX_FRAME_BITS + 7) / 8,
};

enum v92_cpu_result {
	V92_CPU_OK,
	V92_CPU_INVALID_ARGUMENT,
	V92_CPU_INVALID_PARAMETERS,
	V92_CPU_OUTPUT_TOO_SMALL,
	V92_CPU_INVALID_FRAME,
	V92_CPU_CRC_MISMATCH,
	V92_CPU_INCOMPLETE,
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

/* Table 24: retain the preceding downstream modulation parameters. Used in
 * rate renegotiation or fast parameter exchange, not initial training.
 * Rate zero requests cleardown; 1..22 have the same meaning as long CPu. */
struct v92_cpus_parameters {
	uint8_t data_rate_number;
	bool acknowledges_cpd;
};

/* Same alignment and packed-bit conventions as long CPu below. Invalid
 * widths yield zero. Codecs reject without changing outputs; reserved bits
 * are CRC-covered but ignored on receive, and all fill/padding must be zero. */
size_t v92_cpus_frame_bits(unsigned int mapping_frame_bits);
enum v92_cpu_result v92_cpus_encode(
	const struct v92_cpus_parameters *parameters,
	unsigned int mapping_frame_bits,
	uint8_t *frame, size_t frame_capacity, size_t *frame_bit_count);
enum v92_cpu_result v92_cpus_decode(
	const uint8_t *frame, size_t frame_capacity,
	size_t frame_bit_count, unsigned int mapping_frame_bits,
	struct v92_cpus_parameters *parameters);

bool v92_cpu_parameters_are_valid(
	const struct v92_cpu_parameters *parameters);

/* mapping_frame_bits is the bit width of the preceding twelve-symbol mapping:
 * 24/36 for training, or even 36..72 for fast parameter exchange. The result
 * ends on a complete mapping frame, or is zero for invalid inputs. */
size_t v92_cpu_frame_bits(
	const struct v92_cpu_parameters *parameters,
	unsigned int mapping_frame_bits);
size_t v92_cpu_frame_bytes(
	const struct v92_cpu_parameters *parameters,
	unsigned int mapping_frame_bits);

/* Infer a long CPu extent from its 136-bit header, or a short CPus extent from
 * its 34-bit information field. The separate decoders enforce the wire type.
 * INCOMPLETE requests more header bits; a successful probe does not imply
 * that the remaining constellation rows or CRC have arrived or are valid.
 * Rejected calls leave frame_bit_count unchanged. */
enum v92_cpu_result v92_cpu_probe_frame_bits(
	const uint8_t *frame, size_t frame_capacity,
	size_t received_bit_count, unsigned int mapping_frame_bits,
	size_t *frame_bit_count);

/* Pack bits in transmission order, most-significant bit first within each
 * storage octet. Start, reserved, fill, and storage-padding bits are zero.
 * Rejected calls leave every output unchanged. */
enum v92_cpu_result v92_cpu_encode(
	const struct v92_cpu_parameters *parameters,
	unsigned int mapping_frame_bits,
	uint8_t *frame, size_t frame_capacity,
	size_t *frame_bit_count);

/* frame_bit_count is the exact wire length. Reserved information bits are
 * covered by the CRC but otherwise ignored. Fixed, start, fill, and storage-
 * padding bits are strict. Rejected calls leave parameters unchanged. */
enum v92_cpu_result v92_cpu_decode(
	const uint8_t *frame, size_t frame_capacity,
	size_t frame_bit_count, unsigned int mapping_frame_bits,
	struct v92_cpu_parameters *parameters);

const char *v92_cpu_result_name(enum v92_cpu_result result);

#endif /* V92_CPU_H */
