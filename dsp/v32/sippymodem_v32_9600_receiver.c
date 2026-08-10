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

static bool point_has_nonzero_power(
	const struct sippymodem_qam_point *point)
{
	double value;

	if (point == NULL || !isfinite(point->in_phase) ||
	    !isfinite(point->quadrature))
		return false;
	value = point->in_phase * point->in_phase +
		point->quadrature * point->quadrature;
	return isfinite(value) && value > V32_9600_MIN_POINT_POWER;
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
	return receiver != NULL && decoder_is_valid(&receiver->decoder) &&
	       sippymodem_qam_gain_is_valid(&receiver->gain);
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
	struct sippymodem_v32_9600_receiver next = { 0 };
	struct sippymodem_qam_point ideal;

	if (receiver == NULL || !decoder_is_valid(decoder) ||
	    sippymodem_v32_4800_constellation_point(
		    decoder->phase_quadrant, &ideal) !=
		    SIPPYMODEM_V32_CONSTELLATION_OK ||
	    sippymodem_qam_gain_init_from_reference(
		    &next.gain, reference_point, &ideal,
		    V32_9600_CHANNEL_ADAPTATION) != SIPPYMODEM_QAM_GAIN_OK)
		return SIPPYMODEM_V32_9600_RECEIVER_INVALID_ARGUMENT;
	next.decoder = *decoder;
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

	if (!sippymodem_v32_9600_receiver_is_valid(receiver) ||
	    !point_has_nonzero_power(point) || output == NULL)
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
		if (sippymodem_qam_gain_predict(
			    &next.gain, &candidate, &predicted) !=
		    SIPPYMODEM_QAM_GAIN_OK)
			return SIPPYMODEM_V32_9600_RECEIVER_INVALID_ARGUMENT;
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
	if (sippymodem_qam_gain_track(&next.gain, point, &best_point) !=
	    SIPPYMODEM_QAM_GAIN_OK)
		return SIPPYMODEM_V32_9600_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
	*output = decoded;
	return SIPPYMODEM_V32_9600_RECEIVER_OK;
}

enum sippymodem_v32_9600_receiver_result
sippymodem_v32_9600_receiver_track_known(
	struct sippymodem_v32_9600_receiver *receiver,
	const struct sippymodem_qam_point *point, uint8_t input)
{
	struct sippymodem_v32_9600_receiver next;
	struct sippymodem_v32_symbol_encoder encoder;
	struct sippymodem_qam_point transmitted;
	uint8_t symbol;

	if (!sippymodem_v32_9600_receiver_is_valid(receiver) ||
	    !point_has_nonzero_power(point) ||
	    input >= SIPPYMODEM_V32_9600_SYMBOL_COUNT)
		return SIPPYMODEM_V32_9600_RECEIVER_INVALID_ARGUMENT;
	next = *receiver;
	encoder = (struct sippymodem_v32_symbol_encoder){
		.scrambler = next.decoder.descrambler,
		.phase_quadrant = next.decoder.phase_quadrant,
	};
	if (sippymodem_v32_9600_symbol_encode(&encoder, input, &symbol) !=
		    SIPPYMODEM_V32_FRAMING_OK ||
	    sippymodem_v32_constellation_point(symbol, &transmitted) !=
		    SIPPYMODEM_V32_CONSTELLATION_OK)
		return SIPPYMODEM_V32_9600_RECEIVER_INVALID_ARGUMENT;
	next.decoder.descrambler = encoder.scrambler;
	next.decoder.phase_quadrant = encoder.phase_quadrant;
	if (sippymodem_qam_gain_track(&next.gain, point, &transmitted) !=
	    SIPPYMODEM_QAM_GAIN_OK)
		return SIPPYMODEM_V32_9600_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
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
