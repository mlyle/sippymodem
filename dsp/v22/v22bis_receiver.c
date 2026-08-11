// SPDX-License-Identifier: BSD-3-Clause

#include "v22bis_receiver.h"

#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>

static const double V22BIS_MIN_POINT_POWER = 1e-12;
static const double V22BIS_CHANNEL_ADAPTATION = 1.0 / 32.0;
static const double V22BIS_V22_REFERENCE_ERROR_RATIO = 0.08;

static bool point_power(
	const struct qam_point *point, double *power)
{
	double value;

	if (point == NULL || !isfinite(point->in_phase) ||
	    !isfinite(point->quadrature))
		return false;
	value = point->in_phase * point->in_phase +
		point->quadrature * point->quadrature;
	if (!isfinite(value) || value <= V22BIS_MIN_POINT_POWER)
		return false;
	if (power != NULL)
		*power = value;
	return true;
}

bool sippymodem_v22bis_receiver_is_valid(
	const struct sippymodem_v22bis_receiver *receiver)
{
	return receiver != NULL &&
	       receiver->decoder.phase_quadrant <
		       QPSK_QUADRANT_COUNT &&
	       sippymodem_v22_scrambler_is_valid(
		       &receiver->decoder.descrambler) &&
	       qam_gain_is_valid(&receiver->gain);
}

static struct qam_point quadrant_point(uint8_t quadrant)
{
	switch (quadrant) {
	case 0:
		return (struct qam_point){ 1.0, 0.0 };
	case 1:
		return (struct qam_point){ 0.0, 1.0 };
	case 2:
		return (struct qam_point){ -1.0, 0.0 };
	default:
		return (struct qam_point){ 0.0, -1.0 };
	}
}

static double squared_error(
	const struct qam_point *left,
	const struct qam_point *right)
{
	double in_phase = left->in_phase - right->in_phase;
	double quadrature = left->quadrature - right->quadrature;

	return in_phase * in_phase + quadrature * quadrature;
}

enum sippymodem_v22bis_receiver_result sippymodem_v22bis_receiver_init(
	struct sippymodem_v22bis_receiver *receiver,
	const struct qam_point *reference_point)
{
	struct sippymodem_v22bis_receiver next = { 0 };

	if (receiver == NULL ||
	    qam_gain_init(
		    &next.gain, reference_point,
		    V22BIS_CHANNEL_ADAPTATION) != QAM_GAIN_OK ||
	    sippymodem_v22bis_symbol_decoder_init(&next.decoder, 0) !=
		    SIPPYMODEM_V22_FRAMING_OK)
		return SIPPYMODEM_V22BIS_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
	return SIPPYMODEM_V22BIS_RECEIVER_OK;
}

enum sippymodem_v22bis_receiver_result
sippymodem_v22bis_receiver_matches_v22(
	const struct sippymodem_v22bis_receiver *receiver,
	const struct qam_point *point, bool *matches)
{
	double gain_power;
	double best_error = DBL_MAX;

	if (!sippymodem_v22bis_receiver_is_valid(receiver) ||
	    receiver->v22bis_started || !point_power(point, NULL) ||
	    matches == NULL ||
	    !point_power(&receiver->gain.channel_gain, &gain_power))
		return SIPPYMODEM_V22BIS_RECEIVER_INVALID_ARGUMENT;
	for (uint8_t quadrant = 0;
	     quadrant < QPSK_QUADRANT_COUNT; ++quadrant) {
		struct qam_point ideal = quadrant_point(quadrant);
		struct qam_point predicted;
		double error;

		if (qam_gain_predict(
			    &receiver->gain, &ideal, &predicted) !=
		    QAM_GAIN_OK)
			return SIPPYMODEM_V22BIS_RECEIVER_INVALID_ARGUMENT;
		error = squared_error(point, &predicted);

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
	const struct qam_point *point)
{
	struct sippymodem_v22bis_receiver next;
	struct qam_point ideal;

	if (!sippymodem_v22bis_receiver_is_valid(receiver) ||
	    receiver->v22bis_started ||
	    quarter_turns >= QPSK_QUADRANT_COUNT ||
	    !point_power(point, NULL))
		return SIPPYMODEM_V22BIS_RECEIVER_INVALID_ARGUMENT;
	next = *receiver;
	next.decoder.phase_quadrant =
		(uint8_t)((next.decoder.phase_quadrant + quarter_turns) & 3u);
	ideal = quadrant_point(next.decoder.phase_quadrant);
	if (qam_gain_init_from_reference(
		    &next.gain, point, &ideal, V22BIS_CHANNEL_ADAPTATION) !=
	    QAM_GAIN_OK)
		return SIPPYMODEM_V22BIS_RECEIVER_INVALID_ARGUMENT;
	if (!sippymodem_v22bis_receiver_is_valid(&next))
		return SIPPYMODEM_V22BIS_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
	return SIPPYMODEM_V22BIS_RECEIVER_OK;
}

enum sippymodem_v22bis_receiver_result sippymodem_v22bis_receiver_decode(
	struct sippymodem_v22bis_receiver *receiver,
	const struct qam_point *point,
	uint8_t bits[SIPPYMODEM_V22BIS_BITS_PER_SYMBOL])
{
	struct sippymodem_v22bis_receiver next;
	struct qam_point best_point = { 0 };
	uint8_t decoded[SIPPYMODEM_V22BIS_BITS_PER_SYMBOL];
	uint8_t best_symbol = 0;
	double best_error = DBL_MAX;

	if (!sippymodem_v22bis_receiver_is_valid(receiver) ||
	    !point_power(point, NULL) || bits == NULL)
		return SIPPYMODEM_V22BIS_RECEIVER_INVALID_ARGUMENT;
	next = *receiver;
	for (uint8_t symbol = 0; symbol < SIPPYMODEM_V22BIS_SYMBOL_COUNT;
	     ++symbol) {
		struct qam_point candidate;
		struct qam_point predicted;
		double error;

		if (sippymodem_v22bis_constellation_point(
			    symbol, &candidate) != SIPPYMODEM_V22_FRAMING_OK)
			return SIPPYMODEM_V22BIS_RECEIVER_INVALID_ARGUMENT;
		if (qam_gain_predict(
			    &next.gain, &candidate, &predicted) !=
		    QAM_GAIN_OK)
			return SIPPYMODEM_V22BIS_RECEIVER_INVALID_ARGUMENT;
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
	if (qam_gain_track(&next.gain, point, &best_point) !=
	    QAM_GAIN_OK)
		return SIPPYMODEM_V22BIS_RECEIVER_INVALID_ARGUMENT;
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
