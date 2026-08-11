// SPDX-License-Identifier: BSD-3-Clause

#include "trellis_receiver.h"

#include "constellation.h"

#include <math.h>
#include <stddef.h>

/* The post-E confirmation contributes only 128 known symbols. */
static const double V32_TRELLIS_TRAINING_ADAPTATION = 0.5;
/* Follow residual carrier rotation without chasing individual noisy points. */
static const double V32_TRELLIS_DECISION_ADAPTATION = 0.05;
static const double V32_TRELLIS_MINIMUM_POINT_POWER = 1e-12;

static bool point_is_finite(const struct qam_point *point)
{
	return point != NULL && isfinite(point->in_phase) &&
	       isfinite(point->quadrature);
}

static bool point_has_power(const struct qam_point *point)
{
	double power;

	if (!point_is_finite(point))
		return false;
	power = point->in_phase * point->in_phase +
		point->quadrature * point->quadrature;
	return isfinite(power) && power > V32_TRELLIS_MINIMUM_POINT_POWER;
}

static bool framing_is_valid(
	const struct sippymodem_v32_symbol_decoder *framing)
{
	return framing != NULL &&
	       framing->phase_quadrant < QPSK_QUADRANT_COUNT &&
	       sippymodem_v32_scrambler_is_valid(&framing->descrambler);
}

bool sippymodem_v32_trellis_receiver_is_valid(
	const struct sippymodem_v32_trellis_receiver *receiver)
{
	uint8_t data_bits;
	uint8_t input_limit;

	if (receiver == NULL ||
	    !qam_equalizer_is_valid(&receiver->equalizer) ||
	    !sippymodem_v32_trellis_encoder_is_valid(
		    &receiver->known_encoder) ||
	    !sippymodem_v32_trellis_decoder_is_valid(&receiver->decoder) ||
	    !sippymodem_v32_scrambler_is_valid(&receiver->known_scrambler) ||
	    !sippymodem_v32_scrambler_is_valid(&receiver->descrambler) ||
	    receiver->known_input_fill >
		    SIPPYMODEM_V32_TRELLIS_RECEIVER_DELAY ||
	    receiver->payload_symbols >
		    SIPPYMODEM_V32_TRELLIS_TRACEBACK_SYMBOLS)
		return false;
	data_bits = receiver->decoder.data_bits_per_symbol;
	if (receiver->known_encoder.data_bits_per_symbol != data_bits ||
	    receiver->known_scrambler.transmitter_role !=
		    receiver->descrambler.transmitter_role)
		return false;
	input_limit = (uint8_t)(UINT8_C(1) << data_bits);
	for (size_t index = 0;
	     index < SIPPYMODEM_V32_TRELLIS_RECEIVER_DELAY; ++index) {
		if (receiver->known_inputs[index] >= input_limit)
			return false;
	}
	for (size_t index = 0;
	     index < QAM_EQUALIZER_DELAY + 1u; ++index) {
		if (!point_is_finite(&receiver->known_targets[index]))
			return false;
	}
	return true;
}

enum sippymodem_v32_trellis_receiver_result
sippymodem_v32_trellis_receiver_init(
	struct sippymodem_v32_trellis_receiver *receiver,
	const struct sippymodem_v32_symbol_decoder *framing,
	const struct qam_point *reference_point,
	uint8_t reference_phase_quadrant, uint8_t data_bits_per_symbol)
{
	struct sippymodem_v32_trellis_receiver initialized = { 0 };
	struct qam_point ideal;
	uint8_t previous_differential_output;

	if (receiver == NULL || !framing_is_valid(framing) ||
	    sippymodem_v32_4800_constellation_point(
		    reference_phase_quadrant, &ideal) !=
		    SIPPYMODEM_V32_CONSTELLATION_OK ||
	    sippymodem_v32_phase_to_differential_output(
		    reference_phase_quadrant,
		    &previous_differential_output) != SIPPYMODEM_V32_FRAMING_OK ||
	    qam_equalizer_init(
		    &initialized.equalizer, reference_point, &ideal) !=
		    QAM_EQUALIZER_OK ||
	    sippymodem_v32_trellis_encoder_init(
		    &initialized.known_encoder, data_bits_per_symbol,
		    previous_differential_output) !=
		    SIPPYMODEM_V32_TRELLIS_OK ||
	    sippymodem_v32_trellis_decoder_init(
		    &initialized.decoder, data_bits_per_symbol,
		    previous_differential_output) !=
		    SIPPYMODEM_V32_TRELLIS_OK)
		return SIPPYMODEM_V32_TRELLIS_RECEIVER_INVALID_ARGUMENT;
	initialized.known_scrambler = framing->descrambler;
	initialized.descrambler = framing->descrambler;
	if (!sippymodem_v32_trellis_receiver_is_valid(&initialized))
		return SIPPYMODEM_V32_TRELLIS_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return SIPPYMODEM_V32_TRELLIS_RECEIVER_OK;
}

