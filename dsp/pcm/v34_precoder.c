// SPDX-License-Identifier: BSD-3-Clause

#include "v34_precoder.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>

static bool grid_step_is_valid(enum v34_precoder_grid_step step)
{
	return step == V34_PRECODER_GRID_STEP_2 ||
	       step == V34_PRECODER_GRID_STEP_4;
}

static bool coefficient_is_valid(
	const struct v34_q14_coefficient *coefficient)
{
	/* INT16_MIN represents -2 exactly and is outside the specified
	 * half-open coefficient interval. */
	return coefficient->real != INT16_MIN &&
	       coefficient->imaginary != INT16_MIN;
}

bool v34_precoder_is_valid(
	const struct v34_precoder *precoder)
{
	if (precoder == NULL || !grid_step_is_valid(precoder->grid_step))
		return false;
	for (size_t tap = 0; tap < V34_PRECODER_TAP_COUNT;
	     ++tap) {
		if (!coefficient_is_valid(&precoder->coefficients[tap]))
			return false;
	}
	return true;
}

enum v34_precoder_result v34_precoder_init(
	struct v34_precoder *precoder,
	const struct v34_mapping_parameters *parameters,
	const struct v34_q14_coefficient
		coefficients[V34_PRECODER_TAP_COUNT])
{
	struct v34_precoder initialized = { 0 };

	if (precoder == NULL ||
	    !v34_mapping_parameters_is_valid(parameters) ||
	    coefficients == NULL)
		return V34_PRECODER_INVALID_ARGUMENT;
	for (size_t tap = 0; tap < V34_PRECODER_TAP_COUNT;
	     ++tap) {
		if (!coefficient_is_valid(&coefficients[tap]))
			return V34_PRECODER_INVALID_ARGUMENT;
		initialized.coefficients[tap] = coefficients[tap];
	}
	initialized.grid_step = parameters->mapping_frame_bits < 56u ?
		V34_PRECODER_GRID_STEP_2 :
		V34_PRECODER_GRID_STEP_4;
	*precoder = initialized;
	return V34_PRECODER_OK;
}

static bool mapper_output_is_valid(
	const struct v34_grid_point *mapper_output)
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
	const struct v34_precoder *precoder,
	struct v34_q7_point *rounded)
{
	int64_t in_phase = 0;
	int64_t quadrature = 0;

	for (size_t tap = 0; tap < V34_PRECODER_TAP_COUNT;
	     ++tap) {
		const struct v34_q7_point *previous =
			&precoder->previous_symbols[tap];
		const struct v34_q14_coefficient *coefficient =
			&precoder->coefficients[tap];

		in_phase += (int64_t)previous->in_phase * coefficient->real -
			(int64_t)previous->quadrature * coefficient->imaginary;
		quadrature +=
			(int64_t)previous->in_phase * coefficient->imaginary +
			(int64_t)previous->quadrature * coefficient->real;
	}
	in_phase = round_divide_ties_toward_zero(
		in_phase, V34_Q14_SCALE);
	quadrature = round_divide_ties_toward_zero(
		quadrature, V34_Q14_SCALE);
	if (!int64_fits_int32(in_phase) || !int64_fits_int32(quadrature))
		return false;
	*rounded = (struct v34_q7_point){
		.in_phase = (int32_t)in_phase,
		.quadrature = (int32_t)quadrature,
	};
	return true;
}

static bool quantize_filter_output(
	const struct v34_q7_point *filter,
	enum v34_precoder_grid_step grid_step,
	struct v34_grid_point *offset)
{
	int64_t divisor = (int64_t)grid_step * V34_Q7_SCALE;
	int64_t in_phase = round_divide_ties_toward_zero(
		filter->in_phase, divisor) * grid_step;
	int64_t quadrature = round_divide_ties_toward_zero(
		filter->quadrature, divisor) * grid_step;

	if (!int64_fits_int16(in_phase) || !int64_fits_int16(quadrature))
		return false;
	*offset = (struct v34_grid_point){
		.in_phase = (int16_t)in_phase,
		.quadrature = (int16_t)quadrature,
	};
	return true;
}

static bool generate_prediction(
	const struct v34_precoder *precoder,
	struct v34_precoder_prediction *prediction)
{
	return filter_output(precoder, &prediction->filter_output) &&
	       quantize_filter_output(
		       &prediction->filter_output, precoder->grid_step,
		       &prediction->lattice_offset);
}

enum v34_precoder_result v34_precoder_predict(
	const struct v34_precoder *precoder,
	struct v34_precoder_prediction *prediction)
{
	struct v34_precoder_prediction generated;

