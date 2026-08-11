// SPDX-License-Identifier: BSD-3-Clause

#include "v34_channel_conditioner.h"

#include <math.h>
#include <stddef.h>

bool v34_channel_conditioner_is_valid(
	const struct v34_channel_conditioner *conditioner)
{
	return conditioner != NULL &&
	       v34_power_normalizer_is_valid(
		       &conditioner->normalizer);
}

enum v34_channel_conditioner_result
v34_channel_conditioner_init(
	struct v34_channel_conditioner *conditioner,
	const struct v34_power_normalizer *normalizer)
{
	struct v34_channel_conditioner initialized;

	if (conditioner == NULL ||
	    !v34_power_normalizer_is_valid(normalizer))
		return V34_CHANNEL_CONDITIONER_INVALID_ARGUMENT;
	initialized.normalizer = *normalizer;
	if (!v34_channel_conditioner_is_valid(&initialized))
		return V34_CHANNEL_CONDITIONER_INVALID_ARGUMENT;
	*conditioner = initialized;
	return V34_CHANNEL_CONDITIONER_OK;
}

static enum v34_channel_conditioner_result nonlinear_result(
	enum v34_nonlinear_result result)
{
	return result == V34_NONLINEAR_NUMERIC_FAILURE ?
		V34_CHANNEL_CONDITIONER_NUMERIC_FAILURE :
		V34_CHANNEL_CONDITIONER_INVALID_ARGUMENT;
}

static enum v34_channel_conditioner_result condition_finite_point(
	const struct v34_channel_conditioner *conditioner,
	const struct qam_point *normalized_point,
	struct qam_point *grid_estimate)
{
	struct qam_point projected;
	struct qam_point generated;
	enum v34_nonlinear_result result;

	projected = (struct qam_point){
		.in_phase = normalized_point->in_phase /
			conditioner->normalizer.normalization_gain,
		.quadrature = normalized_point->quadrature /
			conditioner->normalizer.normalization_gain,
	};
	if (!isfinite(projected.in_phase) ||
	    !isfinite(projected.quadrature))
		return V34_CHANNEL_CONDITIONER_NUMERIC_FAILURE;
	result = v34_nonlinear_unproject(
		conditioner->normalizer.nonlinear_parameter,
		conditioner->normalizer.precoded_mean_squared_magnitude,
		&projected, &generated);
	if (result != V34_NONLINEAR_OK)
		return nonlinear_result(result);
	*grid_estimate = generated;
	return V34_CHANNEL_CONDITIONER_OK;
}

enum v34_channel_conditioner_result
v34_channel_condition_point(
	const struct v34_channel_conditioner *conditioner,
	const struct qam_point *normalized_point,
	struct qam_point *grid_estimate)
{
	if (!v34_channel_conditioner_is_valid(conditioner) ||
	    normalized_point == NULL || grid_estimate == NULL ||
	    !isfinite(normalized_point->in_phase) ||
	    !isfinite(normalized_point->quadrature))
		return V34_CHANNEL_CONDITIONER_INVALID_ARGUMENT;
	return condition_finite_point(
		conditioner, normalized_point, grid_estimate);
}

enum v34_channel_conditioner_result
v34_channel_condition_interval(
	const struct v34_channel_conditioner *conditioner,
	const struct qam_point
		normalized[V34_2D_SYMBOLS_PER_4D_INTERVAL],
	struct qam_point
		grid_estimates[V34_2D_SYMBOLS_PER_4D_INTERVAL])
{
	struct qam_point
		generated[V34_2D_SYMBOLS_PER_4D_INTERVAL];

	if (!v34_channel_conditioner_is_valid(conditioner) ||
	    normalized == NULL || grid_estimates == NULL)
		return V34_CHANNEL_CONDITIONER_INVALID_ARGUMENT;
	for (size_t symbol = 0;
	     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol) {
		enum v34_channel_conditioner_result result;

		if (!isfinite(normalized[symbol].in_phase) ||
		    !isfinite(normalized[symbol].quadrature))
			return V34_CHANNEL_CONDITIONER_INVALID_ARGUMENT;
		result = condition_finite_point(
			conditioner, &normalized[symbol], &generated[symbol]);

		if (result != V34_CHANNEL_CONDITIONER_OK)
			return result;
	}
	for (size_t symbol = 0;
	     symbol < V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol)
		grid_estimates[symbol] = generated[symbol];
	return V34_CHANNEL_CONDITIONER_OK;
}

const char *v34_channel_conditioner_result_name(
	enum v34_channel_conditioner_result result)
{
	switch (result) {
	case V34_CHANNEL_CONDITIONER_OK:
		return "ok";
	case V34_CHANNEL_CONDITIONER_INVALID_ARGUMENT:
		return "invalid argument";
	case V34_CHANNEL_CONDITIONER_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