static bool push_equalized(
	struct sippymodem_v32_trellis_receiver *receiver,
	const struct qam_point *received,
	const struct qam_point *current_target,
	bool current_target_known, struct qam_point *equalized,
	bool *ready, bool *delayed_target_known)
{
	for (size_t index = QAM_EQUALIZER_DELAY; index != 0;
	     --index) {
		receiver->known_targets[index] =
			receiver->known_targets[index - 1];
		receiver->target_known[index] =
			receiver->target_known[index - 1];
	}
	receiver->known_targets[0] =
		current_target != NULL ? *current_target :
					 (struct qam_point){ 0 };
	receiver->target_known[0] = current_target_known;
	if (qam_equalizer_push(
		    &receiver->equalizer, received, equalized, ready) !=
	    QAM_EQUALIZER_OK)
		return false;
	*delayed_target_known =
		*ready &&
		receiver->target_known[QAM_EQUALIZER_DELAY];
	if (*delayed_target_known &&
	    qam_equalizer_adapt(
		    &receiver->equalizer,
		    &receiver
			     ->known_targets[QAM_EQUALIZER_DELAY],
		    V32_TRELLIS_TRAINING_ADAPTATION) !=
		    QAM_EQUALIZER_OK)
		return false;
	return true;
}

static void remember_known_input(
	struct sippymodem_v32_trellis_receiver *receiver, uint8_t input)
{
	for (size_t index = SIPPYMODEM_V32_TRELLIS_RECEIVER_DELAY - 1u;
	     index != 0; --index)
		receiver->known_inputs[index] =
			receiver->known_inputs[index - 1];
	receiver->known_inputs[0] = input;
	if (receiver->known_input_fill <
	    SIPPYMODEM_V32_TRELLIS_RECEIVER_DELAY)
		receiver->known_input_fill++;
}

enum sippymodem_v32_trellis_receiver_result
sippymodem_v32_trellis_receiver_track_known(
	struct sippymodem_v32_trellis_receiver *receiver,
	const struct qam_point *point, uint8_t input,
	bool *confirmed)
{
	struct sippymodem_v32_trellis_receiver next;
	struct qam_point equalized = { 0 };
	struct qam_point transmitted;
	uint8_t data_bits;
	uint8_t plain = 0;
	uint8_t scrambled;
	uint8_t decoded = 0;
	uint8_t decided_signal = 0;
	uint8_t signal_symbol;
	bool equalizer_ready;
	bool delayed_target_known;
	bool trellis_ready = false;

	if (!sippymodem_v32_trellis_receiver_is_valid(receiver) ||
	    !point_has_power(point) || confirmed == NULL ||
	    receiver->payload_started)
		return SIPPYMODEM_V32_TRELLIS_RECEIVER_INVALID_ARGUMENT;
	data_bits = receiver->decoder.data_bits_per_symbol;
	if (input >= (UINT8_C(1) << data_bits))
		return SIPPYMODEM_V32_TRELLIS_RECEIVER_INVALID_ARGUMENT;
	next = *receiver;
	if (sippymodem_v32_scramble_group(
		    &next.known_scrambler, input, data_bits, &scrambled) !=
		    SIPPYMODEM_V32_FRAMING_OK ||
	    sippymodem_v32_trellis_encode(
		    &next.known_encoder, scrambled, &signal_symbol) !=
		    SIPPYMODEM_V32_TRELLIS_OK ||
	    sippymodem_v32_trellis_constellation_point(
		    data_bits, signal_symbol, &transmitted) !=
		    SIPPYMODEM_V32_CONSTELLATION_OK)
		return SIPPYMODEM_V32_TRELLIS_RECEIVER_INVALID_ARGUMENT;
	remember_known_input(&next, input);
	if (!push_equalized(
		    &next, point, &transmitted, true, &equalized,
		    &equalizer_ready, &delayed_target_known) ||
	    (equalizer_ready &&
	     sippymodem_v32_trellis_decode(
		     &next.decoder, &equalized, &decoded, &trellis_ready,
		     &decided_signal) !=
		     SIPPYMODEM_V32_TRELLIS_OK) ||
	    (trellis_ready &&
	     sippymodem_v32_descramble_group(
		     &next.descrambler, decoded, data_bits, &plain) !=
		     SIPPYMODEM_V32_FRAMING_OK) ||
	    !sippymodem_v32_trellis_receiver_is_valid(&next))
		return SIPPYMODEM_V32_TRELLIS_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
	*confirmed =
		trellis_ready && delayed_target_known &&
		next.known_input_fill == SIPPYMODEM_V32_TRELLIS_RECEIVER_DELAY &&
		plain == next.known_inputs[
			 SIPPYMODEM_V32_TRELLIS_RECEIVER_DELAY - 1u];
	return SIPPYMODEM_V32_TRELLIS_RECEIVER_OK;
}

