// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v32_9600_receiver.h"

#include <float.h>
#include <math.h>
#include <stddef.h>

static const double V32_9600_MIN_POINT_POWER = 1e-12;
static const double V32_9600_CHANNEL_ADAPTATION = 1.0 / 32.0;

_Static_assert((int)SIPPYMODEM_V32_9600_SYMBOL_COUNT ==
		       (int)SIPPYMODEM_V32_CONSTELLATION_SYMBOLS,
	       "V.32 9600 framing and constellation sizes must agree");

static bool point_power(
	const struct sippymodem_qam_point *point, double *power)
{
	double value;

	if (point == NULL || power == NULL || !isfinite(point->in_phase) ||
	    !isfinite(point->quadrature))
		return false;
	value = point->in_phase * point->in_phase +
		point->quadrature * point->quadrature;
	if (!isfinite(value) || value <= V32_9600_MIN_POINT_POWER)
		return false;
	*power = value;
	return true;
}

static bool decoder_is_valid(
	const struct sippymodem_v32_symbol_decoder *decoder)
{
	return decoder != NULL &&
	       decoder->phase_quadrant < SIPPYMODEM_QPSK_QUADRANT_COUNT &&
	       sippymodem_v32_scrambler_is_valid(&decoder->descrambler);
}

bool sippymodem_v32_9600_receiver_is_valid(
	const struct sippymodem_v32_9600_receiver *receiver)
{
	double power;

	return receiver != NULL && decoder_is_valid(&receiver->decoder) &&
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

static double squared_error(
	const struct sippymodem_qam_point *left,
	const struct sippymodem_qam_point *right)
{
	double in_phase = left->in_phase - right->in_phase;
	double quadrature = left->quadrature - right->quadrature;

	return in_phase * in_phase + quadrature * quadrature;
}

enum sippymodem_v32_9600_receiver_result
sippymodem_v32_9600_receiver_init(
	struct sippymodem_v32_9600_receiver *receiver,
	const struct sippymodem_v32_symbol_decoder *decoder,
	const struct sippymodem_qam_point *reference_point)
{
	struct sippymodem_v32_9600_receiver next;
	struct sippymodem_qam_point ideal;
	double ideal_power;
	double reference_power;

	if (receiver == NULL || !decoder_is_valid(decoder) ||
	    !point_power(reference_point, &reference_power) ||
	    sippymodem_v32_4800_constellation_point(
		    decoder->phase_quadrant, &ideal) !=
		    SIPPYMODEM_V32_CONSTELLATION_OK ||
	    !point_power(&ideal, &ideal_power))
		return SIPPYMODEM_V32_9600_RECEIVER_INVALID_ARGUMENT;
	next.decoder = *decoder;
	next.channel_gain =
		divide_by_complex(reference_point, &ideal, ideal_power);
	if (!sippymodem_v32_9600_receiver_is_valid(&next))
		return SIPPYMODEM_V32_9600_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
	return SIPPYMODEM_V32_9600_RECEIVER_OK;
}

enum sippymodem_v32_9600_receiver_result
sippymodem_v32_9600_receiver_decode(
	struct sippymodem_v32_9600_receiver *receiver,
	const struct sippymodem_qam_point *point, uint8_t *output)
{
	struct sippymodem_v32_9600_receiver next;
	struct sippymodem_qam_point best_point = { 0 };
	uint8_t best_symbol = 0;
	uint8_t decoded;
	double best_error = DBL_MAX;
	double received_power;

	if (!sippymodem_v32_9600_receiver_is_valid(receiver) ||
	    !point_power(point, &received_power) || output == NULL)
		return SIPPYMODEM_V32_9600_RECEIVER_INVALID_ARGUMENT;
	next = *receiver;
	for (uint8_t symbol = 0;
	     symbol < SIPPYMODEM_V32_9600_SYMBOL_COUNT; ++symbol) {
		struct sippymodem_qam_point candidate;
		struct sippymodem_qam_point predicted;
		double error;

		if (sippymodem_v32_constellation_point(symbol, &candidate) !=
		    SIPPYMODEM_V32_CONSTELLATION_OK)
			return SIPPYMODEM_V32_9600_RECEIVER_INVALID_ARGUMENT;
		predicted = complex_multiply(&next.channel_gain, &candidate);
		error = squared_error(point, &predicted);
		if (isfinite(error) && error < best_error) {
			best_error = error;
			best_symbol = symbol;
			best_point = candidate;
		}
	}
	if (best_error == DBL_MAX ||
	    sippymodem_v32_9600_symbol_decode(
		    &next.decoder, best_symbol, &decoded) !=
		    SIPPYMODEM_V32_FRAMING_OK)
		return SIPPYMODEM_V32_9600_RECEIVER_INVALID_ARGUMENT;
	{
		struct sippymodem_qam_point observed_gain;
		double ideal_power;

		if (!point_power(&best_point, &ideal_power))
			return SIPPYMODEM_V32_9600_RECEIVER_INVALID_ARGUMENT;
		observed_gain =
			divide_by_complex(point, &best_point, ideal_power);
		next.channel_gain.in_phase +=
			(observed_gain.in_phase - next.channel_gain.in_phase) *
			V32_9600_CHANNEL_ADAPTATION;
		next.channel_gain.quadrature +=
			(observed_gain.quadrature -
			 next.channel_gain.quadrature) *
			V32_9600_CHANNEL_ADAPTATION;
	}
	if (!sippymodem_v32_9600_receiver_is_valid(&next))
		return SIPPYMODEM_V32_9600_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
	*output = decoded;
	return SIPPYMODEM_V32_9600_RECEIVER_OK;
}

const char *sippymodem_v32_9600_receiver_result_name(
	enum sippymodem_v32_9600_receiver_result result)
{
	switch (result) {
	case SIPPYMODEM_V32_9600_RECEIVER_OK:
		return "ok";
	case SIPPYMODEM_V32_9600_RECEIVER_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
