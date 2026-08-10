// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v22bis_receiver.h"

#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>

static const double V22BIS_MIN_POINT_POWER = 1e-12;
static const double V22BIS_CHANNEL_ADAPTATION = 1.0 / 32.0;

static bool point_power(
	const struct sippymodem_qam_point *point, double *power)
{
	double value;

	if (point == NULL || power == NULL || !isfinite(point->in_phase) ||
	    !isfinite(point->quadrature))
		return false;
	value = point->in_phase * point->in_phase +
		point->quadrature * point->quadrature;
	if (!isfinite(value) || value <= V22BIS_MIN_POINT_POWER)
		return false;
	*power = value;
	return true;
}

static bool receiver_is_valid(
	const struct sippymodem_v22bis_receiver *receiver)
{
	double power;

	return receiver != NULL &&
	       receiver->decoder.phase_quadrant <
		       SIPPYMODEM_V22_PHASE_QUADRANTS &&
	       sippymodem_v22_scrambler_is_valid(
		       &receiver->decoder.descrambler) &&
	       point_power(&receiver->channel_gain, &power);
}

static struct sippymodem_qam_point complex_multiply(
	const struct sippymodem_qam_point *left,
	const struct sippymodem_qam_point *right)
{
	return (struct sippymodem_qam_point){
		.in_phase = left->in_phase * right->in_phase -
			    left->quadrature * right->quadrature,
		.quadrature = left->in_phase * right->quadrature +
			      left->quadrature * right->in_phase,
	};
}

static double squared_error(
	const struct sippymodem_qam_point *left,
	const struct sippymodem_qam_point *right)
{
	double in_phase = left->in_phase - right->in_phase;
	double quadrature = left->quadrature - right->quadrature;

	return in_phase * in_phase + quadrature * quadrature;
}

static struct sippymodem_qam_point divide_by_complex(
	const struct sippymodem_qam_point *numerator,
	const struct sippymodem_qam_point *denominator,
	double denominator_power)
{
	return (struct sippymodem_qam_point){
		.in_phase =
			(numerator->in_phase * denominator->in_phase +
			 numerator->quadrature * denominator->quadrature) /
			denominator_power,
		.quadrature =
			(numerator->quadrature * denominator->in_phase -
			 numerator->in_phase * denominator->quadrature) /
			denominator_power,
	};
}

enum sippymodem_v22bis_receiver_result sippymodem_v22bis_receiver_init(
	struct sippymodem_v22bis_receiver *receiver,
	const struct sippymodem_qam_point *reference_point)
{
	struct sippymodem_v22bis_receiver next;
	double power;

	if (receiver == NULL || !point_power(reference_point, &power) ||
	    sippymodem_v22bis_symbol_decoder_init(&next.decoder, 0) !=
		    SIPPYMODEM_V22_FRAMING_OK)
		return SIPPYMODEM_V22BIS_RECEIVER_INVALID_ARGUMENT;
	next.channel_gain = *reference_point;
	*receiver = next;
	return SIPPYMODEM_V22BIS_RECEIVER_OK;
}

enum sippymodem_v22bis_receiver_result sippymodem_v22bis_receiver_decode(
	struct sippymodem_v22bis_receiver *receiver,
	const struct sippymodem_qam_point *point,
	uint8_t bits[SIPPYMODEM_V22BIS_BITS_PER_SYMBOL])
{
	struct sippymodem_v22bis_receiver next;
	struct sippymodem_qam_point best_point = { 0 };
	uint8_t decoded[SIPPYMODEM_V22BIS_BITS_PER_SYMBOL];
	uint8_t best_symbol = 0;
	double received_power;
	double best_error = DBL_MAX;

	if (!receiver_is_valid(receiver) ||
	    !point_power(point, &received_power) || bits == NULL)
		return SIPPYMODEM_V22BIS_RECEIVER_INVALID_ARGUMENT;
	next = *receiver;
	for (uint8_t symbol = 0; symbol < SIPPYMODEM_V22BIS_SYMBOL_COUNT;
	     ++symbol) {
		struct sippymodem_qam_point candidate;
		struct sippymodem_qam_point predicted;
		double error;

		if (sippymodem_v22bis_constellation_point(
			    symbol, &candidate) != SIPPYMODEM_V22_FRAMING_OK)
			return SIPPYMODEM_V22BIS_RECEIVER_INVALID_ARGUMENT;
		predicted = complex_multiply(&next.channel_gain, &candidate);
		error = squared_error(point, &predicted);
		if (isfinite(error) && error < best_error) {
			best_error = error;
			best_symbol = symbol;
			best_point = candidate;
		}
	}
	if (best_error == DBL_MAX ||
	    sippymodem_v22bis_symbol_decode(
		    &next.decoder, best_symbol, decoded) !=
		    SIPPYMODEM_V22_FRAMING_OK)
		return SIPPYMODEM_V22BIS_RECEIVER_INVALID_ARGUMENT;
	{
		double ideal_power;
		struct sippymodem_qam_point observed_gain;

		if (!point_power(&best_point, &ideal_power))
			return SIPPYMODEM_V22BIS_RECEIVER_INVALID_ARGUMENT;
		observed_gain = divide_by_complex(point, &best_point, ideal_power);
		next.channel_gain.in_phase +=
			(observed_gain.in_phase - next.channel_gain.in_phase) *
			V22BIS_CHANNEL_ADAPTATION;
		next.channel_gain.quadrature +=
			(observed_gain.quadrature - next.channel_gain.quadrature) *
			V22BIS_CHANNEL_ADAPTATION;
	}
	if (!receiver_is_valid(&next))
		return SIPPYMODEM_V22BIS_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
	for (size_t index = 0; index < SIPPYMODEM_V22BIS_BITS_PER_SYMBOL;
	     ++index)
		bits[index] = decoded[index];
	return SIPPYMODEM_V22BIS_RECEIVER_OK;
}

const char *sippymodem_v22bis_receiver_result_name(
	enum sippymodem_v22bis_receiver_result result)
{
	switch (result) {
	case SIPPYMODEM_V22BIS_RECEIVER_OK:
		return "ok";
	case SIPPYMODEM_V22BIS_RECEIVER_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
