// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase4_cp_tx.h"

#include "packed_bits.h"

#include <string.h>

struct encoded_cp_record {
	uint8_t frame[V90_CP_MAX_FRAME_BYTES];
	size_t bit_count;
};

_Static_assert(V90_CP_MIN_FRAME_BITS % 4 == 0,
	       "minimum CP record must contain whole 16-point symbols");
_Static_assert(V90_CP_CONSTELLATION_BITS % 4 == 0,
	       "each CP constellation must contain whole 16-point symbols");
_Static_assert(V90_CP_MAX_FRAME_BITS % 4 == 0,
	       "maximum CP record must contain whole 16-point symbols");

static bool frame_is_valid(
	const uint8_t *frame, size_t bit_count)
{
	struct v90_cp_parameters parameters;
	size_t byte_count;

	if (bit_count < V90_CP_MIN_FRAME_BITS ||
	    bit_count > V90_CP_MAX_FRAME_BITS ||
	    bit_count % 4u != 0 ||
	    !packed_msb_tail_is_zero(
		    frame, bit_count, V90_CP_MAX_FRAME_BYTES))
		return false;
	byte_count = (bit_count + 7u) / 8u;
	return v90_cp_decode(frame, byte_count, bit_count, &parameters) ==
		       V90_CP_OK &&
	       v90_cp_frame_bits(&parameters) == bit_count;
}

bool v90_phase4_cp_tx_is_valid(
	const struct v90_phase4_cp_tx *tx)
{
	if (tx == NULL ||
	    !v34_constellation_size_is_valid(tx->constellation) ||
	    !v34_transmit_scrambler_is_valid(&tx->scrambler, tx->role) ||
	    tx->previous_clockwise_quarter_turns > 3u ||
	    tx->bit_index > tx->frame_bit_count ||
	    !frame_is_valid(tx->frame, tx->frame_bit_count))
		return false;
	return tx->bit_index %
		       v34_constellation_bits_per_symbol(tx->constellation) ==
	       0;
}

bool v90_phase4_cp_tx_record_is_complete(
	const struct v90_phase4_cp_tx *tx)
{
	return v90_phase4_cp_tx_is_valid(tx) &&
	       tx->bit_index == tx->frame_bit_count;
}

static enum v90_phase4_cp_tx_result codec_result(
	enum v90_cp_result result)
{
	if (result == V90_CP_OK)
		return V90_PHASE4_CP_TX_OK;
	if (result == V90_CP_INVALID_PARAMETERS)
		return V90_PHASE4_CP_TX_INVALID_PARAMETERS;
	return V90_PHASE4_CP_TX_INVALID_ARGUMENT;
}

static enum v90_phase4_cp_tx_result encode_record(
	struct encoded_cp_record *record,
	const struct v90_cp_parameters *parameters)
{
	struct encoded_cp_record encoded = { 0 };
	enum v90_cp_result result;

	if (record == NULL || parameters == NULL)
		return V90_PHASE4_CP_TX_INVALID_ARGUMENT;
	result = v90_cp_encode(
		parameters, encoded.frame, sizeof(encoded.frame),
		&encoded.bit_count);
	if (result != V90_CP_OK)
		return codec_result(result);
	if (!frame_is_valid(encoded.frame, encoded.bit_count))
		return V90_PHASE4_CP_TX_INVALID_ARGUMENT;
	*record = encoded;
	return V90_PHASE4_CP_TX_OK;
}

enum v90_phase4_cp_tx_result v90_phase4_cp_tx_init(
	struct v90_phase4_cp_tx *tx,
	enum v34_station_role role,
	enum v34_constellation_size constellation,
	const struct v90_cp_parameters *parameters)
{
	struct encoded_cp_record record;
	struct v90_phase4_cp_tx initialized = {
		.role = role,
		.constellation = constellation,
	};
	enum v90_phase4_cp_tx_result result;

	if (tx == NULL ||
	    !v34_constellation_size_is_valid(constellation))
		return V90_PHASE4_CP_TX_INVALID_ARGUMENT;
	if (v34_transmit_scrambler_init(
		    &initialized.scrambler, role) != V34_FRAMING_OK)
		return V90_PHASE4_CP_TX_INVALID_ARGUMENT;
	if (parameters == NULL)
		return V90_PHASE4_CP_TX_INVALID_ARGUMENT;
	if (parameters->kind != V90_CP_TRAINING)
		return V90_PHASE4_CP_TX_INVALID_PARAMETERS;
	result = encode_record(&record, parameters);
	if (result != V90_PHASE4_CP_TX_OK)
		return result;
	memcpy(initialized.frame, record.frame, sizeof(initialized.frame));
	initialized.frame_bit_count = record.bit_count;
	if (!v90_phase4_cp_tx_is_valid(&initialized))
		return V90_PHASE4_CP_TX_INVALID_ARGUMENT;
	*tx = initialized;
	return V90_PHASE4_CP_TX_OK;
}

