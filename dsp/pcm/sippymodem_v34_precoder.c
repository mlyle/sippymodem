// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v34_precoder.h"

#include <limits.h>
#include <stddef.h>

static bool grid_step_is_valid(enum sippymodem_v34_precoder_grid_step step)
{
	return step == SIPPYMODEM_V34_PRECODER_GRID_STEP_2 ||
	       step == SIPPYMODEM_V34_PRECODER_GRID_STEP_4;
}

static bool coefficient_is_valid(
	const struct sippymodem_v34_q14_coefficient *coefficient)
{
	/* INT16_MIN represents -2 exactly and is outside the specified
	 * half-open coefficient interval. */
	return coefficient->real != INT16_MIN &&
	       coefficient->imaginary != INT16_MIN;
}

bool sippymodem_v34_precoder_is_valid(
	const struct sippymodem_v34_precoder *precoder)
{
	if (precoder == NULL || !grid_step_is_valid(precoder->grid_step))
		return false;
	for (size_t tap = 0; tap < SIPPYMODEM_V34_PRECODER_TAP_COUNT;
	     ++tap) {
		if (!coefficient_is_valid(&precoder->coefficients[tap]))
			return false;
	}
	return true;
}

enum sippymodem_v34_precoder_result sippymodem_v34_precoder_init(
	struct sippymodem_v34_precoder *precoder,
	const struct sippymodem_v34_mapping_parameters *parameters,
	const struct sippymodem_v34_q14_coefficient
		coefficients[SIPPYMODEM_V34_PRECODER_TAP_COUNT])
{
	struct sippymodem_v34_precoder initialized = { 0 };

	if (precoder == NULL ||
	    !sippymodem_v34_mapping_parameters_is_valid(parameters) ||
	    coefficients == NULL)
		return SIPPYMODEM_V34_PRECODER_INVALID_ARGUMENT;
	for (size_t tap = 0; tap < SIPPYMODEM_V34_PRECODER_TAP_COUNT;
	     ++tap) {
		if (!coefficient_is_valid(&coefficients[tap]))
			return SIPPYMODEM_V34_PRECODER_INVALID_ARGUMENT;
		initialized.coefficients[tap] = coefficients[tap];
	}
	initialized.grid_step = parameters->mapping_frame_bits < 56u ?
		SIPPYMODEM_V34_PRECODER_GRID_STEP_2 :
		SIPPYMODEM_V34_PRECODER_GRID_STEP_4;
	*precoder = initialized;
	return SIPPYMODEM_V34_PRECODER_OK;
}

static bool mapper_output_is_valid(
	const struct sippymodem_v34_grid_point *mapper_output)
{
	return mapper_output != NULL &&
	       ((uint16_t)mapper_output->in_phase & 1u) != 0u &&
	       ((uint16_t)mapper_output->quadrature & 1u) != 0u;
}

static int64_t round_divide_ties_toward_zero(
	int64_t value, int64_t divisor)
{
	int64_t half_minus_one = divisor / 2 - 1;

	if (value >= 0)
		return (value + half_minus_one) / divisor;
	return -((-value + half_minus_one) / divisor);
}

static bool int64_fits_int32(int64_t value)
{
	return value >= INT32_MIN && value <= INT32_MAX;
}

static bool int64_fits_int16(int64_t value)
{
	return value >= INT16_MIN && value <= INT16_MAX;
}

static bool filter_output(
	const struct sippymodem_v34_precoder *precoder,
	struct sippymodem_v34_q7_point *rounded)
{
	int64_t in_phase = 0;
	int64_t quadrature = 0;

	for (size_t tap = 0; tap < SIPPYMODEM_V34_PRECODER_TAP_COUNT;
	     ++tap) {
		const struct sippymodem_v34_q7_point *previous =
			&precoder->previous_symbols[tap];
		const struct sippymodem_v34_q14_coefficient *coefficient =
			&precoder->coefficients[tap];

		in_phase += (int64_t)previous->in_phase * coefficient->real -
			(int64_t)previous->quadrature * coefficient->imaginary;
		quadrature +=
			(int64_t)previous->in_phase * coefficient->imaginary +
			(int64_t)previous->quadrature * coefficient->real;
	}
	in_phase = round_divide_ties_toward_zero(
		in_phase, SIPPYMODEM_V34_Q14_SCALE);
	quadrature = round_divide_ties_toward_zero(
		quadrature, SIPPYMODEM_V34_Q14_SCALE);
	if (!int64_fits_int32(in_phase) || !int64_fits_int32(quadrature))
		return false;
	*rounded = (struct sippymodem_v34_q7_point){
		.in_phase = (int32_t)in_phase,
		.quadrature = (int32_t)quadrature,
	};
	return true;
}

