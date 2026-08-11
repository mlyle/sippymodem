// SPDX-License-Identifier: BSD-3-Clause

#include "uncoded_receiver.h"

#include <float.h>
#include <math.h>
#include <stddef.h>

static const double V32_9600_MIN_POINT_POWER = 1e-12;
static const double V32_9600_TRAINING_ADAPTATION = 0.2;
static const double V32_9600_DECISION_ADAPTATION = 0.02;

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

static bool point_is_finite(const struct sippymodem_qam_point *point)
{
	return point != NULL && isfinite(point->in_phase) &&
	       isfinite(point->quadrature);
}

bool sippymodem_v32_9600_receiver_is_valid(
	const struct sippymodem_v32_9600_receiver *receiver)
{
	if (receiver == NULL || !decoder_is_valid(&receiver->decoder) ||
	    !sippymodem_qam_equalizer_is_valid(&receiver->equalizer))
		return false;
	for (size_t index = 0;
	     index < SIPPYMODEM_QAM_EQUALIZER_DELAY + 1u; ++index) {
		if (!point_is_finite(&receiver->known_targets[index]))
			return false;
	}
	return true;
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
	    sippymodem_qam_equalizer_init(
		    &next.equalizer, reference_point, &ideal) !=
		    SIPPYMODEM_QAM_EQUALIZER_OK)
		return SIPPYMODEM_V32_9600_RECEIVER_INVALID_ARGUMENT;
	next.decoder = *decoder;
	if (!sippymodem_v32_9600_receiver_is_valid(&next))
		return SIPPYMODEM_V32_9600_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
	return SIPPYMODEM_V32_9600_RECEIVER_OK;
}

static bool equalize_point(
	struct sippymodem_v32_9600_receiver *receiver,
	const struct sippymodem_qam_point *received,
	const struct sippymodem_qam_point *current_target,
	bool current_target_known, struct sippymodem_qam_point *equalized,
	bool *payload_ready)
{
	bool filter_ready;

	for (size_t index = SIPPYMODEM_QAM_EQUALIZER_DELAY; index != 0;
	     --index) {
		receiver->known_targets[index] =
			receiver->known_targets[index - 1];
		receiver->target_known[index] =
			receiver->target_known[index - 1];
	}
	receiver->known_targets[0] =
		current_target != NULL ? *current_target :
					 (struct sippymodem_qam_point){ 0 };
	receiver->target_known[0] = current_target_known;
	if (sippymodem_qam_equalizer_push(
		    &receiver->equalizer, received, equalized, &filter_ready) !=
	    SIPPYMODEM_QAM_EQUALIZER_OK)
		return false;
	if (!filter_ready) {
		*payload_ready = false;
		return true;
	}
	if (!receiver->target_known[SIPPYMODEM_QAM_EQUALIZER_DELAY]) {
		*payload_ready = true;
		return true;
	}
	if (sippymodem_qam_equalizer_adapt(
		    &receiver->equalizer,
		    &receiver->known_targets[SIPPYMODEM_QAM_EQUALIZER_DELAY],
		    V32_9600_TRAINING_ADAPTATION) !=
	    SIPPYMODEM_QAM_EQUALIZER_OK)
		return false;
	*payload_ready = false;
	return true;
}

enum sippymodem_v32_9600_receiver_result
sippymodem_v32_9600_receiver_decode(
	struct sippymodem_v32_9600_receiver *receiver,
	const struct sippymodem_qam_point *point, uint8_t *output, bool *ready)
{
	struct sippymodem_v32_9600_receiver next;
	struct sippymodem_qam_point best_point = { 0 };
	struct sippymodem_qam_point equalized = { 0 };
	uint8_t best_symbol = 0;
	uint8_t decoded;
	double best_error = DBL_MAX;
	bool payload_ready;

	if (!sippymodem_v32_9600_receiver_is_valid(receiver) ||
	    !point_has_nonzero_power(point) || output == NULL || ready == NULL)
		return SIPPYMODEM_V32_9600_RECEIVER_INVALID_ARGUMENT;
	next = *receiver;
	if (!equalize_point(
		    &next, point, NULL, false, &equalized, &payload_ready))
		return SIPPYMODEM_V32_9600_RECEIVER_INVALID_ARGUMENT;
	if (!payload_ready) {
		*receiver = next;
		*ready = false;
		return SIPPYMODEM_V32_9600_RECEIVER_OK;
	}
	for (uint8_t symbol = 0;
	     symbol < SIPPYMODEM_V32_9600_SYMBOL_COUNT; ++symbol) {
		struct sippymodem_qam_point candidate;
		double error;

		if (sippymodem_v32_constellation_point(symbol, &candidate) !=
		    SIPPYMODEM_V32_CONSTELLATION_OK)
			return SIPPYMODEM_V32_9600_RECEIVER_INVALID_ARGUMENT;
		error = squared_error(&equalized, &candidate);
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
	if (sippymodem_qam_equalizer_adapt(
		    &next.equalizer, &best_point, V32_9600_DECISION_ADAPTATION) !=
	    SIPPYMODEM_QAM_EQUALIZER_OK)
		return SIPPYMODEM_V32_9600_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
	*output = decoded;
	*ready = true;
	return SIPPYMODEM_V32_9600_RECEIVER_OK;
}

enum sippymodem_v32_9600_receiver_result
sippymodem_v32_9600_receiver_track_known(
	struct sippymodem_v32_9600_receiver *receiver,
	const struct sippymodem_qam_point *point, uint8_t input)
{
	struct sippymodem_v32_9600_receiver next;
	struct sippymodem_v32_symbol_encoder encoder;
	struct sippymodem_qam_point equalized;
	struct sippymodem_qam_point transmitted;
	uint8_t symbol;
	bool payload_ready;

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
	if (!equalize_point(
		    &next, point, &transmitted, true, &equalized,
		    &payload_ready))
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
