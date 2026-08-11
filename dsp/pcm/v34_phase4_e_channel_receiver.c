// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase4_e_channel_receiver.h"
#include "v34_training_channel.h"

#include <stddef.h>

bool v34_phase4_e_channel_receiver_is_valid(
	const struct v34_phase4_e_channel_receiver *receiver)
{
	return receiver != NULL &&
	       v34_phase4_e_receiver_is_valid(
		       &receiver->symbol_receiver) &&
	       sippymodem_qam_gain_is_valid(&receiver->gain);
}

enum v34_phase4_e_receiver_result
v34_phase4_e_channel_receiver_init_from_mp(
	struct v34_phase4_e_channel_receiver *receiver,
	const struct v34_phase4_mp_channel_receiver *mp_receiver)
{
	struct v34_phase4_e_channel_receiver initialized = { 0 };

	if (receiver == NULL ||
	    !v34_phase4_mp_channel_receiver_is_valid(mp_receiver) ||
	    v34_phase4_e_receiver_init_from_mp(
		    &initialized.symbol_receiver,
		    &mp_receiver->symbol_receiver) !=
		    V34_PHASE4_E_RECEIVER_OK)
		return V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT;
	initialized.gain = mp_receiver->gain;
	if (!v34_phase4_e_channel_receiver_is_valid(&initialized))
		return V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return V34_PHASE4_E_RECEIVER_OK;
}

enum v34_phase4_e_receiver_result
v34_phase4_e_channel_receive_point(
	struct v34_phase4_e_channel_receiver *receiver,
	const struct sippymodem_qam_point *received)
{
	struct v34_phase4_e_channel_receiver next;
	enum v34_phase4_e_receiver_result result;
	uint8_t quarter_point;
	uint8_t clockwise_quarter_turns;

	if (!v34_phase4_e_channel_receiver_is_valid(receiver) ||
	    received == NULL)
		return V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT;
	if (v34_phase4_e_receiver_is_complete(
		    &receiver->symbol_receiver))
		return V34_PHASE4_E_RECEIVER_COMPLETE;
	next = *receiver;
	if (v34_training_channel_decide(
		    &next.gain, next.symbol_receiver.constellation, received,
		    &quarter_point, &clockwise_quarter_turns) !=
	    V34_TRAINING_CHANNEL_OK)
		return V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT;
	result = v34_phase4_e_receive_decision(
		&next.symbol_receiver, quarter_point,
		clockwise_quarter_turns);
	if (result != V34_PHASE4_E_RECEIVER_OK &&
	    result != V34_PHASE4_E_RECEIVER_COMPLETE)
		return result;
	if (!v34_phase4_e_channel_receiver_is_valid(&next))
		return V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
	return result;
}
