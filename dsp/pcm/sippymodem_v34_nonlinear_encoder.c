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
	double projection = 1.0;

	if (!sippymodem_v34_nonlinear_parameter_is_valid(parameter) ||
	    !isfinite(mean_squared_magnitude) ||
	    mean_squared_magnitude <= 0.0 || precoded_symbol == NULL ||
	    projected_symbol == NULL)
		return SIPPYMODEM_V34_NONLINEAR_ENCODER_INVALID_ARGUMENT;
	in_phase = (double)precoded_symbol->in_phase /
		SIPPYMODEM_V34_Q7_SCALE;
	quadrature = (double)precoded_symbol->quadrature /
		SIPPYMODEM_V34_Q7_SCALE;
	if (parameter == SIPPYMODEM_V34_NONLINEAR_THETA_5_OVER_16) {
		double squared_magnitude =
			in_phase * in_phase + quadrature * quadrature;
		double zeta = V34_NONLINEAR_THETA * squared_magnitude /
			mean_squared_magnitude;

		projection = 1.0 + zeta / 6.0 + zeta * zeta / 120.0;
	}
	generated = (struct sippymodem_qam_point){
		.in_phase = in_phase * projection,
		.quadrature = quadrature * projection,
	};
	if (!isfinite(projection) || !isfinite(generated.in_phase) ||
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
