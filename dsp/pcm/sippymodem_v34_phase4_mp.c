// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v34_phase4_mp.h"

static size_t bits_per_symbol(
	enum sippymodem_v34_constellation_size constellation)
{
	return constellation == SIPPYMODEM_V34_CONSTELLATION_16_POINT ? 4u :
								    2u;
}

static bool frame_is_valid(const uint8_t *frame)
{
	struct sippymodem_v34_mp_parameters parameters;

	return sippymodem_v34_mp_type0_decode(
		       frame, SIPPYMODEM_V34_MP_TYPE0_FRAME_BYTES,
		       &parameters) == SIPPYMODEM_V34_MP_OK;
}

bool sippymodem_v34_phase4_mp_is_valid(
	const struct sippymodem_v34_phase4_mp *transmitter)
{
	size_t symbol_bits;

	if (transmitter == NULL ||
	    !sippymodem_v34_constellation_size_is_valid(
		    transmitter->constellation) ||
	    !sippymodem_v34_transmit_scrambler_is_valid(
		    &transmitter->scrambler, transmitter->role) ||
	    transmitter->previous_clockwise_quarter_turns > 3u ||
	    transmitter->bit_index > SIPPYMODEM_V34_MP_TYPE0_FRAME_BITS ||
	    !frame_is_valid(transmitter->frame))
		return false;
	symbol_bits = bits_per_symbol(transmitter->constellation);
	return transmitter->bit_index % symbol_bits == 0;
}

bool sippymodem_v34_phase4_mp_record_is_complete(
	const struct sippymodem_v34_phase4_mp *transmitter)
{
	return sippymodem_v34_phase4_mp_is_valid(transmitter) &&
	       transmitter->bit_index == SIPPYMODEM_V34_MP_TYPE0_FRAME_BITS;
}

static enum sippymodem_v34_phase4_mp_result encode_record(
	uint8_t *frame,
	const struct sippymodem_v34_mp_parameters *parameters)
{
	enum sippymodem_v34_mp_result result;

	if (parameters == NULL)
		return SIPPYMODEM_V34_PHASE4_MP_INVALID_ARGUMENT;
	result = sippymodem_v34_mp_type0_encode(
		parameters, frame, SIPPYMODEM_V34_MP_TYPE0_FRAME_BYTES);
	if (result == SIPPYMODEM_V34_MP_INVALID_PARAMETERS)
		return SIPPYMODEM_V34_PHASE4_MP_INVALID_PARAMETERS;
	if (result != SIPPYMODEM_V34_MP_OK)
		return SIPPYMODEM_V34_PHASE4_MP_INVALID_ARGUMENT;
	return SIPPYMODEM_V34_PHASE4_MP_OK;
}

enum sippymodem_v34_phase4_mp_result
sippymodem_v34_phase4_mp_init_from_trn(
	struct sippymodem_v34_phase4_mp *transmitter,
	const struct sippymodem_v34_phase4_trn *trn,
	const struct sippymodem_v34_mp_parameters *parameters)
{
	struct sippymodem_v34_phase4_mp initialized = { 0 };
	enum sippymodem_v34_phase4_mp_result result;

	if (transmitter == NULL ||
	    !sippymodem_v34_phase4_trn_is_valid(trn) ||
	    !trn->has_last_symbol)
		return SIPPYMODEM_V34_PHASE4_MP_INVALID_ARGUMENT;
	result = encode_record(initialized.frame, parameters);
	if (result != SIPPYMODEM_V34_PHASE4_MP_OK)
		return result;
	initialized.scrambler = trn->scrambler;
	initialized.role = trn->role;
	initialized.constellation = trn->constellation;
	initialized.previous_clockwise_quarter_turns =
		trn->last_clockwise_quarter_turns;
	if (!sippymodem_v34_phase4_mp_is_valid(&initialized))
		return SIPPYMODEM_V34_PHASE4_MP_INVALID_ARGUMENT;
	*transmitter = initialized;
	return SIPPYMODEM_V34_PHASE4_MP_OK;
}

