// SPDX-License-Identifier: BSD-3-Clause

#include "v34_data_equalizer_receiver.h"

#include <math.h>
#include <stddef.h>

static bool point_is_finite(const struct sippymodem_qam_point *point)
{
	return point != NULL && isfinite(point->in_phase) &&
	       isfinite(point->quadrature);
}

bool sippymodem_v34_data_equalizer_receiver_is_valid(
	const struct sippymodem_v34_data_equalizer_receiver *receiver)
{
	bool observation_pending;

	if (receiver == NULL ||
	    !sippymodem_qam_equalizer_is_valid(&receiver->equalizer) ||
	    !sippymodem_v34_data_channel_receiver_is_valid(
		    &receiver->data_receiver) ||
	    !point_is_finite(&receiver->first_symbol) ||
	    !isfinite(receiver->adaptation) || receiver->adaptation < 0.0 ||
	    receiver->adaptation > 1.0 ||
	    receiver->training_outputs_remaining >
		    SIPPYMODEM_QAM_EQUALIZER_DELAY ||
	    (receiver->training_outputs_remaining != 0 &&
	     receiver->has_first_symbol))
		return false;
	if (receiver->has_first_symbol != receiver->first_observation.pending)
		return false;
	if (!receiver->has_first_symbol)
		return true;
	observation_pending =
		sippymodem_qam_equalizer_observation_is_pending(
			&receiver->first_observation);
	return observation_pending &&
	       receiver->first_symbol.in_phase ==
		       receiver->first_observation.output.in_phase &&
	       receiver->first_symbol.quadrature ==
		       receiver->first_observation.output.quadrature;
}

enum sippymodem_v34_data_equalizer_receiver_result
sippymodem_v34_data_equalizer_receiver_init(
	struct sippymodem_v34_data_equalizer_receiver *receiver,
	const struct sippymodem_qam_equalizer *equalizer,
	const struct sippymodem_v34_power_normalizer *normalizer,
	const struct sippymodem_v34_frame_geometry *geometry,
	bool expanded_shaping,
	const struct sippymodem_v34_q14_coefficient
		coefficients[SIPPYMODEM_V34_PRECODER_TAP_COUNT],
	enum sippymodem_v34_trellis_selection selection,
	enum sippymodem_v34_station_role transmitting_role,
	double adaptation)
{
	struct sippymodem_v34_data_equalizer_receiver initialized = { 0 };

	if (receiver == NULL || !isfinite(adaptation) || adaptation < 0.0 ||
	    adaptation > 1.0 || !sippymodem_qam_equalizer_is_valid(equalizer) ||
	    sippymodem_v34_data_channel_receiver_init(
		    &initialized.data_receiver, normalizer, geometry,
		    expanded_shaping, coefficients, selection,
		    transmitting_role) !=
		    SIPPYMODEM_V34_DATA_CHANNEL_RECEIVER_OK)
		return SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_INVALID_ARGUMENT;
	initialized.equalizer = *equalizer;
	/* The data owner deliberately closes any immediate Phase 4 update. */
	initialized.equalizer.can_adapt = false;
	initialized.adaptation = adaptation;
	initialized.training_outputs_remaining =
		equalizer->fill < SIPPYMODEM_QAM_EQUALIZER_DELAY ?
			(uint8_t)equalizer->fill :
			SIPPYMODEM_QAM_EQUALIZER_DELAY;
	if (!sippymodem_v34_data_equalizer_receiver_is_valid(&initialized))
		return SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_OK;
}

bool sippymodem_v34_data_equalizer_receiver_is_receiving_b1(
	const struct sippymodem_v34_data_equalizer_receiver *receiver)
{
	return sippymodem_v34_data_equalizer_receiver_is_valid(receiver) &&
	       sippymodem_v34_data_channel_receiver_is_receiving_b1(
		       &receiver->data_receiver);
}

static enum sippymodem_v34_data_equalizer_receiver_result channel_result(
	enum sippymodem_v34_data_channel_receiver_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_DATA_CHANNEL_RECEIVER_OK:
		return SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_INTERVAL_READY;
	case SIPPYMODEM_V34_DATA_CHANNEL_RECEIVER_MAPPING_FRAME_READY:
		return SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_MAPPING_FRAME_READY;
	case SIPPYMODEM_V34_DATA_CHANNEL_RECEIVER_B1_IN_PROGRESS:
		return SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_B1_IN_PROGRESS;
	case SIPPYMODEM_V34_DATA_CHANNEL_RECEIVER_B1_COMPLETE:
		return SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_B1_COMPLETE;
	case SIPPYMODEM_V34_DATA_CHANNEL_RECEIVER_NUMERIC_FAILURE:
		return SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_NUMERIC_FAILURE;
	case SIPPYMODEM_V34_DATA_CHANNEL_RECEIVER_OUT_OF_RANGE:
		return SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_OUT_OF_RANGE;
	case SIPPYMODEM_V34_DATA_CHANNEL_RECEIVER_INVALID_ARGUMENT:
		break;
	}
	return SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_INVALID_ARGUMENT;
}

