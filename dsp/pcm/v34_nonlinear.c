// SPDX-License-Identifier: BSD-3-Clause

#include "v34_nonlinear.h"

#include <math.h>
#include <stddef.h>

static const double V34_NONLINEAR_THETA = 5.0 / 16.0;

enum {
	V34_NONLINEAR_INVERSE_ITERATIONS = 64,
};

bool v34_nonlinear_parameter_is_valid(
	enum v34_nonlinear_parameter parameter)
{
	return parameter == V34_NONLINEAR_THETA_ZERO ||
	       parameter == V34_NONLINEAR_THETA_5_OVER_16;
}

bool v34_nonlinear_configuration_is_valid(
	enum v34_nonlinear_parameter parameter,
	double mean_squared_magnitude)
{
	return v34_nonlinear_parameter_is_valid(parameter) &&
	       isfinite(mean_squared_magnitude) && mean_squared_magnitude > 0.0;
}

static enum v34_nonlinear_result projection_factor(
	enum v34_nonlinear_parameter parameter,
	double mean_squared_magnitude, double squared_magnitude,
	double *factor)
{
	double generated = 1.0;

	if (!v34_nonlinear_configuration_is_valid(
		    parameter, mean_squared_magnitude) ||
	    !isfinite(squared_magnitude) || squared_magnitude < 0.0 ||
	    factor == NULL)
		return V34_NONLINEAR_INVALID_ARGUMENT;
	if (parameter == V34_NONLINEAR_THETA_5_OVER_16) {
		double zeta = V34_NONLINEAR_THETA * squared_magnitude /
			mean_squared_magnitude;

		generated = 1.0 + zeta / 6.0 + zeta * zeta / 120.0;
	}
	if (!isfinite(generated))
		return V34_NONLINEAR_NUMERIC_FAILURE;
	*factor = generated;
	return V34_NONLINEAR_OK;
}

static double projection_log_factor(double zeta)
{
	if (zeta < 1.0)
		return log1p(zeta / 6.0 + zeta * zeta / 120.0);
	return 2.0 * log(zeta) - log(120.0) +
	       log1p(20.0 / zeta + (120.0 / zeta) / zeta);
}

static double projected_log_zeta(double zeta)
{
	return log(zeta) + 2.0 * projection_log_factor(zeta);
}

static bool projected_zeta_is_below(
	double zeta, double projected_log_power, double projected_power)
{
	if (isfinite(projected_power) && projected_power > 0.0) {
		double factor =
			1.0 + zeta / 6.0 + zeta * zeta / 120.0;
		double mapped = zeta * factor * factor;

		return isfinite(mapped) && mapped < projected_power;
	}
	return projected_log_zeta(zeta) < projected_log_power;
}

/* If t is the projected dimensionless power, the unprojected zeta satisfies
 * t = zeta * (1 + zeta/6 + zeta^2/120)^2. The right side is strictly
 * increasing. Its inverse is bounded above both by t and by
 * (14400*t)^(1/5), which keeps a fixed bisection well-conditioned without a
 * coefficient table even near the limits of binary64. */
static enum v34_nonlinear_result inverse_zeta(
	double projected_log_power, double *zeta)
{
	double upper_log;
	double lower = 0.0;
	double upper;
	double projected_power;

	if (!isfinite(projected_log_power) || zeta == NULL)
		return V34_NONLINEAR_NUMERIC_FAILURE;
	projected_power = exp(projected_log_power);
	upper_log = fmin(
		projected_log_power,
		(projected_log_power + log(14400.0)) / 5.0);
	upper = exp(upper_log);
	if (upper == 0.0) {
		*zeta = 0.0;
		return V34_NONLINEAR_OK;
	}
	if (!isfinite(upper))
		return V34_NONLINEAR_NUMERIC_FAILURE;
	upper = nextafter(upper, INFINITY);
	for (unsigned int attempt = 0;
	     attempt < 4u &&
	     projected_zeta_is_below(
		     upper, projected_log_power, projected_power);
	     ++attempt)
		upper *= 2.0;
	if (!isfinite(upper) ||
	    projected_zeta_is_below(
		    upper, projected_log_power, projected_power))
		return V34_NONLINEAR_NUMERIC_FAILURE;
	for (unsigned int iteration = 0;
	     iteration < V34_NONLINEAR_INVERSE_ITERATIONS; ++iteration) {
		double midpoint = lower + (upper - lower) / 2.0;

		if (midpoint == lower || midpoint == upper)
			break;
		if (projected_zeta_is_below(
			    midpoint, projected_log_power, projected_power))
			lower = midpoint;
		else
			upper = midpoint;
	}
	*zeta = lower + (upper - lower) / 2.0;
	return V34_NONLINEAR_OK;
}