	if (!v34_precoder_is_valid(precoder) || prediction == NULL)
		return V34_PRECODER_INVALID_ARGUMENT;
	if (!generate_prediction(precoder, &generated))
		return V34_PRECODER_OUT_OF_RANGE;
	*prediction = generated;
	return V34_PRECODER_OK;
}

static bool generate_symbol(
	const struct v34_precoder *precoder,
	const struct v34_grid_point *mapper_output,
	struct v34_precoder_symbol *symbol)
{
	struct v34_precoder_symbol generated;
	struct v34_precoder_prediction prediction;
	int64_t channel_in_phase;
	int64_t channel_quadrature;
	int64_t precoded_in_phase;
	int64_t precoded_quadrature;

	if (!generate_prediction(precoder, &prediction))
		return false;
	generated.filter_output = prediction.filter_output;
	generated.lattice_offset = prediction.lattice_offset;
	channel_in_phase = (int64_t)mapper_output->in_phase +
		generated.lattice_offset.in_phase;
	channel_quadrature = (int64_t)mapper_output->quadrature +
		generated.lattice_offset.quadrature;
	if (channel_in_phase < -V34_CHANNEL_COORDINATE_LIMIT ||
	    channel_in_phase > V34_CHANNEL_COORDINATE_LIMIT ||
	    channel_quadrature < -V34_CHANNEL_COORDINATE_LIMIT ||
	    channel_quadrature > V34_CHANNEL_COORDINATE_LIMIT)
		return false;
	generated.channel_output = (struct v34_grid_point){
		.in_phase = (int16_t)channel_in_phase,
		.quadrature = (int16_t)channel_quadrature,
	};
	precoded_in_phase =
		channel_in_phase * V34_Q7_SCALE -
		generated.filter_output.in_phase;
	precoded_quadrature =
		channel_quadrature * V34_Q7_SCALE -
		generated.filter_output.quadrature;
	if (!int64_fits_int32(precoded_in_phase) ||
	    !int64_fits_int32(precoded_quadrature))
		return false;
	generated.precoded_symbol = (struct v34_q7_point){
		.in_phase = (int32_t)precoded_in_phase,
		.quadrature = (int32_t)precoded_quadrature,
	};
	*symbol = generated;
	return true;
}

static void push_previous_symbol(
	struct v34_precoder *precoder,
	const struct v34_q7_point *symbol)
{
	for (size_t tap = V34_PRECODER_TAP_COUNT - 1; tap > 0;
	     --tap)
		precoder->previous_symbols[tap] =
			precoder->previous_symbols[tap - 1];
	precoder->previous_symbols[0] = *symbol;
}

enum v34_precoder_result v34_precoder_step(
	struct v34_precoder *precoder,
	const struct v34_grid_point *mapper_output,
	struct v34_precoder_symbol *symbol)
{
	struct v34_precoder next;
	struct v34_precoder_symbol generated;

	if (!v34_precoder_is_valid(precoder) ||
	    !mapper_output_is_valid(mapper_output) || symbol == NULL)
		return V34_PRECODER_INVALID_ARGUMENT;
	if (!generate_symbol(precoder, mapper_output, &generated))
		return V34_PRECODER_OUT_OF_RANGE;
	next = *precoder;
	push_previous_symbol(&next, &generated.precoded_symbol);
	*precoder = next;
	*symbol = generated;
	return V34_PRECODER_OK;
}

static bool quantize_received_axis(double coordinate, int32_t *quantized)
{
	double integral;
	double fraction;
	double scaled;

	if (!isfinite(coordinate) || quantized == NULL)
		return false;
	scaled = coordinate * V34_Q7_SCALE;
	if (!isfinite(scaled) ||
	    scaled < (double)INT32_MIN - 0.5 ||
	    scaled > (double)INT32_MAX + 0.5)
		return false;
	fraction = modf(scaled, &integral);
	if (fraction > 0.5)
		integral += 1.0;
	else if (fraction < -0.5)
		integral -= 1.0;
	if (integral < INT32_MIN || integral > INT32_MAX)
		return false;
	*quantized = (int32_t)integral;
	return true;
}

static enum v34_precoder_result reconstruct_channel_point(
	struct v34_precoder *receive_history,
	const struct qam_point *precoded_estimate,
	struct qam_point *channel_estimate)
{
	struct v34_precoder_prediction prediction;
	struct v34_precoder_prediction next_prediction;
	struct v34_q7_point received;
	struct qam_point generated;
	enum v34_precoder_result result;

