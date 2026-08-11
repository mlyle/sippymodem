// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase4_mp.h"

#include <string.h>

struct encoded_mp_record {
	uint8_t frame[SIPPYMODEM_V34_MP_TYPE1_FRAME_BYTES];
	size_t bit_count;
};

static size_t bits_per_symbol(
	enum sippymodem_v34_constellation_size constellation)
{
	return constellation == SIPPYMODEM_V34_CONSTELLATION_16_POINT ? 4u :
								    2u;
}

static bool unused_frame_storage_is_zero(
	const uint8_t *frame, size_t bit_count)
{
	size_t byte_index = bit_count / 8u;
	size_t used_bits = bit_count % 8u;

	if (used_bits != 0) {
		uint8_t unused_mask =
			(uint8_t)((1u << (8u - used_bits)) - 1u);

		if ((frame[byte_index] & unused_mask) != 0)
			return false;
		++byte_index;
	}
	for (; byte_index < SIPPYMODEM_V34_MP_TYPE1_FRAME_BYTES;
	     ++byte_index) {
		if (frame[byte_index] != 0)
			return false;
	}
	return true;
}

static bool frame_is_valid(const uint8_t *frame, size_t bit_count)
{
	if (bit_count != SIPPYMODEM_V34_MP_TYPE0_FRAME_BITS &&
	    bit_count != SIPPYMODEM_V34_MP_TYPE1_FRAME_BITS)
		return false;
	if (!unused_frame_storage_is_zero(frame, bit_count))
		return false;
	if (bit_count == SIPPYMODEM_V34_MP_TYPE0_FRAME_BITS) {
		struct sippymodem_v34_mp_parameters parameters;

		return sippymodem_v34_mp_type0_decode(
			       frame, SIPPYMODEM_V34_MP_TYPE0_FRAME_BYTES,
			       &parameters) == SIPPYMODEM_V34_MP_OK;
	}
	{
		struct sippymodem_v34_mp_type1_parameters parameters;

		return sippymodem_v34_mp_type1_decode(
			       frame, SIPPYMODEM_V34_MP_TYPE1_FRAME_BYTES,
			       &parameters) == SIPPYMODEM_V34_MP_OK;
	}
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
	    transmitter->bit_index > transmitter->frame_bit_count ||
	    !frame_is_valid(
		    transmitter->frame, transmitter->frame_bit_count))
		return false;
	symbol_bits = bits_per_symbol(transmitter->constellation);
	return transmitter->bit_index % symbol_bits == 0;
}

bool sippymodem_v34_phase4_mp_record_is_complete(
	const struct sippymodem_v34_phase4_mp *transmitter)
{
	return sippymodem_v34_phase4_mp_is_valid(transmitter) &&
	       transmitter->bit_index == transmitter->frame_bit_count;
}

static enum sippymodem_v34_phase4_mp_result codec_result(
	enum sippymodem_v34_mp_result result)
{
	if (result == SIPPYMODEM_V34_MP_OK)
		return SIPPYMODEM_V34_PHASE4_MP_OK;
	if (result == SIPPYMODEM_V34_MP_INVALID_PARAMETERS)
		return SIPPYMODEM_V34_PHASE4_MP_INVALID_PARAMETERS;
	return SIPPYMODEM_V34_PHASE4_MP_INVALID_ARGUMENT;
}

static enum sippymodem_v34_phase4_mp_result encode_type0_record(
	struct encoded_mp_record *record,
	const struct sippymodem_v34_mp_parameters *parameters)
{
	struct encoded_mp_record encoded = {
		.bit_count = SIPPYMODEM_V34_MP_TYPE0_FRAME_BITS,
	};
	enum sippymodem_v34_mp_result result;

	if (record == NULL || parameters == NULL)
		return SIPPYMODEM_V34_PHASE4_MP_INVALID_ARGUMENT;
	result = sippymodem_v34_mp_type0_encode(
		parameters, encoded.frame, sizeof(encoded.frame));
	if (result != SIPPYMODEM_V34_MP_OK)
		return codec_result(result);
	*record = encoded;
	return SIPPYMODEM_V34_PHASE4_MP_OK;
}

static enum sippymodem_v34_phase4_mp_result encode_type1_record(
	struct encoded_mp_record *record,
	const struct sippymodem_v34_mp_type1_parameters *parameters)
{
	struct encoded_mp_record encoded = {
		.bit_count = SIPPYMODEM_V34_MP_TYPE1_FRAME_BITS,
	};
	enum sippymodem_v34_mp_result result;

	if (record == NULL || parameters == NULL)
		return SIPPYMODEM_V34_PHASE4_MP_INVALID_ARGUMENT;
	result = sippymodem_v34_mp_type1_encode(
		parameters, encoded.frame, sizeof(encoded.frame));
	if (result != SIPPYMODEM_V34_MP_OK)
		return codec_result(result);
	*record = encoded;
	return SIPPYMODEM_V34_PHASE4_MP_OK;
}

static bool can_initialize_from_trn(
	const struct sippymodem_v34_phase4_mp *transmitter,
	const struct sippymodem_v34_phase4_trn *trn)
{
	return transmitter != NULL && sippymodem_v34_phase4_trn_is_valid(trn) &&
	       trn->has_last_symbol;
}