static enum v90_phase4_cp_tx_result record_boundary(
	const struct v90_phase4_cp_tx *tx)
{
	if (!v90_phase4_cp_tx_is_valid(tx))
		return V90_PHASE4_CP_TX_INVALID_ARGUMENT;
	if (tx->bit_index != tx->frame_bit_count)
		return V90_PHASE4_CP_TX_RECORD_IN_PROGRESS;
	return V90_PHASE4_CP_TX_OK;
}

enum v90_phase4_cp_tx_result v90_phase4_cp_tx_start_next_record(
	struct v90_phase4_cp_tx *tx,
	const struct v90_cp_parameters *parameters)
{
	struct encoded_cp_record record;
	struct v90_phase4_cp_tx next;
	enum v90_phase4_cp_tx_result result;

	if (parameters == NULL)
		return V90_PHASE4_CP_TX_INVALID_ARGUMENT;
	result = record_boundary(tx);
	if (result != V90_PHASE4_CP_TX_OK)
		return result;
	result = encode_record(&record, parameters);
	if (result != V90_PHASE4_CP_TX_OK)
		return result;
	next = *tx;
	memcpy(next.frame, record.frame, sizeof(next.frame));
	next.frame_bit_count = record.bit_count;
	next.bit_index = 0;
	if (!v90_phase4_cp_tx_is_valid(&next))
		return V90_PHASE4_CP_TX_INVALID_ARGUMENT;
	*tx = next;
	return V90_PHASE4_CP_TX_OK;
}

static uint8_t frame_dibit(
	const uint8_t *frame, size_t first_bit)
{
	return (uint8_t)((packed_msb_bit(frame, first_bit) << 1) |
			 packed_msb_bit(frame, first_bit + 1u));
}

enum v90_phase4_cp_tx_result v90_phase4_cp_tx_next(
	struct v90_phase4_cp_tx *tx,
	struct qam_point *point, uint8_t *quarter_point,
	uint8_t *clockwise_quarter_turns, bool *record_complete)
{
	struct v90_phase4_cp_tx next;
	struct qam_point next_point;
	uint8_t next_quarter_point = 0;
	uint8_t next_rotation;
	bool complete;

	if (!v90_phase4_cp_tx_is_valid(tx) || point == NULL ||
	    quarter_point == NULL || clockwise_quarter_turns == NULL ||
	    record_complete == NULL)
		return V90_PHASE4_CP_TX_INVALID_ARGUMENT;
	if (tx->bit_index == tx->frame_bit_count)
		return V90_PHASE4_CP_TX_RECORD_COMPLETE;
	next = *tx;
	if (v34_scramble_differential_dibit(
		    &next.scrambler,
		    frame_dibit(next.frame, next.bit_index),
		    next.previous_clockwise_quarter_turns,
		    &next_rotation) != V34_FRAMING_OK)
		return V90_PHASE4_CP_TX_INVALID_ARGUMENT;
	if (next.constellation == V34_CONSTELLATION_16_POINT &&
	    v34_scramble_mapped_dibit(
		    &next.scrambler,
		    frame_dibit(next.frame, next.bit_index + 2u),
		    &next_quarter_point) != V34_FRAMING_OK)
		return V90_PHASE4_CP_TX_INVALID_ARGUMENT;
	if (v34_training_constellation_point(
		    next.constellation, next_quarter_point, next_rotation,
		    &next_point) != V34_CONSTELLATION_OK)
		return V90_PHASE4_CP_TX_INVALID_ARGUMENT;
	next.previous_clockwise_quarter_turns = next_rotation;
	next.bit_index += v34_constellation_bits_per_symbol(next.constellation);
	complete = next.bit_index == next.frame_bit_count;
	if (!v90_phase4_cp_tx_is_valid(&next))
		return V90_PHASE4_CP_TX_INVALID_ARGUMENT;
	*tx = next;
	*point = next_point;
	*quarter_point = next_quarter_point;
	*clockwise_quarter_turns = next_rotation;
	*record_complete = complete;
	return V90_PHASE4_CP_TX_OK;
}

const char *v90_phase4_cp_tx_result_name(
	enum v90_phase4_cp_tx_result result)
{
	switch (result) {
	case V90_PHASE4_CP_TX_OK:
		return "ok";
	case V90_PHASE4_CP_TX_RECORD_COMPLETE:
		return "record complete";
	case V90_PHASE4_CP_TX_RECORD_IN_PROGRESS:
		return "record in progress";
	case V90_PHASE4_CP_TX_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_PHASE4_CP_TX_INVALID_PARAMETERS:
		return "invalid parameters";
	}
	return "unknown";
}
