// SPDX-License-Identifier: BSD-3-Clause

#include "qam_gain.h"

#include <stddef.h>

static bool divide_points(
	const struct qam_point *numerator,
	const struct qam_point *denominator,
	struct qam_point *quotient)
{
	struct qam_point next;
	double power;

	if (!point_is_finite(numerator) || quotient == NULL ||
	    !qam_point_nonzero_power(denominator, &power))
		return false;
	next = qam_point_quotient(numerator, denominator, power);
	if (!point_is_finite(&next))
		return false;
	*quotient = next;
	return true;
}

bool qam_gain_is_valid(
	const struct qam_gain *tracker)
{
	return tracker != NULL &&
	       qam_point_nonzero_power(&tracker->channel_gain, NULL) &&
	       qam_adaptation_is_valid(tracker->adaptation);
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
	    !point_is_finite(transmitted))
		return QAM_GAIN_INVALID_ARGUMENT;
	predicted = qam_point_multiply(&tracker->channel_gain, transmitted);
	if (!point_is_finite(&predicted))
		return QAM_GAIN_INVALID_ARGUMENT;
	*received = predicted;
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
	    !qam_point_nonzero_power(received, NULL) ||
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
