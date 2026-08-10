// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v34_symbol_power.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

bool sippymodem_v34_symbol_power_measurement_is_valid(
	const struct sippymodem_v34_symbol_power_measurement *measurement)
{
	return measurement != NULL &&
	       isfinite(measurement->mean_squared_magnitude) &&
	       measurement->mean_squared_magnitude >= 0.0 &&
	       (measurement->symbol_count != 0 ||
		measurement->mean_squared_magnitude == 0.0);
}

enum sippymodem_v34_symbol_power_result
sippymodem_v34_symbol_power_measurement_reset(
	struct sippymodem_v34_symbol_power_measurement *measurement)
{
	if (measurement == NULL)
		return SIPPYMODEM_V34_SYMBOL_POWER_INVALID_ARGUMENT;
	*measurement =
		(struct sippymodem_v34_symbol_power_measurement){ 0 };
	return SIPPYMODEM_V34_SYMBOL_POWER_OK;
}

static enum sippymodem_v34_symbol_power_result observe_power(
	struct sippymodem_v34_symbol_power_measurement *measurement,
	double squared_magnitude)
{
	struct sippymodem_v34_symbol_power_measurement next;

	if (!sippymodem_v34_symbol_power_measurement_is_valid(measurement) ||
	    !isfinite(squared_magnitude) || squared_magnitude < 0.0)
		return SIPPYMODEM_V34_SYMBOL_POWER_INVALID_ARGUMENT;
	if (measurement->symbol_count == UINT32_MAX)
		return SIPPYMODEM_V34_SYMBOL_POWER_OUT_OF_RANGE;
	next = *measurement;
	next.symbol_count++;
	next.mean_squared_magnitude +=
		(squared_magnitude - next.mean_squared_magnitude) /
		next.symbol_count;
	if (!isfinite(next.mean_squared_magnitude) ||
	    next.mean_squared_magnitude < 0.0)
		return SIPPYMODEM_V34_SYMBOL_POWER_NUMERIC_FAILURE;
	*measurement = next;
	return SIPPYMODEM_V34_SYMBOL_POWER_OK;
}

enum sippymodem_v34_symbol_power_result
sippymodem_v34_symbol_power_observe_q7(
	struct sippymodem_v34_symbol_power_measurement *measurement,
	const struct sippymodem_v34_q7_point *point)
{
	struct sippymodem_qam_point converted;

	if (point == NULL)
		return SIPPYMODEM_V34_SYMBOL_POWER_INVALID_ARGUMENT;
	converted = (struct sippymodem_qam_point){
		.in_phase =
			(double)point->in_phase / SIPPYMODEM_V34_Q7_SCALE,
		.quadrature =
			(double)point->quadrature / SIPPYMODEM_V34_Q7_SCALE,
	};
	return sippymodem_v34_symbol_power_observe_point(
		measurement, &converted);
}

enum sippymodem_v34_symbol_power_result
sippymodem_v34_symbol_power_observe_point(
	struct sippymodem_v34_symbol_power_measurement *measurement,
	const struct sippymodem_qam_point *point)
{
	double squared_magnitude;

	if (point == NULL || !isfinite(point->in_phase) ||
	    !isfinite(point->quadrature))
		return SIPPYMODEM_V34_SYMBOL_POWER_INVALID_ARGUMENT;
	squared_magnitude = point->in_phase * point->in_phase +
		point->quadrature * point->quadrature;
	if (!isfinite(squared_magnitude))
		return SIPPYMODEM_V34_SYMBOL_POWER_NUMERIC_FAILURE;
	return observe_power(measurement, squared_magnitude);
}

const char *sippymodem_v34_symbol_power_result_name(
	enum sippymodem_v34_symbol_power_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_SYMBOL_POWER_OK:
		return "ok";
	case SIPPYMODEM_V34_SYMBOL_POWER_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_V34_SYMBOL_POWER_OUT_OF_RANGE:
		return "out of range";
	case SIPPYMODEM_V34_SYMBOL_POWER_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
