// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_SUVU_H
#define V92_SUVU_H

#include "v92_j.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V92_SUVU_INFORMATION_BITS = 34,
	V92_SUVU_TRAILER_BITS = 18,
	V92_SUVU_SYMBOLS_PER_FRAME = 12,
	V92_SUVU_FRAME_BITS = 72,
	V92_SUVU_FRAME_BYTES = V92_SUVU_FRAME_BITS / 8,
	V92_SUVU_NO_MEASUREMENT_Q2_2 = -16,
};

enum v92_suvu_result {
	V92_SUVU_OK,
	V92_SUVU_INVALID_ARGUMENT,
	V92_SUVU_INVALID_PARAMETERS,
	V92_SUVU_OUTPUT_TOO_SMALL,
	V92_SUVU_INVALID_FRAME,
	V92_SUVU_CRC_MISMATCH,
};

/* Exact Table 27 information fields. prefilter_level_db_q2_2 is signed
 * five-bit Q2.2; -16 is the specified "not measured" representation. */
struct v92_suvu_parameters {
	int8_t prefilter_level_db_q2_2;
	bool wait_for_cpu;
	bool request_silence;
	bool acknowledges_cpd;
};

bool v92_suvu_parameters_are_valid(
	const struct v92_suvu_parameters *parameters);

/* Return zero for a constellation that cannot carry initial SUVu. Both
 * permitted TRN2u constellations yield the same 72-bit training record. */
size_t v92_suvu_frame_bits(enum v92_constellation_size constellation);
size_t v92_suvu_frame_bytes(enum v92_constellation_size constellation);

/* Pack bits in transmission order, most-significant bit first within each
 * storage octet. Rejected calls leave every output unchanged. */
enum v92_suvu_result v92_suvu_encode(
	const struct v92_suvu_parameters *parameters,
	enum v92_constellation_size constellation,
	uint8_t *frame, size_t frame_capacity, size_t *frame_bit_count);

/* frame_bit_count is the exact wire length. Reserved bits are covered by the
 * CRC but otherwise ignored. Fixed fields and all fill bits are strict.
 * Rejected calls leave parameters unchanged. */
enum v92_suvu_result v92_suvu_decode(
	const uint8_t *frame, size_t frame_capacity, size_t frame_bit_count,
	enum v92_constellation_size constellation,
	struct v92_suvu_parameters *parameters);

const char *v92_suvu_result_name(enum v92_suvu_result result);

#endif /* V92_SUVU_H */
