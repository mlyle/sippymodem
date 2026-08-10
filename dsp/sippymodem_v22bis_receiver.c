// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v22bis_receiver.h"

#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>

static const double V22BIS_MIN_POINT_POWER = 1e-12;
static const double V22BIS_CHANNEL_ADAPTATION = 1.0 / 32.0;
static const double V22BIS_V22_REFERENCE_ERROR_RATIO = 0.08;

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

bool sippymodem_v22bis_receiver_is_valid(
	const struct sippymodem_v22bis_receiver *receiver)
{
	double power;

	return receiver != NULL &&
	       receiver->decoder.phase_quadrant <
		       SIPPYMODEM_QPSK_QUADRANT_COUNT &&
	       sippymodem_v22_scrambler_is_valid(
		       &receiver->decoder.descrambler) &&
	       point_power(&receiver->channel_gain, &power);
}

static struct sippymodem_qam_point quadrant_point(uint8_t quadrant)
{
	switch (quadrant) {
	case 0:
		return (struct sippymodem_qam_point){ 1.0, 0.0 };
	case 1:
		return (struct sippymodem_qam_point){ 0.0, 1.0 };
	case 2:
		return (struct sippymodem_qam_point){ -1.0, 0.0 };
	default:
		return (struct sippymodem_qam_point){ 0.0, -1.0 };
	}
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
	struct sippymodem_v22bis_receiver next = { 0 };
	double power;

	if (receiver == NULL || !point_power(reference_point, &power) ||
	    sippymodem_v22bis_symbol_decoder_init(&next.decoder, 0) !=
		    SIPPYMODEM_V22_FRAMING_OK)
		return SIPPYMODEM_V22BIS_RECEIVER_INVALID_ARGUMENT;
	next.channel_gain = *reference_point;
	*receiver = next;
	return SIPPYMODEM_V22BIS_RECEIVER_OK;
}

enum sippymodem_v22bis_receiver_result
sippymodem_v22bis_receiver_matches_v22(
	const struct sippymodem_v22bis_receiver *receiver,
	const struct sippymodem_qam_point *point, bool *matches)
{
	double gain_power;
	double received_power;
	double best_error = DBL_MAX;

	if (!sippymodem_v22bis_receiver_is_valid(receiver) ||
	    receiver->v22bis_started || !point_power(point, &received_power) ||
	    matches == NULL ||
	    !point_power(&receiver->channel_gain, &gain_power))
		return SIPPYMODEM_V22BIS_RECEIVER_INVALID_ARGUMENT;
	for (uint8_t quadrant = 0;
	     quadrant < SIPPYMODEM_QPSK_QUADRANT_COUNT; ++quadrant) {
		struct sippymodem_qam_point ideal = quadrant_point(quadrant);
		struct sippymodem_qam_point predicted =
			complex_multiply(&receiver->channel_gain, &ideal);
		double error = squared_error(point, &predicted);

		if (error < best_error)
			best_error = error;
	}
	*matches = best_error <=
		   gain_power * V22BIS_V22_REFERENCE_ERROR_RATIO;
	return SIPPYMODEM_V22BIS_RECEIVER_OK;
}

enum sippymodem_v22bis_receiver_result
sippymodem_v22bis_receiver_track_v22(
	struct sippymodem_v22bis_receiver *receiver, uint8_t quarter_turns,
	const struct sippymodem_qam_point *point)
{
	struct sippymodem_v22bis_receiver next;
	struct sippymodem_qam_point ideal;
	double received_power;

	if (!sippymodem_v22bis_receiver_is_valid(receiver) ||
	    receiver->v22bis_started ||
	    quarter_turns >= SIPPYMODEM_QPSK_QUADRANT_COUNT ||
	    !point_power(point, &received_power))
		return SIPPYMODEM_V22BIS_RECEIVER_INVALID_ARGUMENT;
	next = *receiver;
	next.decoder.phase_quadrant =
		(uint8_t)((next.decoder.phase_quadrant + quarter_turns) & 3u);
	ideal = quadrant_point(next.decoder.phase_quadrant);
	next.channel_gain = divide_by_complex(point, &ideal, 1.0);
	if (!sippymodem_v22bis_receiver_is_valid(&next))
		return SIPPYMODEM_V22BIS_RECEIVER_INVALID_ARGUMENT;
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

	if (!sippymodem_v22bis_receiver_is_valid(receiver) ||
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
	next.v22bis_started = true;
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
	if (!sippymodem_v22bis_receiver_is_valid(&next))
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
