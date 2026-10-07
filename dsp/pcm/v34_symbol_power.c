// SPDX-License-Identifier: BSD-3-Clause

#include "v34_symbol_power.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

static const double V34_POWER_COMPARISON_EPSILON = 1e-12;

bool v34_symbol_power_measurement_is_valid(
	const struct v34_symbol_power_measurement *measurement)
{
	return measurement != NULL &&
	       isfinite(measurement->mean_squared_magnitude) &&
	       measurement->mean_squared_magnitude >= 0.0 &&
	       (measurement->symbol_count != 0 ||
		measurement->mean_squared_magnitude == 0.0);
}

static enum v34_symbol_power_result observe_power(
	struct v34_symbol_power_measurement *measurement,
	double squared_magnitude)
{
	struct v34_symbol_power_measurement next;

	if (!v34_symbol_power_measurement_is_valid(measurement) ||
	    !isfinite(squared_magnitude) || squared_magnitude < 0.0)
		return V34_SYMBOL_POWER_INVALID_ARGUMENT;
	if (measurement->symbol_count == UINT32_MAX)
		return V34_SYMBOL_POWER_OUT_OF_RANGE;
	next = *measurement;
	next.symbol_count++;
	next.mean_squared_magnitude +=
		(squared_magnitude - next.mean_squared_magnitude) /
		next.symbol_count;
	if (!isfinite(next.mean_squared_magnitude) ||
	    next.mean_squared_magnitude < 0.0)
		return V34_SYMBOL_POWER_NUMERIC_FAILURE;
	*measurement = next;
	return V34_SYMBOL_POWER_OK;
}

enum v34_symbol_power_result
v34_symbol_power_observe_q7(
	struct v34_symbol_power_measurement *measurement,
	const struct v34_q7_point *point)
{
	struct qam_point converted;

	if (point == NULL)
		return V34_SYMBOL_POWER_INVALID_ARGUMENT;
	converted = (struct qam_point){
		.in_phase =
			(double)point->in_phase / V34_Q7_SCALE,
		.quadrature =
			(double)point->quadrature / V34_Q7_SCALE,
	};
	return v34_symbol_power_observe_point(
		measurement, &converted);
}

enum v34_symbol_power_result
v34_symbol_power_observe_point(
	struct v34_symbol_power_measurement *measurement,
	const struct qam_point *point)
{
	double squared_magnitude;

	if (!point_is_finite(point))
		return V34_SYMBOL_POWER_INVALID_ARGUMENT;
	squared_magnitude = point->in_phase * point->in_phase +
		point->quadrature * point->quadrature;
	if (!isfinite(squared_magnitude))
		return V34_SYMBOL_POWER_NUMERIC_FAILURE;
	return observe_power(measurement, squared_magnitude);
}

static bool nearly_equal(double left, double right)
{
	double scale = fmax(1.0, fmax(fabs(left), fabs(right)));

	return fabs(left - right) <= V34_POWER_COMPARISON_EPSILON * scale;
}

static bool power_does_not_exceed(double power, double maximum)
{
	return power <= maximum ||
	       power / maximum <= 1.0 + V34_POWER_COMPARISON_EPSILON;
}

static bool measured_power_relation_is_valid(
	enum v34_nonlinear_parameter nonlinear_parameter,
	double precoded_mean, double projected_mean)
{
	if (nonlinear_parameter == V34_NONLINEAR_THETA_ZERO)
		return nearly_equal(precoded_mean, projected_mean);
	return projected_mean >= precoded_mean ||
	       nearly_equal(precoded_mean, projected_mean);
}

static bool maximum_precoded_point_power(
	const struct v34_mapping_parameters *parameters,
	double *maximum)
{
	int64_t maximum_squared_q7 = 0;
	int64_t half_grid_step_q7 =
		(int64_t)v34_precoder_grid_step_for_mapping(parameters) *
		V34_Q7_SCALE / 2;
	size_t quarter_point_count = parameters->constellation_points / 4u;

	/* c(n) is the nearest grid multiple to p(n), so each component of
	 * c(n) - p(n) is bounded by half a grid step. Enumerating the mapper
	 * labels and adding that residual outward gives a data-independent
	 * envelope for x(n). */
	for (size_t label = 0; label < quarter_point_count; ++label) {
		struct v34_grid_point point;
		int64_t in_phase;
		int64_t quadrature;
		int64_t squared_q7;

		if (v34_quarter_superconstellation_point(
			    label, &point) != V34_CONSTELLATION_OK)
			return false;
		in_phase =
			(point.in_phase < 0 ? -(int64_t)point.in_phase :
					      point.in_phase) *
				V34_Q7_SCALE +
			half_grid_step_q7;
		quadrature =
			(point.quadrature < 0 ? -(int64_t)point.quadrature :
						 point.quadrature) *
				V34_Q7_SCALE +
			half_grid_step_q7;
		squared_q7 = in_phase * in_phase + quadrature * quadrature;
		if (squared_q7 > maximum_squared_q7)
			maximum_squared_q7 = squared_q7;
	}
	*maximum = (double)maximum_squared_q7 /
		((double)V34_Q7_SCALE * V34_Q7_SCALE);
	return isfinite(*maximum) && *maximum > 0.0;
}