static enum sippymodem_v34_phase4_mp_result initialize_from_record(
	struct sippymodem_v34_phase4_mp *transmitter,
	const struct sippymodem_v34_phase4_trn *trn,
	const struct encoded_mp_record *record)
{
	struct sippymodem_v34_phase4_mp initialized = { 0 };

	if (!can_initialize_from_trn(transmitter, trn) || record == NULL)
		return SIPPYMODEM_V34_PHASE4_MP_INVALID_ARGUMENT;
	memcpy(initialized.frame, record->frame, sizeof(initialized.frame));
	initialized.scrambler = trn->scrambler;
	initialized.role = trn->role;
	initialized.constellation = trn->constellation;
	initialized.frame_bit_count = record->bit_count;
	initialized.previous_clockwise_quarter_turns =
		trn->last_clockwise_quarter_turns;
	if (!sippymodem_v34_phase4_mp_is_valid(&initialized))
		return SIPPYMODEM_V34_PHASE4_MP_INVALID_ARGUMENT;
	*transmitter = initialized;
	return SIPPYMODEM_V34_PHASE4_MP_OK;
}

enum sippymodem_v34_phase4_mp_result
sippymodem_v34_phase4_mp_init_type0_from_trn(
	struct sippymodem_v34_phase4_mp *transmitter,
	const struct sippymodem_v34_phase4_trn *trn,
	const struct sippymodem_v34_mp_parameters *parameters)
{
	struct encoded_mp_record record;
	enum sippymodem_v34_phase4_mp_result result;

	if (!can_initialize_from_trn(transmitter, trn))
		return SIPPYMODEM_V34_PHASE4_MP_INVALID_ARGUMENT;
	result = encode_type0_record(&record, parameters);
	if (result != SIPPYMODEM_V34_PHASE4_MP_OK)
		return result;
	return initialize_from_record(transmitter, trn, &record);
}

enum sippymodem_v34_phase4_mp_result
sippymodem_v34_phase4_mp_init_type1_from_trn(
	struct sippymodem_v34_phase4_mp *transmitter,
	const struct sippymodem_v34_phase4_trn *trn,
	const struct sippymodem_v34_mp_type1_parameters *parameters)
{
	struct encoded_mp_record record;
	enum sippymodem_v34_phase4_mp_result result;

	if (!can_initialize_from_trn(transmitter, trn))
		return SIPPYMODEM_V34_PHASE4_MP_INVALID_ARGUMENT;
	result = encode_type1_record(&record, parameters);
	if (result != SIPPYMODEM_V34_PHASE4_MP_OK)
		return result;
	return initialize_from_record(transmitter, trn, &record);
}

static enum sippymodem_v34_phase4_mp_result record_boundary(
	const struct sippymodem_v34_phase4_mp *transmitter)
{
	if (!sippymodem_v34_phase4_mp_is_valid(transmitter))
		return SIPPYMODEM_V34_PHASE4_MP_INVALID_ARGUMENT;
	if (transmitter->bit_index != transmitter->frame_bit_count)
		return SIPPYMODEM_V34_PHASE4_MP_RECORD_IN_PROGRESS;
	return SIPPYMODEM_V34_PHASE4_MP_OK;
}

static enum sippymodem_v34_phase4_mp_result replace_record(
	struct sippymodem_v34_phase4_mp *transmitter,
	const struct encoded_mp_record *record)
{
	struct sippymodem_v34_phase4_mp next = *transmitter;

	memcpy(next.frame, record->frame, sizeof(next.frame));
	next.frame_bit_count = record->bit_count;
	next.bit_index = 0;
	if (!sippymodem_v34_phase4_mp_is_valid(&next))
		return SIPPYMODEM_V34_PHASE4_MP_INVALID_ARGUMENT;
	*transmitter = next;
	return SIPPYMODEM_V34_PHASE4_MP_OK;
}

enum sippymodem_v34_phase4_mp_result
sippymodem_v34_phase4_mp_start_next_type0_record(
	struct sippymodem_v34_phase4_mp *transmitter,
	const struct sippymodem_v34_mp_parameters *parameters)
{
	struct encoded_mp_record record;
	enum sippymodem_v34_phase4_mp_result result;

	if (parameters == NULL)
		return SIPPYMODEM_V34_PHASE4_MP_INVALID_ARGUMENT;
	result = record_boundary(transmitter);
	if (result != SIPPYMODEM_V34_PHASE4_MP_OK)
		return result;
	result = encode_type0_record(&record, parameters);
	if (result != SIPPYMODEM_V34_PHASE4_MP_OK)
		return result;
	return replace_record(transmitter, &record);
}

enum sippymodem_v34_phase4_mp_result
sippymodem_v34_phase4_mp_start_next_type1_record(
	struct sippymodem_v34_phase4_mp *transmitter,
	const struct sippymodem_v34_mp_type1_parameters *parameters)
{
	struct encoded_mp_record record;
	enum sippymodem_v34_phase4_mp_result result;

	if (parameters == NULL)
		return SIPPYMODEM_V34_PHASE4_MP_INVALID_ARGUMENT;
	result = record_boundary(transmitter);
	if (result != SIPPYMODEM_V34_PHASE4_MP_OK)
		return result;
	result = encode_type1_record(&record, parameters);
	if (result != SIPPYMODEM_V34_PHASE4_MP_OK)
		return result;
	return replace_record(transmitter, &record);
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
	if (transmitter->bit_index == transmitter->frame_bit_count)
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
	complete = next.bit_index == next.frame_bit_count;
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
