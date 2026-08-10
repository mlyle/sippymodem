// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v34_nonlinear_encoder.h"

#include <math.h>
#include <stddef.h>

static const double V34_NONLINEAR_THETA = 5.0 / 16.0;

bool sippymodem_v34_nonlinear_parameter_is_valid(
	enum sippymodem_v34_nonlinear_parameter parameter)
{
	return parameter == SIPPYMODEM_V34_NONLINEAR_THETA_ZERO ||
	       parameter == SIPPYMODEM_V34_NONLINEAR_THETA_5_OVER_16;
}

bool sippymodem_v34_nonlinear_configuration_is_valid(
	enum sippymodem_v34_nonlinear_parameter parameter,
	double mean_squared_magnitude)
{
	return sippymodem_v34_nonlinear_parameter_is_valid(parameter) &&
	       isfinite(mean_squared_magnitude) && mean_squared_magnitude > 0.0;
}

static enum sippymodem_v34_nonlinear_encoder_result projection_factor(
	enum sippymodem_v34_nonlinear_parameter parameter,
	double mean_squared_magnitude, double squared_magnitude,
	double *factor)
{
	double generated = 1.0;

	if (!sippymodem_v34_nonlinear_configuration_is_valid(
		    parameter, mean_squared_magnitude) ||
	    !isfinite(squared_magnitude) || squared_magnitude < 0.0 ||
	    factor == NULL)
		return SIPPYMODEM_V34_NONLINEAR_ENCODER_INVALID_ARGUMENT;
	if (parameter == SIPPYMODEM_V34_NONLINEAR_THETA_5_OVER_16) {
		double zeta = V34_NONLINEAR_THETA * squared_magnitude /
			mean_squared_magnitude;

		generated = 1.0 + zeta / 6.0 + zeta * zeta / 120.0;
	}
	if (!isfinite(generated))
		return SIPPYMODEM_V34_NONLINEAR_ENCODER_NUMERIC_FAILURE;
	*factor = generated;
	return SIPPYMODEM_V34_NONLINEAR_ENCODER_OK;
}

enum sippymodem_v34_nonlinear_encoder_result
sippymodem_v34_nonlinear_project_power(
	enum sippymodem_v34_nonlinear_parameter parameter,
	double mean_squared_magnitude, double squared_magnitude,
	double *projected_squared_magnitude)
{
	double factor;
	double generated;
	enum sippymodem_v34_nonlinear_encoder_result result;

	if (projected_squared_magnitude == NULL)
		return SIPPYMODEM_V34_NONLINEAR_ENCODER_INVALID_ARGUMENT;
	result = projection_factor(
		parameter, mean_squared_magnitude, squared_magnitude, &factor);
	if (result != SIPPYMODEM_V34_NONLINEAR_ENCODER_OK)
		return result;
	generated = squared_magnitude * factor * factor;
	if (!isfinite(generated))
		return SIPPYMODEM_V34_NONLINEAR_ENCODER_NUMERIC_FAILURE;
	*projected_squared_magnitude = generated;
	return SIPPYMODEM_V34_NONLINEAR_ENCODER_OK;
}

enum sippymodem_v34_nonlinear_encoder_result
sippymodem_v34_nonlinear_encode(
	enum sippymodem_v34_nonlinear_parameter parameter,
	double mean_squared_magnitude,
	const struct sippymodem_v34_q7_point *precoded_symbol,
	struct sippymodem_qam_point *projected_symbol)
{
	struct sippymodem_qam_point generated;
	double in_phase;
	double quadrature;
	double squared_magnitude;
	double projection;
	enum sippymodem_v34_nonlinear_encoder_result result;

	if (precoded_symbol == NULL || projected_symbol == NULL)
		return SIPPYMODEM_V34_NONLINEAR_ENCODER_INVALID_ARGUMENT;
	in_phase = (double)precoded_symbol->in_phase /
		SIPPYMODEM_V34_Q7_SCALE;
	quadrature = (double)precoded_symbol->quadrature /
		SIPPYMODEM_V34_Q7_SCALE;
	squared_magnitude =
		in_phase * in_phase + quadrature * quadrature;
	result = projection_factor(
		parameter, mean_squared_magnitude, squared_magnitude,
		&projection);
	if (result != SIPPYMODEM_V34_NONLINEAR_ENCODER_OK)
		return result;
	generated = (struct sippymodem_qam_point){
		.in_phase = in_phase * projection,
		.quadrature = quadrature * projection,
	};
	if (!isfinite(generated.in_phase) ||
	    !isfinite(generated.quadrature))
		return SIPPYMODEM_V34_NONLINEAR_ENCODER_NUMERIC_FAILURE;
	*projected_symbol = generated;
	return SIPPYMODEM_V34_NONLINEAR_ENCODER_OK;
}

const char *sippymodem_v34_nonlinear_encoder_result_name(
	enum sippymodem_v34_nonlinear_encoder_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_NONLINEAR_ENCODER_OK:
		return "ok";
	case SIPPYMODEM_V34_NONLINEAR_ENCODER_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_V34_NONLINEAR_ENCODER_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
