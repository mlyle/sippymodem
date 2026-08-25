// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase4_mp.h"

#include "packed_bits.h"

#include <string.h>

struct encoded_mp_record {
	uint8_t frame[V34_MP_TYPE1_FRAME_BYTES];
	size_t bit_count;
};

static size_t bits_per_symbol(
	enum v34_constellation_size constellation)
{
	return constellation == V34_CONSTELLATION_16_POINT ? 4u :
								    2u;
}

static bool frame_is_valid(const uint8_t *frame, size_t bit_count)
{
	if (bit_count != V34_MP_TYPE0_FRAME_BITS &&
	    bit_count != V34_MP_TYPE1_FRAME_BITS)
		return false;
	if (!packed_msb_tail_is_zero(
		    frame, bit_count, V34_MP_TYPE1_FRAME_BYTES))
		return false;
	if (bit_count == V34_MP_TYPE0_FRAME_BITS) {
		struct v34_mp_parameters parameters;

		return v34_mp_type0_decode(
			       frame, V34_MP_TYPE0_FRAME_BYTES,
			       &parameters) == V34_MP_OK;
	}
	{
		struct v34_mp_type1_parameters parameters;

		return v34_mp_type1_decode(
			       frame, V34_MP_TYPE1_FRAME_BYTES,
			       &parameters) == V34_MP_OK;
	}
}

bool v34_phase4_mp_is_valid(
	const struct v34_phase4_mp *transmitter)
{
	size_t symbol_bits;

	if (transmitter == NULL ||
	    !v34_constellation_size_is_valid(
		    transmitter->constellation) ||
	    !v34_transmit_scrambler_is_valid(
		    &transmitter->scrambler, transmitter->role) ||
	    transmitter->previous_clockwise_quarter_turns > 3u ||
	    transmitter->bit_index > transmitter->frame_bit_count ||
	    !frame_is_valid(
		    transmitter->frame, transmitter->frame_bit_count))
		return false;
	symbol_bits = bits_per_symbol(transmitter->constellation);
	return transmitter->bit_index % symbol_bits == 0;
}

bool v34_phase4_mp_record_is_complete(
	const struct v34_phase4_mp *transmitter)
{
	return v34_phase4_mp_is_valid(transmitter) &&
	       transmitter->bit_index == transmitter->frame_bit_count;
}

static enum v34_phase4_mp_result codec_result(
	enum v34_mp_result result)
{
	if (result == V34_MP_OK)
		return V34_PHASE4_MP_OK;
	if (result == V34_MP_INVALID_PARAMETERS)
		return V34_PHASE4_MP_INVALID_PARAMETERS;
	return V34_PHASE4_MP_INVALID_ARGUMENT;
}

static enum v34_phase4_mp_result encode_type0_record(
	struct encoded_mp_record *record,
	const struct v34_mp_parameters *parameters)
{
	struct encoded_mp_record encoded = {
		.bit_count = V34_MP_TYPE0_FRAME_BITS,
	};
	enum v34_mp_result result;

	if (record == NULL || parameters == NULL)
		return V34_PHASE4_MP_INVALID_ARGUMENT;
	result = v34_mp_type0_encode(
		parameters, encoded.frame, sizeof(encoded.frame));
	if (result != V34_MP_OK)
		return codec_result(result);
	*record = encoded;
	return V34_PHASE4_MP_OK;
}

static enum v34_phase4_mp_result encode_type1_record(
	struct encoded_mp_record *record,
	const struct v34_mp_type1_parameters *parameters)
{
	struct encoded_mp_record encoded = {
		.bit_count = V34_MP_TYPE1_FRAME_BITS,
	};
	enum v34_mp_result result;

	if (record == NULL || parameters == NULL)
		return V34_PHASE4_MP_INVALID_ARGUMENT;
	result = v34_mp_type1_encode(
		parameters, encoded.frame, sizeof(encoded.frame));
	if (result != V34_MP_OK)
		return codec_result(result);
	*record = encoded;
	return V34_PHASE4_MP_OK;
}

static bool can_initialize_from_trn(
	const struct v34_phase4_mp *transmitter,
	const struct v34_phase4_trn *trn)
{
	return transmitter != NULL && v34_phase4_trn_is_valid(trn) &&
	       trn->has_last_symbol;
}

static enum v34_phase4_mp_result initialize_from_record(
	struct v34_phase4_mp *transmitter,
	const struct v34_phase4_trn *trn,
	const struct encoded_mp_record *record)
{
	struct v34_phase4_mp initialized = { 0 };

	if (!can_initialize_from_trn(transmitter, trn) || record == NULL)
		return V34_PHASE4_MP_INVALID_ARGUMENT;
	memcpy(initialized.frame, record->frame, sizeof(initialized.frame));
	initialized.scrambler = trn->scrambler;
	initialized.role = trn->role;
	initialized.constellation = trn->constellation;
	initialized.frame_bit_count = record->bit_count;
	initialized.previous_clockwise_quarter_turns =
		trn->last_clockwise_quarter_turns;
	if (!v34_phase4_mp_is_valid(&initialized))
		return V34_PHASE4_MP_INVALID_ARGUMENT;
	*transmitter = initialized;
	return V34_PHASE4_MP_OK;
}

enum v34_phase4_mp_result
v34_phase4_mp_init_type0_from_trn(
	struct v34_phase4_mp *transmitter,
	const struct v34_phase4_trn *trn,
	const struct v34_mp_parameters *parameters)
{
	struct encoded_mp_record record;
	enum v34_phase4_mp_result result;

	if (!can_initialize_from_trn(transmitter, trn))
		return V34_PHASE4_MP_INVALID_ARGUMENT;
	result = encode_type0_record(&record, parameters);
	if (result != V34_PHASE4_MP_OK)
		return result;
	return initialize_from_record(transmitter, trn, &record);
}