	result = v34_precoder_predict(
		receive_history, &prediction);
	if (result != V34_PRECODER_OK)
		return result;
	if (!quantize_received_axis(
		    precoded_estimate->in_phase, &received.in_phase) ||
	    !quantize_received_axis(
		    precoded_estimate->quadrature, &received.quadrature))
		return V34_PRECODER_OUT_OF_RANGE;
	generated = (struct qam_point){
		.in_phase = precoded_estimate->in_phase +
			(double)prediction.filter_output.in_phase /
				V34_Q7_SCALE,
		.quadrature = precoded_estimate->quadrature +
			(double)prediction.filter_output.quadrature /
				V34_Q7_SCALE,
	};
	if (!isfinite(generated.in_phase) ||
	    !isfinite(generated.quadrature))
		return V34_PRECODER_OUT_OF_RANGE;
	push_previous_symbol(receive_history, &received);
	result = v34_precoder_predict(
		receive_history, &next_prediction);
	if (result != V34_PRECODER_OK)
		return result;
	*channel_estimate = generated;
	return V34_PRECODER_OK;
}

enum v34_precoder_result
v34_precoder_reconstruct_interval(
	struct v34_precoder *receive_history,
	const struct qam_point
		precoded_estimates[V34_2D_SYMBOLS_PER_4D_INTERVAL],
	struct qam_point
		channel_estimates[V34_2D_SYMBOLS_PER_4D_INTERVAL])
{
	struct v34_precoder next;
	struct qam_point generated[
		V34_2D_SYMBOLS_PER_4D_INTERVAL];

	if (!v34_precoder_is_valid(receive_history) ||
	    precoded_estimates == NULL || channel_estimates == NULL)
		return V34_PRECODER_INVALID_ARGUMENT;
	for (size_t symbol = 0;
	     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol) {
		if (!isfinite(precoded_estimates[symbol].in_phase) ||
		    !isfinite(precoded_estimates[symbol].quadrature))
			return V34_PRECODER_INVALID_ARGUMENT;
	}
	next = *receive_history;
	for (size_t symbol = 0;
	     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol) {
		enum v34_precoder_result result =
			reconstruct_channel_point(
				&next, &precoded_estimates[symbol],
				&generated[symbol]);

		if (result != V34_PRECODER_OK)
			return result;
	}
	*receive_history = next;
	for (size_t symbol = 0;
	     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol)
		channel_estimates[symbol] = generated[symbol];
	return V34_PRECODER_OK;
}

static bool channel_output_is_valid(
	const struct v34_grid_point *channel_output)
{
	return channel_output != NULL &&
	       channel_output->in_phase >=
		       -V34_CHANNEL_COORDINATE_LIMIT &&
	       channel_output->in_phase <=
		       V34_CHANNEL_COORDINATE_LIMIT &&
	       channel_output->quadrature >=
		       -V34_CHANNEL_COORDINATE_LIMIT &&
	       channel_output->quadrature <=
		       V34_CHANNEL_COORDINATE_LIMIT &&
	       ((uint16_t)channel_output->in_phase & 1u) != 0u &&
	       ((uint16_t)channel_output->quadrature & 1u) != 0u;
}

enum v34_precoder_result v34_precoder_inverse_step(
	struct v34_precoder *precoder,
	const struct v34_grid_point *channel_output,
	struct v34_grid_point *mapper_output,
	struct v34_precoder_symbol *symbol)
{
	struct v34_precoder next;
	struct v34_precoder_prediction prediction;
	struct v34_precoder_symbol generated;
	struct v34_grid_point channel;
	struct v34_grid_point recovered;
	int64_t recovered_in_phase;
	int64_t recovered_quadrature;
	enum v34_precoder_result result;

	if (!v34_precoder_is_valid(precoder) ||
	    !channel_output_is_valid(channel_output) || mapper_output == NULL ||
	    symbol == NULL)
		return V34_PRECODER_INVALID_ARGUMENT;
	next = *precoder;
	channel = *channel_output;
	result = v34_precoder_predict(&next, &prediction);
	if (result != V34_PRECODER_OK)
		return result;
	recovered_in_phase =
		(int64_t)channel.in_phase - prediction.lattice_offset.in_phase;
	recovered_quadrature =
		(int64_t)channel.quadrature - prediction.lattice_offset.quadrature;
	if (!int64_fits_int16(recovered_in_phase) ||
	    !int64_fits_int16(recovered_quadrature))
		return V34_PRECODER_OUT_OF_RANGE;
	recovered = (struct v34_grid_point){
		.in_phase = (int16_t)recovered_in_phase,
		.quadrature = (int16_t)recovered_quadrature,
	};
	result = v34_precoder_step(&next, &recovered, &generated);
	if (result != V34_PRECODER_OK)
		return result;
	if (generated.channel_output.in_phase != channel.in_phase ||
	    generated.channel_output.quadrature != channel.quadrature)
		return V34_PRECODER_INVALID_ARGUMENT;
	*precoder = next;
	*mapper_output = recovered;
	*symbol = generated;
	return V34_PRECODER_OK;
}
