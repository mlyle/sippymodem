// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_SYMBOL_RATE_H
#define V34_SYMBOL_RATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum v34_symbol_rate {
	V34_SYMBOL_RATE_2400,
	V34_SYMBOL_RATE_2743,
	V34_SYMBOL_RATE_2800,
	V34_SYMBOL_RATE_3000,
	V34_SYMBOL_RATE_3200,
	V34_SYMBOL_RATE_3429,
	V34_SYMBOL_RATE_COUNT,
};

struct v34_symbol_rate_parameters {
	uint16_t numerator;
	uint8_t denominator;
	uint8_t data_frames_per_superframe;
	uint8_t mapping_frames_per_data_frame;
	uint8_t minimum_primary_rate_2400_bps_units;
	uint8_t maximum_primary_rate_2400_bps_units;
};

/* One immutable definition of the exact clock and its framing cadence.
 * J data frames of P eight-symbol mappings span one 280 ms superframe:
 * baud = J * P * 200/7. Invalid selections have no descriptor. */
static inline const struct v34_symbol_rate_parameters *
v34_symbol_rate_parameters(enum v34_symbol_rate rate)
{
	static const struct v34_symbol_rate_parameters parameters[] = {
		[V34_SYMBOL_RATE_2400] = { 2400, 1, 7, 12, 1, 9 },
		[V34_SYMBOL_RATE_2743] = { 19200, 7, 8, 12, 2, 11 },
		[V34_SYMBOL_RATE_2800] = { 2800, 1, 7, 14, 2, 11 },
		[V34_SYMBOL_RATE_3000] = { 3000, 1, 7, 15, 2, 12 },
		[V34_SYMBOL_RATE_3200] = { 3200, 1, 7, 16, 2, 13 },
		[V34_SYMBOL_RATE_3429] = { 24000, 7, 8, 15, 2, 14 },
	};
	_Static_assert(sizeof(parameters) / sizeof(parameters[0]) == V34_SYMBOL_RATE_COUNT,
		"symbol-rate descriptors must cover every selection");

	return (unsigned)rate < V34_SYMBOL_RATE_COUNT ? &parameters[rate] : NULL;
}

/* Decode the integer baud labels used by configuration. Labels 2743 and
 * 3429 denote exact clocks 19200/7 and 24000/7, not rounded DSP clocks.
 * Zero (automatic selection) and protocol-specific rate restrictions belong
 * to the caller. Reject unknown labels or NULL without changing the output.
 * Match the AT parser width so a wide input cannot wrap into a valid label. */
static inline bool v34_symbol_rate_from_number(
	unsigned long number, enum v34_symbol_rate *symbol_rate)
{
	if (symbol_rate == NULL)
		return false;
	for (unsigned rate = 0; rate < V34_SYMBOL_RATE_COUNT; ++rate) {
		const struct v34_symbol_rate_parameters *parameters =
			v34_symbol_rate_parameters((enum v34_symbol_rate)rate);

		if (number == (unsigned long)(parameters->numerator +
				parameters->denominator / 2u) / parameters->denominator) {
			*symbol_rate = (enum v34_symbol_rate)rate;
			return true;
		}
	}
	return false;
}

#endif /* V34_SYMBOL_RATE_H */
