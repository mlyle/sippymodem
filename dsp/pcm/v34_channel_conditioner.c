// SPDX-License-Identifier: BSD-3-Clause

#include "v34_channel_conditioner.h"

#include <math.h>
#include <stddef.h>

bool sippymodem_v34_channel_conditioner_is_valid(
	const struct sippymodem_v34_channel_conditioner *conditioner)
{
	return conditioner != NULL &&
	       sippymodem_v34_power_normalizer_is_valid(
		       &conditioner->normalizer);
}

enum sippymodem_v34_channel_conditioner_result
sippymodem_v34_channel_conditioner_init(
	struct sippymodem_v34_channel_conditioner *conditioner,
	const struct sippymodem_v34_power_normalizer *normalizer)
{
	struct sippymodem_v34_channel_conditioner initialized;

	if (conditioner == NULL ||
	    !sippymodem_v34_power_normalizer_is_valid(normalizer))
		return SIPPYMODEM_V34_CHANNEL_CONDITIONER_INVALID_ARGUMENT;
	initialized.normalizer = *normalizer;
	if (!sippymodem_v34_channel_conditioner_is_valid(&initialized))
		return SIPPYMODEM_V34_CHANNEL_CONDITIONER_INVALID_ARGUMENT;
	*conditioner = initialized;
	return SIPPYMODEM_V34_CHANNEL_CONDITIONER_OK;
}

static enum sippymodem_v34_channel_conditioner_result nonlinear_result(
	enum sippymodem_v34_nonlinear_result result)
{
	return result == SIPPYMODEM_V34_NONLINEAR_NUMERIC_FAILURE ?
		SIPPYMODEM_V34_CHANNEL_CONDITIONER_NUMERIC_FAILURE :
		SIPPYMODEM_V34_CHANNEL_CONDITIONER_INVALID_ARGUMENT;
}

static enum sippymodem_v34_channel_conditioner_result condition_finite_point(
	const struct sippymodem_v34_channel_conditioner *conditioner,
	const struct sippymodem_qam_point *normalized_point,
	struct sippymodem_qam_point *grid_estimate)
{
	struct sippymodem_qam_point projected;
	struct sippymodem_qam_point generated;
	enum sippymodem_v34_nonlinear_result result;

	projected = (struct sippymodem_qam_point){
		.in_phase = normalized_point->in_phase /
			conditioner->normalizer.normalization_gain,
		.quadrature = normalized_point->quadrature /
			conditioner->normalizer.normalization_gain,
	};
	if (!isfinite(projected.in_phase) ||
	    !isfinite(projected.quadrature))
		return SIPPYMODEM_V34_CHANNEL_CONDITIONER_NUMERIC_FAILURE;
	result = sippymodem_v34_nonlinear_unproject(
		conditioner->normalizer.nonlinear_parameter,
		conditioner->normalizer.precoded_mean_squared_magnitude,
		&projected, &generated);
	if (result != SIPPYMODEM_V34_NONLINEAR_OK)
		return nonlinear_result(result);
	*grid_estimate = generated;
	return SIPPYMODEM_V34_CHANNEL_CONDITIONER_OK;
}

enum sippymodem_v34_channel_conditioner_result
sippymodem_v34_channel_condition_point(
	const struct sippymodem_v34_channel_conditioner *conditioner,
	const struct sippymodem_qam_point *normalized_point,
	struct sippymodem_qam_point *grid_estimate)
{
	if (!sippymodem_v34_channel_conditioner_is_valid(conditioner) ||
	    normalized_point == NULL || grid_estimate == NULL ||
	    !isfinite(normalized_point->in_phase) ||
	    !isfinite(normalized_point->quadrature))
		return SIPPYMODEM_V34_CHANNEL_CONDITIONER_INVALID_ARGUMENT;
	return condition_finite_point(
		conditioner, normalized_point, grid_estimate);
}

enum sippymodem_v34_channel_conditioner_result
sippymodem_v34_channel_condition_interval(
	const struct sippymodem_v34_channel_conditioner *conditioner,
	const struct sippymodem_qam_point
		normalized[SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL],
	struct sippymodem_qam_point
		grid_estimates[SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL])
{
	struct sippymodem_qam_point
		generated[SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL];

	if (!sippymodem_v34_channel_conditioner_is_valid(conditioner) ||
	    normalized == NULL || grid_estimates == NULL)
		return SIPPYMODEM_V34_CHANNEL_CONDITIONER_INVALID_ARGUMENT;
	for (size_t symbol = 0;
	     symbol < SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol) {
		enum sippymodem_v34_channel_conditioner_result result;

		if (!isfinite(normalized[symbol].in_phase) ||
		    !isfinite(normalized[symbol].quadrature))
			return SIPPYMODEM_V34_CHANNEL_CONDITIONER_INVALID_ARGUMENT;
		result = condition_finite_point(
			conditioner, &normalized[symbol], &generated[symbol]);

		if (result != SIPPYMODEM_V34_CHANNEL_CONDITIONER_OK)
			return result;
	}
	for (size_t symbol = 0;
	     symbol < SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL; ++symbol)
		grid_estimates[symbol] = generated[symbol];
	return SIPPYMODEM_V34_CHANNEL_CONDITIONER_OK;
}

const char *sippymodem_v34_channel_conditioner_result_name(
	enum sippymodem_v34_channel_conditioner_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_CHANNEL_CONDITIONER_OK:
		return "ok";
	case SIPPYMODEM_V34_CHANNEL_CONDITIONER_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_V34_CHANNEL_CONDITIONER_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
