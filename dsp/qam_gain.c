// SPDX-License-Identifier: BSD-3-Clause

#include "qam_gain.h"

#include <math.h>
#include <stddef.h>

static const double QAM_GAIN_MINIMUM_POWER = 1e-12;

static bool point_is_finite(const struct qam_point *point)
{
	return point != NULL && isfinite(point->in_phase) &&
	       isfinite(point->quadrature);
}

static bool nonzero_point_power(
	const struct qam_point *point, double *power)
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
	const struct qam_point *left,
	const struct qam_point *right,
	struct qam_point *product)
{
	struct qam_point next;

	if (!point_is_finite(left) || !point_is_finite(right) ||
	    product == NULL)
		return false;
	next = (struct qam_point){
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
	const struct qam_point *numerator,
	const struct qam_point *denominator,
	struct qam_point *quotient)
{
	struct qam_point next;
	double power;

	if (!point_is_finite(numerator) || quotient == NULL ||
	    !nonzero_point_power(denominator, &power))
		return false;
	next = (struct qam_point){
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

bool qam_gain_is_valid(
	const struct qam_gain *tracker)
{
	return tracker != NULL &&
	       nonzero_point_power(&tracker->channel_gain, NULL) &&
	       isfinite(tracker->adaptation) && tracker->adaptation >= 0.0 &&
	       tracker->adaptation <= 1.0;
}

enum qam_gain_result qam_gain_init(
	struct qam_gain *tracker,
	const struct qam_point *channel_gain, double adaptation)
{
	struct qam_gain initialized;

	if (tracker == NULL || channel_gain == NULL)
		return QAM_GAIN_INVALID_ARGUMENT;
	initialized = (struct qam_gain){
		.channel_gain = *channel_gain,
		.adaptation = adaptation,
	};
	if (!qam_gain_is_valid(&initialized))
		return QAM_GAIN_INVALID_ARGUMENT;
	*tracker = initialized;
	return QAM_GAIN_OK;
}

enum qam_gain_result qam_gain_init_from_reference(
	struct qam_gain *tracker,
	const struct qam_point *received,
	const struct qam_point *transmitted, double adaptation)
{
	struct qam_point observed_gain;

	if (tracker == NULL ||
	    !divide_points(received, transmitted, &observed_gain))
		return QAM_GAIN_INVALID_ARGUMENT;
	return qam_gain_init(tracker, &observed_gain, adaptation);
}

enum qam_gain_result qam_gain_predict(
	const struct qam_gain *tracker,
	const struct qam_point *transmitted,
	struct qam_point *received)
{
	struct qam_point predicted;

	if (!qam_gain_is_valid(tracker) || received == NULL ||
	    !multiply_points(&tracker->channel_gain, transmitted, &predicted))
		return QAM_GAIN_INVALID_ARGUMENT;
	*received = predicted;
	return QAM_GAIN_OK;
}

enum qam_gain_result qam_gain_equalize(
	const struct qam_gain *tracker,
	const struct qam_point *received,
	struct qam_point *equalized)
{
	struct qam_point next;

	if (!qam_gain_is_valid(tracker) || equalized == NULL ||
	    !divide_points(received, &tracker->channel_gain, &next))
		return QAM_GAIN_INVALID_ARGUMENT;
	*equalized = next;
	return QAM_GAIN_OK;
}

enum qam_gain_result qam_gain_track(
	struct qam_gain *tracker,
	const struct qam_point *received,
	const struct qam_point *transmitted)
{
	struct qam_gain next;
	struct qam_point observed_gain;

	if (!qam_gain_is_valid(tracker) ||
	    !nonzero_point_power(received, NULL) ||
	    !divide_points(received, transmitted, &observed_gain))
		return QAM_GAIN_INVALID_ARGUMENT;
	next = *tracker;
	next.channel_gain.in_phase +=
		(observed_gain.in_phase - next.channel_gain.in_phase) *
		next.adaptation;
	next.channel_gain.quadrature +=
		(observed_gain.quadrature - next.channel_gain.quadrature) *
		next.adaptation;
	if (!qam_gain_is_valid(&next))
		return QAM_GAIN_INVALID_ARGUMENT;
	*tracker = next;
	return QAM_GAIN_OK;
}

const char *qam_gain_result_name(
	enum qam_gain_result result)
{
	switch (result) {
	case QAM_GAIN_OK:
		return "ok";
	case QAM_GAIN_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
