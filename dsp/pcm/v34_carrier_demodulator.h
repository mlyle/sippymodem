// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_CARRIER_DEMODULATOR_H
#define V34_CARRIER_DEMODULATOR_H

#include "v34_analytic_filter.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_carrier_demodulator_result {
	V34_CARRIER_DEMODULATOR_OK,
	V34_CARRIER_DEMODULATOR_INVALID_ARGUMENT,
	V34_CARRIER_DEMODULATOR_INVALID_STATE,
};

/* Nominal V.34 carrier phase ownership at the line-sample clock. */
struct v34_carrier_demodulator {
	uint32_t phase;
	uint32_t phase_step;
	bool initialized;
};

enum v34_carrier_demodulator_result v34_carrier_demodulator_init(
	struct v34_carrier_demodulator *demodulator,
	uint32_t sample_rate_hz, uint32_t frequency_numerator,
	uint32_t frequency_denominator);
bool v34_carrier_demodulator_is_valid(
	const struct v34_carrier_demodulator *demodulator);

/* Mix one analytic sample to baseband and advance the nominal carrier.
 * Rejected calls leave the owner and output unchanged. */
enum v34_carrier_demodulator_result v34_carrier_demodulator_process(
	struct v34_carrier_demodulator *demodulator,
	struct v34_analytic_sample input,
	struct v34_analytic_sample *output);

#endif /* V34_CARRIER_DEMODULATOR_H */