static enum v34_symbol_power_result derive_normalizer(
	struct v34_power_normalizer *normalizer,
	const struct v34_mapping_parameters *parameters,
	enum v34_nonlinear_parameter nonlinear_parameter,
	const struct v34_symbol_power_measurement
		*precoded_measurement,
	const struct v34_symbol_power_measurement
		*projected_measurement)
{
	struct v34_power_normalizer generated = { 0 };
	enum v34_nonlinear_result nonlinear_result;

	if (normalizer == NULL ||
	    !v34_mapping_parameters_is_valid(parameters) ||
	    !v34_symbol_power_measurement_is_valid(
		    precoded_measurement) ||
	    !v34_symbol_power_measurement_is_valid(
		    projected_measurement) ||
	    precoded_measurement->symbol_count == 0 ||
	    precoded_measurement->symbol_count !=
		    projected_measurement->symbol_count ||
	    !v34_nonlinear_configuration_is_valid(
		    nonlinear_parameter,
		    precoded_measurement->mean_squared_magnitude) ||
	    projected_measurement->mean_squared_magnitude <= 0.0 ||
	    !maximum_precoded_point_power(
		    parameters, &generated.maximum_precoded_point_power) ||
	    !power_does_not_exceed(
		    precoded_measurement->mean_squared_magnitude,
		    generated.maximum_precoded_point_power))
		return V34_SYMBOL_POWER_INVALID_ARGUMENT;
	nonlinear_result = v34_nonlinear_project_power(
		nonlinear_parameter,
		precoded_measurement->mean_squared_magnitude,
		generated.maximum_precoded_point_power,
		&generated.maximum_projected_point_power);
	if (nonlinear_result ==
	    V34_NONLINEAR_NUMERIC_FAILURE)
		return V34_SYMBOL_POWER_NUMERIC_FAILURE;
	if (nonlinear_result != V34_NONLINEAR_OK)
		return V34_SYMBOL_POWER_INVALID_ARGUMENT;
	if (!power_does_not_exceed(
		    projected_measurement->mean_squared_magnitude,
		    generated.maximum_projected_point_power) ||
	    !measured_power_relation_is_valid(
		    nonlinear_parameter,
		    precoded_measurement->mean_squared_magnitude,
		    projected_measurement->mean_squared_magnitude))
		return V34_SYMBOL_POWER_INVALID_ARGUMENT;
	generated.parameters = *parameters;
	generated.precoded_mean_squared_magnitude =
		precoded_measurement->mean_squared_magnitude;
	generated.projected_mean_squared_magnitude =
		projected_measurement->mean_squared_magnitude;
	generated.normalization_gain =
		1.0 / sqrt(projected_measurement->mean_squared_magnitude);
	generated.maximum_normalized_point_power =
		generated.maximum_projected_point_power *
		generated.normalization_gain * generated.normalization_gain;
	generated.calibration_symbol_count = precoded_measurement->symbol_count;
	generated.nonlinear_parameter = nonlinear_parameter;
	if (!isfinite(generated.normalization_gain) ||
	    generated.normalization_gain <= 0.0 ||
	    !isfinite(generated.maximum_normalized_point_power) ||
	    generated.maximum_normalized_point_power <= 0.0)
		return V34_SYMBOL_POWER_NUMERIC_FAILURE;
	*normalizer = generated;
	return V34_SYMBOL_POWER_OK;
}

enum v34_symbol_power_result
v34_power_normalizer_init(
	struct v34_power_normalizer *normalizer,
	const struct v34_mapping_parameters *parameters,
	enum v34_nonlinear_parameter nonlinear_parameter,
	const struct v34_symbol_power_measurement
		*precoded_measurement,
	const struct v34_symbol_power_measurement
		*projected_measurement)
{
	struct v34_power_normalizer generated;
	enum v34_symbol_power_result result;

	if (normalizer == NULL)
		return V34_SYMBOL_POWER_INVALID_ARGUMENT;
	result = derive_normalizer(
		&generated, parameters, nonlinear_parameter,
		precoded_measurement, projected_measurement);
	if (result == V34_SYMBOL_POWER_OK)
		*normalizer = generated;
	return result;
}