enum v34_nonlinear_result
v34_nonlinear_project_power(
	enum v34_nonlinear_parameter parameter,
	double mean_squared_magnitude, double squared_magnitude,
	double *projected_squared_magnitude)
{
	double factor;
	double generated;
	enum v34_nonlinear_result result;

	if (projected_squared_magnitude == NULL)
		return V34_NONLINEAR_INVALID_ARGUMENT;
	result = projection_factor(
		parameter, mean_squared_magnitude, squared_magnitude, &factor);
	if (result != V34_NONLINEAR_OK)
		return result;
	generated = squared_magnitude * factor * factor;
	if (!isfinite(generated))
		return V34_NONLINEAR_NUMERIC_FAILURE;
	*projected_squared_magnitude = generated;
	return V34_NONLINEAR_OK;
}

enum v34_nonlinear_result
v34_nonlinear_encode(
	enum v34_nonlinear_parameter parameter,
	double mean_squared_magnitude,
	const struct v34_q7_point *precoded_symbol,
	struct qam_point *projected_symbol)
{
	struct qam_point generated;
	double in_phase;
	double quadrature;
	double squared_magnitude;
	double projection;
	enum v34_nonlinear_result result;

	if (precoded_symbol == NULL || projected_symbol == NULL)
		return V34_NONLINEAR_INVALID_ARGUMENT;
	in_phase = (double)precoded_symbol->in_phase /
		V34_Q7_SCALE;
	quadrature = (double)precoded_symbol->quadrature /
		V34_Q7_SCALE;
	squared_magnitude =
		in_phase * in_phase + quadrature * quadrature;
	result = projection_factor(
		parameter, mean_squared_magnitude, squared_magnitude,
		&projection);
	if (result != V34_NONLINEAR_OK)
		return result;
	generated = (struct qam_point){
		.in_phase = in_phase * projection,
		.quadrature = quadrature * projection,
	};
	if (!isfinite(generated.in_phase) ||
	    !isfinite(generated.quadrature))
		return V34_NONLINEAR_NUMERIC_FAILURE;
	*projected_symbol = generated;
	return V34_NONLINEAR_OK;
}

enum v34_nonlinear_result
v34_nonlinear_unproject(
	enum v34_nonlinear_parameter parameter,
	double mean_squared_magnitude,
	const struct qam_point *projected_symbol,
	struct qam_point *precoded_estimate)
{
	struct qam_point generated;
	double maximum_axis;
	double minimum_axis;
	double axis_ratio;
	double projected_log_power;
	double zeta;
	double factor;

	if (!v34_nonlinear_configuration_is_valid(
		    parameter, mean_squared_magnitude) ||
	    projected_symbol == NULL || precoded_estimate == NULL ||
	    !isfinite(projected_symbol->in_phase) ||
	    !isfinite(projected_symbol->quadrature))
		return V34_NONLINEAR_INVALID_ARGUMENT;
	generated = *projected_symbol;
	if (parameter == V34_NONLINEAR_THETA_ZERO) {
		*precoded_estimate = generated;
		return V34_NONLINEAR_OK;
	}
	maximum_axis = fmax(
		fabs(projected_symbol->in_phase),
		fabs(projected_symbol->quadrature));
	if (maximum_axis == 0.0) {
		*precoded_estimate = generated;
		return V34_NONLINEAR_OK;
	}
	minimum_axis = fmin(
		fabs(projected_symbol->in_phase),
		fabs(projected_symbol->quadrature));
	axis_ratio = minimum_axis / maximum_axis;
	projected_log_power =
		log(V34_NONLINEAR_THETA) + 2.0 * log(maximum_axis) +
		log1p(axis_ratio * axis_ratio) - log(mean_squared_magnitude);
	if (inverse_zeta(projected_log_power, &zeta) !=
	    V34_NONLINEAR_OK)
		return V34_NONLINEAR_NUMERIC_FAILURE;
	factor = 1.0 + zeta / 6.0 + zeta * zeta / 120.0;
	if (isfinite(factor)) {
		generated.in_phase /= factor;
		generated.quadrature /= factor;
	} else {
		double tail = 1.0 + 20.0 / zeta +
			(120.0 / zeta) / zeta;
		double scale = 120.0 / tail;

		generated.in_phase =
			((generated.in_phase / zeta) / zeta) * scale;
		generated.quadrature =
			((generated.quadrature / zeta) / zeta) * scale;
	}
	if (!isfinite(generated.in_phase) ||
	    !isfinite(generated.quadrature))
		return V34_NONLINEAR_NUMERIC_FAILURE;
	*precoded_estimate = generated;
	return V34_NONLINEAR_OK;
}