enum sippymodem_v34_phase4_mp_result
sippymodem_v34_phase4_mp_start_next_record(
	struct sippymodem_v34_phase4_mp *transmitter,
	const struct sippymodem_v34_mp_parameters *parameters)
{
	struct sippymodem_v34_phase4_mp next;
	enum sippymodem_v34_phase4_mp_result result;

	if (!sippymodem_v34_phase4_mp_is_valid(transmitter) ||
	    parameters == NULL)
		return SIPPYMODEM_V34_PHASE4_MP_INVALID_ARGUMENT;
	if (transmitter->bit_index != SIPPYMODEM_V34_MP_TYPE0_FRAME_BITS)
		return SIPPYMODEM_V34_PHASE4_MP_RECORD_IN_PROGRESS;
	next = *transmitter;
	result = encode_record(next.frame, parameters);
	if (result != SIPPYMODEM_V34_PHASE4_MP_OK)
		return result;
	next.bit_index = 0;
	if (!sippymodem_v34_phase4_mp_is_valid(&next))
		return SIPPYMODEM_V34_PHASE4_MP_INVALID_ARGUMENT;
	*transmitter = next;
	return SIPPYMODEM_V34_PHASE4_MP_OK;
}

static uint8_t frame_bit(const uint8_t *frame, size_t bit_index)
{
	return (uint8_t)((frame[bit_index / 8u] >>
			  (7u - bit_index % 8u)) &
			 1u);
}

static uint8_t frame_dibit(const uint8_t *frame, size_t first_bit)
{
	return (uint8_t)((frame_bit(frame, first_bit) << 1) |
			 frame_bit(frame, first_bit + 1u));
}

enum sippymodem_v34_phase4_mp_result sippymodem_v34_phase4_mp_next(
	struct sippymodem_v34_phase4_mp *transmitter,
	struct sippymodem_qam_point *point, uint8_t *quarter_point,
	uint8_t *clockwise_quarter_turns, bool *record_complete)
{
	struct sippymodem_v34_phase4_mp next;
	struct sippymodem_qam_point next_point;
	uint8_t next_quarter_point = 0;
	uint8_t next_rotation;
	bool complete;

	if (!sippymodem_v34_phase4_mp_is_valid(transmitter) ||
	    point == NULL || quarter_point == NULL ||
	    clockwise_quarter_turns == NULL || record_complete == NULL)
		return SIPPYMODEM_V34_PHASE4_MP_INVALID_ARGUMENT;
	if (transmitter->bit_index == SIPPYMODEM_V34_MP_TYPE0_FRAME_BITS)
		return SIPPYMODEM_V34_PHASE4_MP_RECORD_COMPLETE;
	next = *transmitter;
	if (sippymodem_v34_scramble_differential_dibit(
		    &next.scrambler,
		    frame_dibit(next.frame, next.bit_index),
		    next.previous_clockwise_quarter_turns,
		    &next_rotation) != SIPPYMODEM_V34_FRAMING_OK)
		return SIPPYMODEM_V34_PHASE4_MP_INVALID_ARGUMENT;
	if (next.constellation == SIPPYMODEM_V34_CONSTELLATION_16_POINT &&
	    sippymodem_v34_scramble_mapped_dibit(
		    &next.scrambler,
		    frame_dibit(next.frame, next.bit_index + 2u),
		    &next_quarter_point) != SIPPYMODEM_V34_FRAMING_OK)
		return SIPPYMODEM_V34_PHASE4_MP_INVALID_ARGUMENT;
	if (sippymodem_v34_training_constellation_point(
		    next.constellation, next_quarter_point, next_rotation,
		    &next_point) != SIPPYMODEM_V34_CONSTELLATION_OK)
		return SIPPYMODEM_V34_PHASE4_MP_INVALID_ARGUMENT;
	next.previous_clockwise_quarter_turns = next_rotation;
	next.bit_index += bits_per_symbol(next.constellation);
	complete = next.bit_index == SIPPYMODEM_V34_MP_TYPE0_FRAME_BITS;
	if (!sippymodem_v34_phase4_mp_is_valid(&next))
		return SIPPYMODEM_V34_PHASE4_MP_INVALID_ARGUMENT;
	*transmitter = next;
	*point = next_point;
	*quarter_point = next_quarter_point;
	*clockwise_quarter_turns = next_rotation;
	*record_complete = complete;
	return SIPPYMODEM_V34_PHASE4_MP_OK;
}

const char *sippymodem_v34_phase4_mp_result_name(
	enum sippymodem_v34_phase4_mp_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_PHASE4_MP_OK:
		return "ok";
	case SIPPYMODEM_V34_PHASE4_MP_RECORD_COMPLETE:
		return "record complete";
	case SIPPYMODEM_V34_PHASE4_MP_RECORD_IN_PROGRESS:
		return "record in progress";
	case SIPPYMODEM_V34_PHASE4_MP_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_V34_PHASE4_MP_INVALID_PARAMETERS:
		return "invalid parameters";
	}
	return "unknown";
}
