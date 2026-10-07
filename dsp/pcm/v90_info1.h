// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_INFO1_H
#define V90_INFO1_H

#include "v34_info1.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V90_INFO1A_PAYLOAD_BITS = 38,
	V90_INFO1A_PAYLOAD_BYTES =
		(V90_INFO1A_PAYLOAD_BITS + 7) / 8,
	V90_INFO1A_MIN_TRAINING_UCODE = 67,
	V90_INFO1A_MAX_TRAINING_UCODE = 127,
};

enum v90_info1_result {
	V90_INFO1_OK,
	V90_INFO1_INVALID_ARGUMENT,
	V90_INFO1_INVALID_SELECTION,
	V90_INFO1_INVALID_PAYLOAD,
};

/* V.90 6.2 permits only these three conventional upstream clocks. */
static inline bool v90_upstream_symbol_rate_is_valid(enum v34_symbol_rate rate)
{
	return rate >= V34_SYMBOL_RATE_3000 && rate <= V34_SYMBOL_RATE_3429;
}

/* This is the INFO1a shape that selects V.90 rather than the V.34 fallback.
 * The digital modem's downstream symbol rate is fixed at 8000 and therefore
 * is not exposed as mutable state. Frequency offset is in exact 0.02 Hz units;
 * -512 means that a sufficiently accurate measurement was unavailable. */
struct v90_info1a {
	uint8_t local_md_length_35_ms_units;
	uint8_t training_ucode;
	enum v34_symbol_rate upstream_symbol_rate;
	int16_t frequency_offset_1_50_hz_units;
};

bool v90_info1a_is_valid(const struct v90_info1a *info1a);

/* INFO1d is bit-for-bit the V.34 INFO1c record. Use struct v34_info1c and the
 * v34_info1c codec directly so the shared wire format has one implementation. */
enum v90_info1_result v90_info1a_encode(
	const struct v90_info1a *info1a, uint8_t *payload,
	size_t payload_capacity);
enum v90_info1_result v90_info1a_decode(
	const uint8_t *payload, size_t payload_capacity,
	size_t payload_bit_count, struct v90_info1a *info1a);

const char *v90_info1_result_name(enum v90_info1_result result);

#endif