enum v34_symbol_power_result v34_power_normalizer_set_gain(
	struct v34_power_normalizer *normalizer, double normalization_gain)
{
	struct v34_power_normalizer updated;

	if (!v34_power_normalizer_is_valid(normalizer) ||
	    !isfinite(normalization_gain) || normalization_gain <= 0.0)
		return V34_SYMBOL_POWER_INVALID_ARGUMENT;
	updated = *normalizer;
	updated.normalization_gain = normalization_gain;
	updated.maximum_normalized_point_power =
		updated.maximum_projected_point_power * normalization_gain *
		normalization_gain;
	if (!isfinite(updated.maximum_normalized_point_power) ||
	    updated.maximum_normalized_point_power <= 0.0)
		return V34_SYMBOL_POWER_NUMERIC_FAILURE;
	*normalizer = updated;
	return V34_SYMBOL_POWER_OK;
}

bool v34_power_normalizer_is_valid(
	const struct v34_power_normalizer *normalizer)
{
	double expected_maximum_projected;
	double expected_gain;
	double expected_maximum_normalized;

	if (normalizer == NULL ||
	    !v34_mapping_parameters_is_valid(
		    &normalizer->parameters) ||
	    normalizer->calibration_symbol_count == 0 ||
	    !v34_nonlinear_configuration_is_valid(
		    normalizer->nonlinear_parameter,
		    normalizer->precoded_mean_squared_magnitude) ||
	    !isfinite(normalizer->projected_mean_squared_magnitude) ||
	    normalizer->projected_mean_squared_magnitude <= 0.0 ||
	    !isfinite(normalizer->maximum_precoded_point_power) ||
	    normalizer->maximum_precoded_point_power <= 0.0 ||
	    !power_does_not_exceed(
		    normalizer->precoded_mean_squared_magnitude,
		    normalizer->maximum_precoded_point_power) ||
	    !measured_power_relation_is_valid(
		    normalizer->nonlinear_parameter,
		    normalizer->precoded_mean_squared_magnitude,
		    normalizer->projected_mean_squared_magnitude) ||
	    v34_nonlinear_project_power(
		    normalizer->nonlinear_parameter,
		    normalizer->precoded_mean_squared_magnitude,
		    normalizer->maximum_precoded_point_power,
		    &expected_maximum_projected) !=
		    V34_NONLINEAR_OK)
		return false;
	expected_gain = normalizer->normalization_gain;
	expected_maximum_normalized = expected_maximum_projected *
		expected_gain * expected_gain;
	return normalizer->maximum_projected_point_power ==
		       expected_maximum_projected &&
	       power_does_not_exceed(
		       normalizer->projected_mean_squared_magnitude,
		       normalizer->maximum_projected_point_power) &&
	       isfinite(expected_gain) && expected_gain > 0.0 &&
	       normalizer->maximum_normalized_point_power ==
		       expected_maximum_normalized &&
	       isfinite(normalizer->maximum_normalized_point_power) &&
	       normalizer->maximum_normalized_point_power > 0.0;
}

enum v34_symbol_power_result v34_normalize_power(
	const struct v34_power_normalizer *normalizer,
	const struct qam_point *projected_point,
	struct qam_point *normalized_point)
{
	struct qam_point generated;
	double projected_power;
	double normalized_power;

	if (!v34_power_normalizer_is_valid(normalizer) ||
	    !point_is_finite(projected_point) || normalized_point == NULL)
		return V34_SYMBOL_POWER_INVALID_ARGUMENT;
	projected_power =
		projected_point->in_phase * projected_point->in_phase +
		projected_point->quadrature * projected_point->quadrature;
	if (!isfinite(projected_power))
		return V34_SYMBOL_POWER_NUMERIC_FAILURE;
	if (!power_does_not_exceed(
		    projected_power, normalizer->maximum_projected_point_power))
		return V34_SYMBOL_POWER_OUT_OF_RANGE;
	generated = (struct qam_point){
		.in_phase =
			projected_point->in_phase * normalizer->normalization_gain,
		.quadrature =
			projected_point->quadrature * normalizer->normalization_gain,
	};
	normalized_power = generated.in_phase * generated.in_phase +
		generated.quadrature * generated.quadrature;
	if (!isfinite(normalized_power))
		return V34_SYMBOL_POWER_NUMERIC_FAILURE;
	if (!power_does_not_exceed(
		    normalized_power,
		    normalizer->maximum_normalized_point_power))
		return V34_SYMBOL_POWER_NUMERIC_FAILURE;
	*normalized_point = generated;
	return V34_SYMBOL_POWER_OK;
}