enum v34_phase4_mp_result
v34_phase4_mp_init_type1_from_trn(
	struct v34_phase4_mp *transmitter,
	const struct v34_phase4_trn *trn,
	const struct v34_mp_type1_parameters *parameters)
{
	struct encoded_mp_record record;
	enum v34_phase4_mp_result result;

	if (!can_initialize_from_trn(transmitter, trn))
		return V34_PHASE4_MP_INVALID_ARGUMENT;
	result = encode_type1_record(&record, parameters);
	if (result != V34_PHASE4_MP_OK)
		return result;
	return initialize_from_record(transmitter, trn, &record);
}

static enum v34_phase4_mp_result record_boundary(
	const struct v34_phase4_mp *transmitter)
{
	if (!v34_phase4_mp_is_valid(transmitter))
		return V34_PHASE4_MP_INVALID_ARGUMENT;
	if (transmitter->bit_index != transmitter->frame_bit_count)
		return V34_PHASE4_MP_RECORD_IN_PROGRESS;
	return V34_PHASE4_MP_OK;
}

static enum v34_phase4_mp_result replace_record(
	struct v34_phase4_mp *transmitter,
	const struct encoded_mp_record *record)
{
	struct v34_phase4_mp next = *transmitter;

	memcpy(next.frame, record->frame, sizeof(next.frame));
	next.frame_bit_count = record->bit_count;
	next.bit_index = 0;
	if (!v34_phase4_mp_is_valid(&next))
		return V34_PHASE4_MP_INVALID_ARGUMENT;
	*transmitter = next;
	return V34_PHASE4_MP_OK;
}

enum v34_phase4_mp_result
v34_phase4_mp_start_next_type0_record(
	struct v34_phase4_mp *transmitter,
	const struct v34_mp_parameters *parameters)
{
	struct encoded_mp_record record;
	enum v34_phase4_mp_result result;

	if (parameters == NULL)
		return V34_PHASE4_MP_INVALID_ARGUMENT;
	result = record_boundary(transmitter);
	if (result != V34_PHASE4_MP_OK)
		return result;
	result = encode_type0_record(&record, parameters);
	if (result != V34_PHASE4_MP_OK)
		return result;
	return replace_record(transmitter, &record);
}

enum v34_phase4_mp_result
v34_phase4_mp_start_next_type1_record(
	struct v34_phase4_mp *transmitter,
	const struct v34_mp_type1_parameters *parameters)
{
	struct encoded_mp_record record;
	enum v34_phase4_mp_result result;

	if (parameters == NULL)
		return V34_PHASE4_MP_INVALID_ARGUMENT;
	result = record_boundary(transmitter);
	if (result != V34_PHASE4_MP_OK)
		return result;
	result = encode_type1_record(&record, parameters);
	if (result != V34_PHASE4_MP_OK)
		return result;
	return replace_record(transmitter, &record);
}

static uint8_t frame_dibit(const uint8_t *frame, size_t first_bit)
{
	return (uint8_t)((packed_msb_bit(frame, first_bit) << 1) |
			 packed_msb_bit(frame, first_bit + 1u));
}

enum v34_phase4_mp_result v34_phase4_mp_next(
	struct v34_phase4_mp *transmitter,
	struct qam_point *point, uint8_t *quarter_point,
	uint8_t *clockwise_quarter_turns, bool *record_complete)
{
	struct v34_phase4_mp next;
	struct qam_point next_point;
	uint8_t next_quarter_point = 0;
	uint8_t next_rotation;
	bool complete;

	if (!v34_phase4_mp_is_valid(transmitter) ||
	    point == NULL || quarter_point == NULL ||
	    clockwise_quarter_turns == NULL || record_complete == NULL)
		return V34_PHASE4_MP_INVALID_ARGUMENT;
	if (transmitter->bit_index == transmitter->frame_bit_count)
		return V34_PHASE4_MP_RECORD_COMPLETE;
	next = *transmitter;
	if (v34_scramble_differential_dibit(
		    &next.scrambler,
		    frame_dibit(next.frame, next.bit_index),
		    next.previous_clockwise_quarter_turns,
		    &next_rotation) != V34_FRAMING_OK)
		return V34_PHASE4_MP_INVALID_ARGUMENT;
	if (next.constellation == V34_CONSTELLATION_16_POINT &&
	    v34_scramble_mapped_dibit(
		    &next.scrambler,
		    frame_dibit(next.frame, next.bit_index + 2u),
		    &next_quarter_point) != V34_FRAMING_OK)
		return V34_PHASE4_MP_INVALID_ARGUMENT;
	if (v34_training_constellation_point(
		    next.constellation, next_quarter_point, next_rotation,
		    &next_point) != V34_CONSTELLATION_OK)
		return V34_PHASE4_MP_INVALID_ARGUMENT;
	next.previous_clockwise_quarter_turns = next_rotation;
	next.bit_index += bits_per_symbol(next.constellation);
	complete = next.bit_index == next.frame_bit_count;
	if (!v34_phase4_mp_is_valid(&next))
		return V34_PHASE4_MP_INVALID_ARGUMENT;
	*transmitter = next;
	*point = next_point;
	*quarter_point = next_quarter_point;
	*clockwise_quarter_turns = next_rotation;
	*record_complete = complete;
	return V34_PHASE4_MP_OK;
}