static bool quantize_filter_output(
	const struct sippymodem_v34_q7_point *filter,
	enum sippymodem_v34_precoder_grid_step grid_step,
	struct sippymodem_v34_grid_point *offset)
{
	int64_t divisor = (int64_t)grid_step * SIPPYMODEM_V34_Q7_SCALE;
	int64_t in_phase = round_divide_ties_toward_zero(
		filter->in_phase, divisor) * grid_step;
	int64_t quadrature = round_divide_ties_toward_zero(
		filter->quadrature, divisor) * grid_step;

	if (!int64_fits_int16(in_phase) || !int64_fits_int16(quadrature))
		return false;
	*offset = (struct sippymodem_v34_grid_point){
		.in_phase = (int16_t)in_phase,
		.quadrature = (int16_t)quadrature,
	};
	return true;
}

static bool generate_symbol(
	const struct sippymodem_v34_precoder *precoder,
	const struct sippymodem_v34_grid_point *mapper_output,
	struct sippymodem_v34_precoder_symbol *symbol)
{
	struct sippymodem_v34_precoder_symbol generated;
	int64_t channel_in_phase;
	int64_t channel_quadrature;
	int64_t precoded_in_phase;
	int64_t precoded_quadrature;

	if (!filter_output(precoder, &generated.filter_output) ||
	    !quantize_filter_output(
		    &generated.filter_output, precoder->grid_step,
		    &generated.lattice_offset))
		return false;
	channel_in_phase = (int64_t)mapper_output->in_phase +
		generated.lattice_offset.in_phase;
	channel_quadrature = (int64_t)mapper_output->quadrature +
		generated.lattice_offset.quadrature;
	if (channel_in_phase < -SIPPYMODEM_V34_CHANNEL_COORDINATE_LIMIT ||
	    channel_in_phase > SIPPYMODEM_V34_CHANNEL_COORDINATE_LIMIT ||
	    channel_quadrature < -SIPPYMODEM_V34_CHANNEL_COORDINATE_LIMIT ||
	    channel_quadrature > SIPPYMODEM_V34_CHANNEL_COORDINATE_LIMIT)
		return false;
	generated.channel_output = (struct sippymodem_v34_grid_point){
		.in_phase = (int16_t)channel_in_phase,
		.quadrature = (int16_t)channel_quadrature,
	};
	precoded_in_phase =
		channel_in_phase * SIPPYMODEM_V34_Q7_SCALE -
		generated.filter_output.in_phase;
	precoded_quadrature =
		channel_quadrature * SIPPYMODEM_V34_Q7_SCALE -
		generated.filter_output.quadrature;
	if (!int64_fits_int32(precoded_in_phase) ||
	    !int64_fits_int32(precoded_quadrature))
		return false;
	generated.precoded_symbol = (struct sippymodem_v34_q7_point){
		.in_phase = (int32_t)precoded_in_phase,
		.quadrature = (int32_t)precoded_quadrature,
	};
	*symbol = generated;
	return true;
}

enum sippymodem_v34_precoder_result sippymodem_v34_precoder_step(
	struct sippymodem_v34_precoder *precoder,
	const struct sippymodem_v34_grid_point *mapper_output,
	struct sippymodem_v34_precoder_symbol *symbol)
{
	struct sippymodem_v34_precoder next;
	struct sippymodem_v34_precoder_symbol generated;

	if (!sippymodem_v34_precoder_is_valid(precoder) ||
	    !mapper_output_is_valid(mapper_output) || symbol == NULL)
		return SIPPYMODEM_V34_PRECODER_INVALID_ARGUMENT;
	if (!generate_symbol(precoder, mapper_output, &generated))
		return SIPPYMODEM_V34_PRECODER_OUT_OF_RANGE;
	next = *precoder;
	for (size_t tap = SIPPYMODEM_V34_PRECODER_TAP_COUNT - 1; tap > 0;
	     --tap)
		next.previous_symbols[tap] = next.previous_symbols[tap - 1];
	next.previous_symbols[0] = generated.precoded_symbol;
	*precoder = next;
	*symbol = generated;
	return SIPPYMODEM_V34_PRECODER_OK;
}

const char *sippymodem_v34_precoder_result_name(
	enum sippymodem_v34_precoder_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_PRECODER_OK:
		return "ok";
	case SIPPYMODEM_V34_PRECODER_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_V34_PRECODER_OUT_OF_RANGE:
		return "out of range";
	}
	return "unknown";
}
