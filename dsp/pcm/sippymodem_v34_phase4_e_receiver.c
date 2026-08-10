// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v34_phase4_e_receiver.h"

#include <stddef.h>

static uint8_t bits_per_symbol(
	enum sippymodem_v34_constellation_size constellation)
{
	return constellation == SIPPYMODEM_V34_CONSTELLATION_16_POINT ? 4u :
								    2u;
}

bool sippymodem_v34_phase4_e_receiver_is_valid(
	const struct sippymodem_v34_phase4_e_receiver *receiver)
{
	if (receiver == NULL ||
	    !sippymodem_v34_constellation_size_is_valid(
		    receiver->constellation) ||
	    !sippymodem_v34_transmit_scrambler_is_valid(
		    &receiver->descrambler, receiver->transmitting_role) ||
	    receiver->previous_clockwise_quarter_turns > 3u ||
	    receiver->received_bits > SIPPYMODEM_V34_PHASE4_E_BITS)
		return false;
	return receiver->received_bits %
		       bits_per_symbol(receiver->constellation) ==
	       0;
}

bool sippymodem_v34_phase4_e_receiver_is_complete(
	const struct sippymodem_v34_phase4_e_receiver *receiver)
{
	return sippymodem_v34_phase4_e_receiver_is_valid(receiver) &&
	       receiver->received_bits == SIPPYMODEM_V34_PHASE4_E_BITS;
}

enum sippymodem_v34_phase4_e_receiver_result
sippymodem_v34_phase4_e_receiver_init_from_mp(
	struct sippymodem_v34_phase4_e_receiver *receiver,
	const struct sippymodem_v34_phase4_mp_receiver *mp_receiver)
{
	struct sippymodem_v34_phase4_e_receiver initialized;

	if (receiver == NULL ||
	    !sippymodem_v34_phase4_mp_receiver_is_valid(mp_receiver) ||
	    !mp_receiver->has_previous_symbol)
		return SIPPYMODEM_V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT;
	initialized = (struct sippymodem_v34_phase4_e_receiver){
		.descrambler = mp_receiver->descrambler,
		.transmitting_role = mp_receiver->transmitting_role,
		.constellation = mp_receiver->constellation,
		.previous_clockwise_quarter_turns =
			mp_receiver->previous_clockwise_quarter_turns,
	};
	if (!sippymodem_v34_phase4_e_receiver_is_valid(&initialized))
		return SIPPYMODEM_V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return SIPPYMODEM_V34_PHASE4_E_RECEIVER_OK;
}

enum sippymodem_v34_phase4_e_receiver_result
sippymodem_v34_phase4_e_receive_decision(
	struct sippymodem_v34_phase4_e_receiver *receiver,
	uint8_t quarter_point, uint8_t clockwise_quarter_turns)
{
	struct sippymodem_v34_phase4_e_receiver next;
	struct sippymodem_qam_point checked_point;
	uint8_t dibit;

	if (!sippymodem_v34_phase4_e_receiver_is_valid(receiver) ||
	    sippymodem_v34_training_constellation_point(
		    receiver->constellation, quarter_point,
		    clockwise_quarter_turns, &checked_point) !=
		    SIPPYMODEM_V34_CONSTELLATION_OK)
		return SIPPYMODEM_V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT;
	if (sippymodem_v34_phase4_e_receiver_is_complete(receiver))
		return SIPPYMODEM_V34_PHASE4_E_RECEIVER_COMPLETE;
	next = *receiver;
	if (sippymodem_v34_descramble_differential_dibit(
		    &next.descrambler,
		    next.previous_clockwise_quarter_turns,
		    clockwise_quarter_turns, &dibit) !=
		    SIPPYMODEM_V34_FRAMING_OK ||
	    dibit != 3u)
		return SIPPYMODEM_V34_PHASE4_E_RECEIVER_SEQUENCE_MISMATCH;
	if (next.constellation == SIPPYMODEM_V34_CONSTELLATION_16_POINT &&
	    (sippymodem_v34_descramble_mapped_dibit(
		     &next.descrambler, quarter_point, &dibit) !=
		     SIPPYMODEM_V34_FRAMING_OK ||
	     dibit != 3u))
		return SIPPYMODEM_V34_PHASE4_E_RECEIVER_SEQUENCE_MISMATCH;
	next.previous_clockwise_quarter_turns = clockwise_quarter_turns;
	next.received_bits = (uint8_t)(
		next.received_bits + bits_per_symbol(next.constellation));
	if (!sippymodem_v34_phase4_e_receiver_is_valid(&next))
		return SIPPYMODEM_V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
	return sippymodem_v34_phase4_e_receiver_is_complete(receiver) ?
		       SIPPYMODEM_V34_PHASE4_E_RECEIVER_COMPLETE :
		       SIPPYMODEM_V34_PHASE4_E_RECEIVER_OK;
}

enum sippymodem_v34_phase4_e_receiver_result
sippymodem_v34_phase4_e_receive_point(
	struct sippymodem_v34_phase4_e_receiver *receiver,
	const struct sippymodem_qam_point *point)
{
	uint8_t quarter_point;
	uint8_t clockwise_quarter_turns;

	if (!sippymodem_v34_phase4_e_receiver_is_valid(receiver) ||
	    sippymodem_v34_training_constellation_slice(
		    receiver->constellation, point, &quarter_point,
		    &clockwise_quarter_turns) !=
		    SIPPYMODEM_V34_CONSTELLATION_OK)
		return SIPPYMODEM_V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT;
	return sippymodem_v34_phase4_e_receive_decision(
		receiver, quarter_point, clockwise_quarter_turns);
}

const char *sippymodem_v34_phase4_e_receiver_result_name(
	enum sippymodem_v34_phase4_e_receiver_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_PHASE4_E_RECEIVER_OK:
		return "ok";
	case SIPPYMODEM_V34_PHASE4_E_RECEIVER_COMPLETE:
		return "complete";
	case SIPPYMODEM_V34_PHASE4_E_RECEIVER_SEQUENCE_MISMATCH:
		return "sequence mismatch";
	case SIPPYMODEM_V34_PHASE4_E_RECEIVER_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
