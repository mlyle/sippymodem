// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase4_mp_channel_receiver.h"
#include "v34_training_channel.h"

#include <stddef.h>

static const double V34_PHASE4_CHANNEL_ADAPTATION = 1.0 / 32.0;

bool v34_phase4_mp_channel_receiver_is_valid(
	const struct v34_phase4_mp_channel_receiver *receiver)
{
	return receiver != NULL &&
	       v34_phase4_mp_receiver_is_valid(
		       &receiver->symbol_receiver) &&
	       sippymodem_qam_gain_is_valid(&receiver->gain);
}

enum v34_phase4_mp_receiver_result
v34_phase4_mp_channel_receiver_init(
	struct v34_phase4_mp_channel_receiver *receiver,
	enum v34_station_role transmitting_role,
	enum v34_constellation_size constellation,
	const struct sippymodem_qam_point *received_reference,
	const struct sippymodem_qam_point *transmitted_reference)
{
	struct v34_phase4_mp_channel_receiver initialized = { 0 };

	if (receiver == NULL ||
	    sippymodem_qam_gain_init_from_reference(
		    &initialized.gain, received_reference,
		    transmitted_reference, V34_PHASE4_CHANNEL_ADAPTATION) !=
		    SIPPYMODEM_QAM_GAIN_OK ||
	    v34_phase4_mp_receiver_init(
		    &initialized.symbol_receiver, transmitting_role,
		    constellation) != V34_PHASE4_MP_RECEIVER_OK ||
	    !v34_phase4_mp_channel_receiver_is_valid(&initialized))
		return V34_PHASE4_MP_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return V34_PHASE4_MP_RECEIVER_OK;
}

enum v34_phase4_mp_receiver_result
v34_phase4_mp_channel_receive_point(
	struct v34_phase4_mp_channel_receiver *receiver,
	const struct sippymodem_qam_point *received,
	struct v34_mp_receive_event *event)
{
	struct v34_phase4_mp_channel_receiver next;
	struct v34_mp_receive_event emitted;
	uint8_t quarter_point;
	uint8_t clockwise_quarter_turns;

	if (!v34_phase4_mp_channel_receiver_is_valid(receiver) ||
	    event == NULL)
		return V34_PHASE4_MP_RECEIVER_INVALID_ARGUMENT;
	next = *receiver;
	if (v34_training_channel_decide(
		    &next.gain, next.symbol_receiver.constellation, received,
		    &quarter_point, &clockwise_quarter_turns) !=
		    V34_TRAINING_CHANNEL_OK ||
	    v34_phase4_mp_receive_decision(
		    &next.symbol_receiver, quarter_point,
		    clockwise_quarter_turns, &emitted) !=
		    V34_PHASE4_MP_RECEIVER_OK ||
	    !v34_phase4_mp_channel_receiver_is_valid(&next))
		return V34_PHASE4_MP_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
	*event = emitted;
	return V34_PHASE4_MP_RECEIVER_OK;
}
