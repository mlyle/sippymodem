// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_qam_gain.h"

#include <math.h>
#include <stddef.h>

static const double QAM_GAIN_MINIMUM_POWER = 1e-12;

static bool point_is_finite(const struct sippymodem_qam_point *point)
{
	return point != NULL && isfinite(point->in_phase) &&
	       isfinite(point->quadrature);
}

static bool nonzero_point_power(
	const struct sippymodem_qam_point *point, double *power)
{
	double value;

	if (!point_is_finite(point))
		return false;
	value = point->in_phase * point->in_phase +
		point->quadrature * point->quadrature;
	if (!isfinite(value) || value <= QAM_GAIN_MINIMUM_POWER)
		return false;
	if (power != NULL)
		*power = value;
	return true;
}

static bool multiply_points(
	const struct sippymodem_qam_point *left,
	const struct sippymodem_qam_point *right,
	struct sippymodem_qam_point *product)
{
	struct sippymodem_qam_point next;

	if (!point_is_finite(left) || !point_is_finite(right) ||
	    product == NULL)
		return false;
	next = (struct sippymodem_qam_point){
		.in_phase = left->in_phase * right->in_phase -
			    left->quadrature * right->quadrature,
		.quadrature = left->in_phase * right->quadrature +
			      left->quadrature * right->in_phase,
	};
	if (!point_is_finite(&next))
		return false;
	*product = next;
	return true;
}

static bool divide_points(
	const struct sippymodem_qam_point *numerator,
	const struct sippymodem_qam_point *denominator,
	struct sippymodem_qam_point *quotient)
{
	struct sippymodem_qam_point next;
	double power;

	if (!point_is_finite(numerator) || quotient == NULL ||
	    !nonzero_point_power(denominator, &power))
		return false;
	next = (struct sippymodem_qam_point){
		.in_phase =
			(numerator->in_phase * denominator->in_phase +
			 numerator->quadrature * denominator->quadrature) /
			power,
		.quadrature =
			(numerator->quadrature * denominator->in_phase -
			 numerator->in_phase * denominator->quadrature) /
			power,
	};
	if (!point_is_finite(&next))
		return false;
	*quotient = next;
	return true;
}

bool sippymodem_qam_gain_is_valid(
	const struct sippymodem_qam_gain *tracker)
{
	return tracker != NULL &&
	       nonzero_point_power(&tracker->channel_gain, NULL) &&
	       isfinite(tracker->adaptation) && tracker->adaptation >= 0.0 &&
	       tracker->adaptation <= 1.0;
}

enum sippymodem_qam_gain_result sippymodem_qam_gain_init(
	struct sippymodem_qam_gain *tracker,
	const struct sippymodem_qam_point *channel_gain, double adaptation)
{
	struct sippymodem_qam_gain initialized;

	if (tracker == NULL || channel_gain == NULL)
		return SIPPYMODEM_QAM_GAIN_INVALID_ARGUMENT;
	initialized = (struct sippymodem_qam_gain){
		.channel_gain = *channel_gain,
		.adaptation = adaptation,
	};
	if (!sippymodem_qam_gain_is_valid(&initialized))
		return SIPPYMODEM_QAM_GAIN_INVALID_ARGUMENT;
	*tracker = initialized;
	return SIPPYMODEM_QAM_GAIN_OK;
}

enum sippymodem_qam_gain_result sippymodem_qam_gain_init_from_reference(
	struct sippymodem_qam_gain *tracker,
	const struct sippymodem_qam_point *received,
	const struct sippymodem_qam_point *transmitted, double adaptation)
{
	struct sippymodem_qam_point observed_gain;

	if (tracker == NULL ||
	    !divide_points(received, transmitted, &observed_gain))
		return SIPPYMODEM_QAM_GAIN_INVALID_ARGUMENT;
	return sippymodem_qam_gain_init(tracker, &observed_gain, adaptation);
}

enum sippymodem_qam_gain_result sippymodem_qam_gain_predict(
	const struct sippymodem_qam_gain *tracker,
	const struct sippymodem_qam_point *transmitted,
	struct sippymodem_qam_point *received)
{
	struct sippymodem_qam_point predicted;

	if (!sippymodem_qam_gain_is_valid(tracker) || received == NULL ||
	    !multiply_points(&tracker->channel_gain, transmitted, &predicted))
		return SIPPYMODEM_QAM_GAIN_INVALID_ARGUMENT;
	*received = predicted;
	return SIPPYMODEM_QAM_GAIN_OK;
}

enum sippymodem_qam_gain_result sippymodem_qam_gain_equalize(
	const struct sippymodem_qam_gain *tracker,
	const struct sippymodem_qam_point *received,
	struct sippymodem_qam_point *equalized)
{
	struct sippymodem_qam_point next;

	if (!sippymodem_qam_gain_is_valid(tracker) || equalized == NULL ||
	    !divide_points(received, &tracker->channel_gain, &next))
		return SIPPYMODEM_QAM_GAIN_INVALID_ARGUMENT;
	*equalized = next;
	return SIPPYMODEM_QAM_GAIN_OK;
}

enum sippymodem_qam_gain_result sippymodem_qam_gain_track(
	struct sippymodem_qam_gain *tracker,
	const struct sippymodem_qam_point *received,
	const struct sippymodem_qam_point *transmitted)
{
	struct sippymodem_qam_gain next;
	struct sippymodem_qam_point observed_gain;

	if (!sippymodem_qam_gain_is_valid(tracker) ||
	    !nonzero_point_power(received, NULL) ||
	    !divide_points(received, transmitted, &observed_gain))
		return SIPPYMODEM_QAM_GAIN_INVALID_ARGUMENT;
	next = *tracker;
	next.channel_gain.in_phase +=
		(observed_gain.in_phase - next.channel_gain.in_phase) *
		next.adaptation;
	next.channel_gain.quadrature +=
		(observed_gain.quadrature - next.channel_gain.quadrature) *
		next.adaptation;
	if (!sippymodem_qam_gain_is_valid(&next))
		return SIPPYMODEM_QAM_GAIN_INVALID_ARGUMENT;
	*tracker = next;
	return SIPPYMODEM_QAM_GAIN_OK;
}

const char *sippymodem_qam_gain_result_name(
	enum sippymodem_qam_gain_result result)
{
	switch (result) {
	case SIPPYMODEM_QAM_GAIN_OK:
		return "ok";
	case SIPPYMODEM_QAM_GAIN_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