static bool emits_mapping_frame(
	enum sippymodem_v34_data_equalizer_receiver_result result)
{
	return result ==
		       SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_MAPPING_FRAME_READY ||
	       result ==
		       SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_B1_IN_PROGRESS ||
	       result == SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_B1_COMPLETE;
}

static enum sippymodem_v34_data_equalizer_receiver_result
receive_interval(
	struct sippymodem_v34_data_equalizer_receiver *receiver,
	const struct sippymodem_qam_equalizer *equalizer,
	struct sippymodem_qam_equalizer_observation *second_observation,
	struct sippymodem_v34_survivor_decision *current,
	struct sippymodem_v34_received_mapping_frame *received)
{
	struct sippymodem_v34_data_equalizer_receiver next;
	struct sippymodem_qam_point normalized[
		SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL];
	struct sippymodem_qam_point decision_targets[
		SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL];
	struct sippymodem_v34_survivor_decision generated_current;
	struct sippymodem_v34_received_mapping_frame generated_received;
	enum sippymodem_v34_data_equalizer_receiver_result emitted;
	enum sippymodem_v34_data_channel_receiver_result channel_status;

	next = *receiver;
	next.equalizer = *equalizer;
	normalized[0] = next.first_symbol;
	normalized[1] = second_observation->output;
	channel_status = sippymodem_v34_data_channel_receiver_advance(
		&next.data_receiver, normalized, &generated_current,
		decision_targets, &generated_received);
	emitted = channel_result(channel_status);
	if (emitted == SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_INVALID_ARGUMENT ||
	    emitted == SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_NUMERIC_FAILURE ||
	    emitted == SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_OUT_OF_RANGE)
		return emitted;
	if (sippymodem_qam_equalizer_adapt_observation(
		    &next.equalizer, &next.first_observation,
		    &decision_targets[0], next.adaptation) !=
		    SIPPYMODEM_QAM_EQUALIZER_OK ||
	    sippymodem_qam_equalizer_adapt_observation(
		    &next.equalizer, second_observation, &decision_targets[1],
		    next.adaptation) != SIPPYMODEM_QAM_EQUALIZER_OK)
		return SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_NUMERIC_FAILURE;
	next.first_observation =
		(struct sippymodem_qam_equalizer_observation){ 0 };
	next.first_symbol = (struct sippymodem_qam_point){ 0 };
	next.has_first_symbol = false;
	if (!sippymodem_v34_data_equalizer_receiver_is_valid(&next))
		return SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
	*current = generated_current;
	if (emits_mapping_frame(emitted))
		*received = generated_received;
	return emitted;
}

enum sippymodem_v34_data_equalizer_receiver_result
sippymodem_v34_data_equalizer_receive_point(
	struct sippymodem_v34_data_equalizer_receiver *receiver,
	const struct sippymodem_qam_point *received_point,
	struct sippymodem_v34_survivor_decision *current,
	struct sippymodem_v34_received_mapping_frame *received)
{
	struct sippymodem_qam_equalizer next_equalizer;
	struct sippymodem_qam_equalizer_observation observation = { 0 };
	bool equalized_ready;

	if (!sippymodem_v34_data_equalizer_receiver_is_valid(receiver) ||
	    !point_is_finite(received_point) || current == NULL ||
	    received == NULL)
		return SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_INVALID_ARGUMENT;
	next_equalizer = receiver->equalizer;
	if (sippymodem_qam_equalizer_observe(
		    &next_equalizer, received_point, &observation,
		    &equalized_ready) != SIPPYMODEM_QAM_EQUALIZER_OK)
		return SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_NUMERIC_FAILURE;
	if (!equalized_ready) {
		if (receiver->has_first_symbol)
			return SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_INVALID_ARGUMENT;
		receiver->equalizer = next_equalizer;
		return SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_OK;
	}
	if (receiver->training_outputs_remaining != 0) {
		receiver->equalizer = next_equalizer;
		receiver->training_outputs_remaining--;
		return SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_OK;
	}
	if (!receiver->has_first_symbol) {
		receiver->equalizer = next_equalizer;
		receiver->first_symbol = observation.output;
		receiver->first_observation = observation;
		receiver->has_first_symbol = true;
		return SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_OK;
	}
	return receive_interval(
		receiver, &next_equalizer, &observation, current, received);
}

const char *sippymodem_v34_data_equalizer_receiver_result_name(
	enum sippymodem_v34_data_equalizer_receiver_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_OK:
		return "ok";
	case SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_INTERVAL_READY:
		return "interval ready";
	case SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_MAPPING_FRAME_READY:
		return "mapping frame ready";
	case SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_B1_IN_PROGRESS:
		return "B1 in progress";
	case SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_B1_COMPLETE:
		return "B1 complete";
	case SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_NUMERIC_FAILURE:
		return "numeric failure";
	case SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_OUT_OF_RANGE:
		return "out of range";
	}
	return "unknown";
}
