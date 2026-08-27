// SPDX-License-Identifier: BSD-3-Clause

#include "v34_carrier_demodulator.h"

#include "fixed_phase.h"
#include "integer_math.h"
#include "memory_region.h"
#include "phase.h"

#include <stdint.h>
#include <string.h>

static int16_t rounded_q14(uint32_t accumulator)
{
	return wrap_s16_from_s32(arithmetic_shift_right_u32(
		accumulator + UINT32_C(0x2000), 14));
}

bool v34_carrier_demodulator_is_valid(
	const struct v34_carrier_demodulator *demodulator)
{
	return demodulator != NULL && demodulator->initialized &&
	       demodulator->phase_step != 0;
}

enum v34_carrier_demodulator_result v34_carrier_demodulator_init(
	struct v34_carrier_demodulator *demodulator,
	uint32_t sample_rate_hz, uint32_t frequency_numerator,
	uint32_t frequency_denominator)
{
	struct v34_carrier_demodulator initialized;
	uint64_t scaled_rate;

	if (demodulator == NULL || sample_rate_hz == 0 ||
	    frequency_numerator == 0 || frequency_denominator == 0)
		return V34_CARRIER_DEMODULATOR_INVALID_ARGUMENT;
	scaled_rate = (uint64_t)sample_rate_hz * frequency_denominator;
	if ((uint64_t)frequency_numerator * 2u >= scaled_rate)
		return V34_CARRIER_DEMODULATOR_INVALID_ARGUMENT;
	memset(&initialized, 0, sizeof(initialized));
	initialized.phase_step = phase_step_u32(
		sample_rate_hz, frequency_numerator,
		frequency_denominator);
	initialized.initialized = true;
	if (!v34_carrier_demodulator_is_valid(&initialized))
		return V34_CARRIER_DEMODULATOR_INVALID_ARGUMENT;
	*demodulator = initialized;
	return V34_CARRIER_DEMODULATOR_OK;
}

enum v34_carrier_demodulator_result v34_carrier_demodulator_process(
	struct v34_carrier_demodulator *demodulator,
	struct v34_analytic_sample input,
	struct v34_analytic_sample *output)
{
	struct v34_carrier_demodulator next;
	struct fixed_phase_q14 carrier;
	struct v34_analytic_sample generated;
	uint32_t real;
	uint32_t quadrature;

	if (!v34_carrier_demodulator_is_valid(demodulator))
		return V34_CARRIER_DEMODULATOR_INVALID_STATE;
	if (output == NULL || memory_regions_overlap(
			demodulator, sizeof(*demodulator),
			output, sizeof(*output)))
		return V34_CARRIER_DEMODULATOR_INVALID_ARGUMENT;
	next = *demodulator;
	carrier = fixed_phase_sin_cos_u16_q14(
		(uint16_t)(next.phase >> 16));
	next.phase += next.phase_step;
	real = (uint32_t)((int32_t)input.in_phase * carrier.cosine) +
	       (uint32_t)((int32_t)input.quadrature * carrier.sine);
	quadrature =
		(uint32_t)((int32_t)carrier.cosine * input.quadrature) -
		(uint32_t)((int32_t)carrier.sine * input.in_phase);
	generated = (struct v34_analytic_sample){
		.in_phase = rounded_q14(real),
		.quadrature = rounded_q14(quadrature),
	};
	*demodulator = next;
	*output = generated;
	return V34_CARRIER_DEMODULATOR_OK;
}