enum sippymodem_v32_trellis_receiver_result
sippymodem_v32_trellis_receiver_decode(
	struct sippymodem_v32_trellis_receiver *receiver,
	const struct qam_point *point, uint8_t *output, bool *ready)
{
	struct sippymodem_v32_trellis_receiver next;
	struct qam_point equalized = { 0 };
	uint8_t plain = 0;
	uint8_t scrambled = 0;
	uint8_t decided_signal = 0;
	bool equalizer_ready;
	bool delayed_target_known;
	bool trellis_ready = false;
	bool payload_ready;

	if (!sippymodem_v32_trellis_receiver_is_valid(receiver) ||
	    !point_has_power(point) || output == NULL || ready == NULL)
		return SIPPYMODEM_V32_TRELLIS_RECEIVER_INVALID_ARGUMENT;
	next = *receiver;
	next.payload_started = true;
	if (!push_equalized(
		    &next, point, NULL, false, &equalized, &equalizer_ready,
		    &delayed_target_known) ||
	    (equalizer_ready &&
	     sippymodem_v32_trellis_decode(
		     &next.decoder, &equalized, &scrambled, &trellis_ready,
		     &decided_signal) !=
		     SIPPYMODEM_V32_TRELLIS_OK))
		return SIPPYMODEM_V32_TRELLIS_RECEIVER_INVALID_ARGUMENT;
	if (equalizer_ready && !delayed_target_known) {
		struct qam_point decided_point;

		/* The best current survivor belongs to this equalizer output;
		 * traceback data belongs to an older point. */
		if (sippymodem_v32_trellis_constellation_point(
			    next.decoder.data_bits_per_symbol, decided_signal,
			    &decided_point) != SIPPYMODEM_V32_CONSTELLATION_OK ||
		    qam_equalizer_adapt(
			    &next.equalizer, &decided_point,
			    V32_TRELLIS_DECISION_ADAPTATION) !=
			    QAM_EQUALIZER_OK)
			return SIPPYMODEM_V32_TRELLIS_RECEIVER_INVALID_ARGUMENT;
	}
	if (equalizer_ready && !delayed_target_known &&
	    next.payload_symbols < SIPPYMODEM_V32_TRELLIS_TRACEBACK_SYMBOLS)
		next.payload_symbols++;
	payload_ready =
		trellis_ready && !delayed_target_known &&
		next.payload_symbols == SIPPYMODEM_V32_TRELLIS_TRACEBACK_SYMBOLS;
	if ((trellis_ready &&
	     sippymodem_v32_descramble_group(
		     &next.descrambler, scrambled,
		     next.decoder.data_bits_per_symbol, &plain) !=
		     SIPPYMODEM_V32_FRAMING_OK) ||
	    !sippymodem_v32_trellis_receiver_is_valid(&next))
		return SIPPYMODEM_V32_TRELLIS_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
	*ready = payload_ready;
	if (payload_ready)
		*output = plain;
	return SIPPYMODEM_V32_TRELLIS_RECEIVER_OK;
}

const char *sippymodem_v32_trellis_receiver_result_name(
	enum sippymodem_v32_trellis_receiver_result result)
{
	switch (result) {
	case SIPPYMODEM_V32_TRELLIS_RECEIVER_OK:
		return "ok";
	case SIPPYMODEM_V32_TRELLIS_RECEIVER_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
