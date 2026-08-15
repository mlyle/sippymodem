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

bool v22bis_receiver_is_valid(
	const struct v22bis_receiver *receiver)
{
	return receiver != NULL &&
	       receiver->decoder.phase_quadrant <
		       QPSK_QUADRANT_COUNT &&
	       v22_scrambler_is_valid(
		       &receiver->decoder.descrambler) &&
	       qam_gain_is_valid(&receiver->gain);
}

static double squared_error(
	const struct qam_point *left,
	const struct qam_point *right)
{
	double in_phase = left->in_phase - right->in_phase;
	double quadrature = left->quadrature - right->quadrature;

	return in_phase * in_phase + quadrature * quadrature;
}

enum v22bis_receiver_result v22bis_receiver_init(
	struct v22bis_receiver *receiver,
	const struct qam_point *reference_point)
{
	struct v22bis_receiver next = { 0 };
	struct qam_point ideal;

	if (receiver == NULL ||
	    v22bis_1200_constellation_point(0, &ideal) !=
		    V22_FRAMING_OK ||
	    qam_gain_init_from_reference(
		    &next.gain, reference_point, &ideal,
		    V22BIS_CHANNEL_ADAPTATION) != QAM_GAIN_OK ||
	    v22bis_symbol_decoder_init(&next.decoder, 0) !=
		    V22_FRAMING_OK)
		return V22BIS_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
	return V22BIS_RECEIVER_OK;
}

enum v22bis_receiver_result
v22bis_receiver_matches_v22(
	const struct v22bis_receiver *receiver,
	const struct qam_point *point, bool *matches)
{
	double gain_power;
	double best_error = DBL_MAX;

	if (!v22bis_receiver_is_valid(receiver) ||
	    receiver->v22bis_started || !point_power(point, NULL) ||
	    matches == NULL ||
	    !point_power(&receiver->gain.channel_gain, &gain_power))
		return V22BIS_RECEIVER_INVALID_ARGUMENT;
	for (uint8_t quadrant = 0;
	     quadrant < QPSK_QUADRANT_COUNT; ++quadrant) {
		struct qam_point ideal;
		struct qam_point predicted;
		double error;

		if (v22bis_1200_constellation_point(quadrant, &ideal) !=
			    V22_FRAMING_OK ||
		    qam_gain_predict(
			    &receiver->gain, &ideal, &predicted) !=
		    QAM_GAIN_OK)
			return V22BIS_RECEIVER_INVALID_ARGUMENT;
		error = squared_error(point, &predicted);

		if (error < best_error)
			best_error = error;
	}
	*matches = best_error <=
		   gain_power * V22BIS_V22_REFERENCE_ERROR_RATIO;
	return V22BIS_RECEIVER_OK;
}

enum v22bis_receiver_result
v22bis_receiver_track_v22(
	struct v22bis_receiver *receiver, uint8_t quarter_turns,
	const struct qam_point *point)
{
	struct v22bis_receiver next;
	struct qam_point ideal;

	if (!v22bis_receiver_is_valid(receiver) ||
	    receiver->v22bis_started ||
	    quarter_turns >= QPSK_QUADRANT_COUNT ||
	    !point_power(point, NULL))
		return V22BIS_RECEIVER_INVALID_ARGUMENT;
	next = *receiver;
	next.decoder.phase_quadrant =
		(uint8_t)((next.decoder.phase_quadrant + quarter_turns) & 3u);
	if (v22bis_1200_constellation_point(
		    next.decoder.phase_quadrant, &ideal) != V22_FRAMING_OK)
		return V22BIS_RECEIVER_INVALID_ARGUMENT;
	if (qam_gain_init_from_reference(
		    &next.gain, point, &ideal, V22BIS_CHANNEL_ADAPTATION) !=
	    QAM_GAIN_OK)
		return V22BIS_RECEIVER_INVALID_ARGUMENT;
	if (!v22bis_receiver_is_valid(&next))
		return V22BIS_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
	return V22BIS_RECEIVER_OK;
}

enum v22bis_receiver_result v22bis_receiver_decode(
	struct v22bis_receiver *receiver,
	const struct qam_point *point,
	uint8_t bits[V22BIS_BITS_PER_SYMBOL])
{
	struct v22bis_receiver next;
	struct qam_point best_point = { 0 };
	uint8_t decoded[V22BIS_BITS_PER_SYMBOL];
	uint8_t best_symbol = 0;
	double best_error = DBL_MAX;

	if (!v22bis_receiver_is_valid(receiver) ||
	    !point_power(point, NULL) || bits == NULL)
		return V22BIS_RECEIVER_INVALID_ARGUMENT;
	next = *receiver;
	for (uint8_t symbol = 0; symbol < V22BIS_SYMBOL_COUNT;
	     ++symbol) {
		struct qam_point candidate;
		struct qam_point predicted;
		double error;

		if (v22bis_constellation_point(
			    symbol, &candidate) != V22_FRAMING_OK)
			return V22BIS_RECEIVER_INVALID_ARGUMENT;
		if (qam_gain_predict(
			    &next.gain, &candidate, &predicted) !=
		    QAM_GAIN_OK)
			return V22BIS_RECEIVER_INVALID_ARGUMENT;
		error = squared_error(point, &predicted);
		if (isfinite(error) && error < best_error) {
			best_error = error;
			best_symbol = symbol;
			best_point = candidate;
		}
	}
	if (best_error == DBL_MAX ||
	    v22bis_symbol_decode(
		    &next.decoder, best_symbol, decoded) !=
		    V22_FRAMING_OK)
		return V22BIS_RECEIVER_INVALID_ARGUMENT;
	next.v22bis_started = true;
	if (qam_gain_track(&next.gain, point, &best_point) !=
	    QAM_GAIN_OK)
		return V22BIS_RECEIVER_INVALID_ARGUMENT;
	if (!v22bis_receiver_is_valid(&next))
		return V22BIS_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
	for (size_t index = 0; index < V22BIS_BITS_PER_SYMBOL;
	     ++index)
		bits[index] = decoded[index];
	return V22BIS_RECEIVER_OK;
}

const char *v22bis_receiver_result_name(
	enum v22bis_receiver_result result)
{
	switch (result) {
	case V22BIS_RECEIVER_OK:
		return "ok";
	case V22BIS_RECEIVER_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
