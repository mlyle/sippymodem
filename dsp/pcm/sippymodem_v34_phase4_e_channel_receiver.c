// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v34_phase4_e_channel_receiver.h"
#include "sippymodem_v34_training_channel.h"

#include <stddef.h>

bool sippymodem_v34_phase4_e_channel_receiver_is_valid(
	const struct sippymodem_v34_phase4_e_channel_receiver *receiver)
{
	return receiver != NULL &&
	       sippymodem_v34_phase4_e_receiver_is_valid(
		       &receiver->symbol_receiver) &&
	       sippymodem_qam_gain_is_valid(&receiver->gain);
}

enum sippymodem_v34_phase4_e_receiver_result
sippymodem_v34_phase4_e_channel_receiver_init_from_mp(
	struct sippymodem_v34_phase4_e_channel_receiver *receiver,
	const struct sippymodem_v34_phase4_mp_channel_receiver *mp_receiver)
{
	struct sippymodem_v34_phase4_e_channel_receiver initialized = { 0 };

	if (receiver == NULL ||
	    !sippymodem_v34_phase4_mp_channel_receiver_is_valid(mp_receiver) ||
	    sippymodem_v34_phase4_e_receiver_init_from_mp(
		    &initialized.symbol_receiver,
		    &mp_receiver->symbol_receiver) !=
		    SIPPYMODEM_V34_PHASE4_E_RECEIVER_OK)
		return SIPPYMODEM_V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT;
	initialized.gain = mp_receiver->gain;
	if (!sippymodem_v34_phase4_e_channel_receiver_is_valid(&initialized))
		return SIPPYMODEM_V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return SIPPYMODEM_V34_PHASE4_E_RECEIVER_OK;
}

enum sippymodem_v34_phase4_e_receiver_result
sippymodem_v34_phase4_e_channel_receive_point(
	struct sippymodem_v34_phase4_e_channel_receiver *receiver,
	const struct sippymodem_qam_point *received)
{
	struct sippymodem_v34_phase4_e_channel_receiver next;
	enum sippymodem_v34_phase4_e_receiver_result result;
	uint8_t quarter_point;
	uint8_t clockwise_quarter_turns;

	if (!sippymodem_v34_phase4_e_channel_receiver_is_valid(receiver) ||
	    received == NULL)
		return SIPPYMODEM_V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT;
	if (sippymodem_v34_phase4_e_receiver_is_complete(
		    &receiver->symbol_receiver))
		return SIPPYMODEM_V34_PHASE4_E_RECEIVER_COMPLETE;
	next = *receiver;
	if (sippymodem_v34_training_channel_decide(
		    &next.gain, next.symbol_receiver.constellation, received,
		    &quarter_point, &clockwise_quarter_turns) !=
	    SIPPYMODEM_V34_TRAINING_CHANNEL_OK)
		return SIPPYMODEM_V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT;
	result = sippymodem_v34_phase4_e_receive_decision(
		&next.symbol_receiver, quarter_point,
		clockwise_quarter_turns);
	if (result != SIPPYMODEM_V34_PHASE4_E_RECEIVER_OK &&
	    result != SIPPYMODEM_V34_PHASE4_E_RECEIVER_COMPLETE)
		return result;
	if (!sippymodem_v34_phase4_e_channel_receiver_is_valid(&next))
		return SIPPYMODEM_V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
	return result;
}
