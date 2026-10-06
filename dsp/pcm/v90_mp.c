// SPDX-License-Identifier: BSD-3-Clause

#include "v90_mp.h"

#include "pcm_mp.h"
#include "pcm_control_frame.h"
#include "packed_bits.h"

#include <string.h>

enum {
	V90_MP_START0_BIT = 17,
	V90_MP_TYPE_BIT = 18,
	V90_MP_RATE_FIRST_BIT = 24,
	V90_MP_RATE_BITS = 4,
	V90_MP_TRELLIS_FIRST_BIT = 29,
	V90_MP_TRELLIS_BITS = 2,
	V90_MP_NONLINEAR_ENCODING_BIT = 31,
	V90_MP_EXPANDED_SHAPING_BIT = 32,
	V90_MP_ACKNOWLEDGMENT_BIT = 33,
	V90_MP_RATE_CAPABILITY_FIRST_BIT = 36,
	V90_MP_TYPE0_CRC_START_BIT = 68,
	V90_MP_TYPE1_CRC_START_BIT = 170,
};

static bool rate_is_valid(uint8_t rate_2400_bps_units)
{
	return rate_2400_bps_units == 0 ||
	       (rate_2400_bps_units >=
			V90_MP_MINIMUM_RATE_2400_BPS_UNITS &&
		rate_2400_bps_units <=
			V90_MP_MAXIMUM_RATE_2400_BPS_UNITS);
}

bool v90_mp_parameters_are_valid(
	const struct v90_mp_parameters *parameters)
{
	return parameters != NULL &&
	       rate_is_valid(parameters->maximum_upstream_rate_2400_bps_units) &&
	       v34_trellis_selection_is_valid(parameters->upstream_trellis) &&
	       (parameters->upstream_rate_capability_mask &
		~V90_MP_RATE_CAPABILITY_MASK) == 0;
}

bool v90_mp_type1_parameters_are_valid(
	const struct v90_mp_type1_parameters *parameters)
{
	return parameters != NULL &&
	       v90_mp_parameters_are_valid(&parameters->common) &&
	       v34_precoder_coefficients_are_valid(
		       parameters->precoder_coefficients);
}

bool v90_mp_record_is_valid(const struct v90_mp_record *record)
{
	if (record == NULL)
		return false;
	if (record->type == V90_MP_RECORD_TYPE0)
		return v90_mp_parameters_are_valid(&record->parameters.type0);
	return record->type == V90_MP_RECORD_TYPE1 &&
	       v90_mp_type1_parameters_are_valid(&record->parameters.type1);
}

bool v90_mp_record_limit_maximum_upstream_rate(
	struct v90_mp_record *record,
	uint8_t maximum_rate_2400_bps_units)
{
	struct v90_mp_record limited;
	struct v90_mp_parameters *parameters;

	if (!v90_mp_record_is_valid(record) ||
	    maximum_rate_2400_bps_units <
		    V90_MP_MINIMUM_RATE_2400_BPS_UNITS ||
	    maximum_rate_2400_bps_units >
		    V90_MP_MAXIMUM_RATE_2400_BPS_UNITS)
		return false;
	limited = *record;
	parameters = v90_mp_record_mutable_common(&limited);
	if (parameters->maximum_upstream_rate_2400_bps_units == 0u ||
	    parameters->maximum_upstream_rate_2400_bps_units <=
		    maximum_rate_2400_bps_units)
		return true;
	for (int rate = maximum_rate_2400_bps_units;
	     rate >= V90_MP_MINIMUM_RATE_2400_BPS_UNITS; --rate) {
		uint16_t bit = (uint16_t)(1u <<
			(rate - V90_MP_MINIMUM_RATE_2400_BPS_UNITS));

		if ((parameters->upstream_rate_capability_mask & bit) == 0u)
			continue;
		parameters->maximum_upstream_rate_2400_bps_units =
			(uint8_t)rate;
		break;
	}
	if (!v90_mp_record_is_valid(&limited))
		return false;
	*record = limited;
	return true;
}

bool v90_mp_record_is_initial(const struct v90_mp_record *record)
{
	return v90_mp_record_is_valid(record) &&
	       !v90_mp_record_common(record)->acknowledges_cp;
}

bool v90_mp_record_acknowledges_cp(
	const struct v90_mp_record *record)
{
	return v90_mp_record_is_valid(record) &&
	       v90_mp_record_common(record)->acknowledges_cp;
}

static bool common_parameters_equal(
	const struct v90_mp_parameters *left,
	const struct v90_mp_parameters *right,
	bool compare_acknowledgement)
{
	return left->maximum_upstream_rate_2400_bps_units ==
		       right->maximum_upstream_rate_2400_bps_units &&
	       left->upstream_trellis == right->upstream_trellis &&
	       left->upstream_nonlinear_encoding ==
		       right->upstream_nonlinear_encoding &&
	       left->upstream_expanded_shaping ==
		       right->upstream_expanded_shaping &&
	       (!compare_acknowledgement ||
		left->acknowledges_cp == right->acknowledges_cp) &&
	       left->upstream_rate_capability_mask ==
		       right->upstream_rate_capability_mask;
}

static bool records_equal(
	const struct v90_mp_record *left,
	const struct v90_mp_record *right,
	bool compare_acknowledgement)
{
	return v90_mp_record_is_valid(left) &&
	       v90_mp_record_is_valid(right) && left->type == right->type &&
	       common_parameters_equal(
		       v90_mp_record_common(left), v90_mp_record_common(right),
		       compare_acknowledgement) &&
	       (left->type == V90_MP_RECORD_TYPE0 ||
		v34_precoder_coefficients_are_equal(
			left->parameters.type1.precoder_coefficients,
			right->parameters.type1.precoder_coefficients));
}

bool v90_mp_record_equal(
	const struct v90_mp_record *left,
	const struct v90_mp_record *right)
{
	return records_equal(left, right, true);
}

bool v90_mp_record_parameters_equal(
	const struct v90_mp_record *left,
	const struct v90_mp_record *right)
{
	return records_equal(left, right, false);
}

static bool training_frame_bits_are_valid(uint8_t training_frame_bits)
{
	return training_frame_bits >=
		       V90_MP_TRAINING_FRAME_MINIMUM_BITS &&
	       training_frame_bits <=
		       V90_MP_TRAINING_FRAME_MAXIMUM_BITS;
}

static size_t minimum_sequence_bits(enum v90_mp_record_type type)
{
	return type == V90_MP_RECORD_TYPE0 ?
		       V90_MP_TYPE0_MINIMUM_BITS :
		       V90_MP_TYPE1_MINIMUM_BITS;
}

enum v90_mp_result v90_mp_sequence_size(
	enum v90_mp_record_type type, uint8_t training_frame_bits,
	size_t *bit_count)
{
	size_t minimum;
	size_t rounded;

	if (bit_count == NULL)
		return V90_MP_INVALID_ARGUMENT;
	if ((type != V90_MP_RECORD_TYPE0 &&
	     type != V90_MP_RECORD_TYPE1) ||
	    !training_frame_bits_are_valid(training_frame_bits))
		return V90_MP_INVALID_PARAMETERS;
	minimum = minimum_sequence_bits(type);
	rounded = minimum +
		  (training_frame_bits - minimum % training_frame_bits) %
			  training_frame_bits;
	if (rounded > V90_MP_MAXIMUM_SEQUENCE_BITS)
		return V90_MP_INVALID_PARAMETERS;
	*bit_count = rounded;
	return V90_MP_OK;
}

static void encode_common(
	uint8_t *sequence, const struct v90_mp_parameters *parameters,
	bool type1)
{
	pcm_control_frame_write_prefix(sequence);
	packed_msb_set_bit(sequence, V90_MP_TYPE_BIT, type1);
	packed_msb_write_lsb_field(
		sequence, V90_MP_RATE_FIRST_BIT, V90_MP_RATE_BITS,
		parameters->maximum_upstream_rate_2400_bps_units);
	packed_msb_write_lsb_field(
		sequence, V90_MP_TRELLIS_FIRST_BIT, V90_MP_TRELLIS_BITS,
		(uint16_t)parameters->upstream_trellis);
	packed_msb_set_bit(
		sequence, V90_MP_NONLINEAR_ENCODING_BIT,
		parameters->upstream_nonlinear_encoding);
	packed_msb_set_bit(
		sequence, V90_MP_EXPANDED_SHAPING_BIT,
		parameters->upstream_expanded_shaping);
	packed_msb_set_bit(
		sequence, V90_MP_ACKNOWLEDGMENT_BIT,
		parameters->acknowledges_cp);
	packed_msb_write_lsb_field(
		sequence, V90_MP_RATE_CAPABILITY_FIRST_BIT,
		V90_MP_RATE_CAPABILITY_BITS,
		parameters->upstream_rate_capability_mask);
}

enum v90_mp_result v90_mp_encode(
	const struct v90_mp_record *record, uint8_t training_frame_bits,
	uint8_t *sequence, size_t sequence_capacity,
	size_t *sequence_bit_count)
{
	uint8_t encoded[V90_MP_MAXIMUM_SEQUENCE_BYTES] = { 0 };
	size_t bit_count;
	size_t byte_count;
	size_t crc_start_bit;
	uint16_t crc;

	if (record == NULL || sequence == NULL ||
	    sequence_bit_count == NULL)
		return V90_MP_INVALID_ARGUMENT;
	if (!v90_mp_record_is_valid(record) ||
	    v90_mp_sequence_size(
		    record->type, training_frame_bits, &bit_count) !=
		    V90_MP_OK)
		return V90_MP_INVALID_PARAMETERS;
	byte_count = (bit_count + 7u) / 8u;
	if (sequence_capacity < byte_count)
		return V90_MP_NEED_OUTPUT;

	encode_common(
		encoded, v90_mp_record_common(record),
		record->type == V90_MP_RECORD_TYPE1);
	if (record->type == V90_MP_RECORD_TYPE1) {
		pcm_mp_write_precoder(encoded, record->parameters.type1.precoder_coefficients);
		crc_start_bit = V90_MP_TYPE1_CRC_START_BIT;
	} else {
		crc_start_bit = V90_MP_TYPE0_CRC_START_BIT;
	}
	crc = pcm_control_frame_crc(encoded, crc_start_bit);
	packed_msb_write_msb_field(
		encoded, crc_start_bit + 1u, V90_MP_CRC_BITS, crc);
	memcpy(sequence, encoded, byte_count);
	*sequence_bit_count = bit_count;
	return V90_MP_OK;
}

static bool framing_bits_are_valid(
	const uint8_t *sequence, size_t sequence_bit_count,
	size_t crc_start_bit)
{
	if (packed_msb_read_msb_field(
		    sequence, 0u, V90_MP_FRAME_SYNC_BITS) != UINT32_C(0x1ffff))
		return false;
	return pcm_control_frame_starts_are_zero(
		       sequence, V90_MP_START0_BIT, crc_start_bit + 1u) &&
	       packed_msb_bits_are_zero(
		       sequence, crc_start_bit + 1u + V90_MP_CRC_BITS,
		       sequence_bit_count - crc_start_bit - 1u - V90_MP_CRC_BITS);
}

static void decode_common(
	const uint8_t *sequence, struct v90_mp_parameters *parameters)
{
	parameters->maximum_upstream_rate_2400_bps_units =
		(uint8_t)packed_msb_read_lsb_field(
			sequence, V90_MP_RATE_FIRST_BIT, V90_MP_RATE_BITS);
	parameters->upstream_trellis =
		(enum v34_trellis_selection)packed_msb_read_lsb_field(
			sequence, V90_MP_TRELLIS_FIRST_BIT,
			V90_MP_TRELLIS_BITS);
	parameters->upstream_nonlinear_encoding =
		packed_msb_bit(sequence, V90_MP_NONLINEAR_ENCODING_BIT) != 0;
	parameters->upstream_expanded_shaping =
		packed_msb_bit(sequence, V90_MP_EXPANDED_SHAPING_BIT) != 0;
	parameters->acknowledges_cp =
		packed_msb_bit(sequence, V90_MP_ACKNOWLEDGMENT_BIT) != 0;
	parameters->upstream_rate_capability_mask =
		(uint16_t)packed_msb_read_lsb_field(
			sequence, V90_MP_RATE_CAPABILITY_FIRST_BIT,
			V90_MP_RATE_CAPABILITY_BITS);
}

enum v90_mp_result v90_mp_decode(
	const uint8_t *sequence, size_t sequence_bit_count,
	uint8_t training_frame_bits, struct v90_mp_record *record)
{
	struct v90_mp_record decoded = { 0 };
	size_t expected_bit_count;
	size_t crc_start_bit;
	uint16_t received_crc;

	if (sequence == NULL || record == NULL)
		return V90_MP_INVALID_ARGUMENT;
	if (!training_frame_bits_are_valid(training_frame_bits))
		return V90_MP_INVALID_PARAMETERS;
	if (sequence_bit_count <= V90_MP_TYPE_BIT)
		return V90_MP_INVALID_FRAME;
	decoded.type = packed_msb_bit(sequence, V90_MP_TYPE_BIT) != 0 ?
			       V90_MP_RECORD_TYPE1 :
			       V90_MP_RECORD_TYPE0;
	if (v90_mp_sequence_size(
		    decoded.type, training_frame_bits,
		    &expected_bit_count) != V90_MP_OK ||
	    sequence_bit_count != expected_bit_count)
		return V90_MP_INVALID_FRAME;
	crc_start_bit = decoded.type == V90_MP_RECORD_TYPE1 ?
				V90_MP_TYPE1_CRC_START_BIT :
				V90_MP_TYPE0_CRC_START_BIT;
	if (!framing_bits_are_valid(
		    sequence, sequence_bit_count, crc_start_bit))
		return V90_MP_INVALID_FRAME;
	received_crc = (uint16_t)packed_msb_read_msb_field(
		sequence, crc_start_bit + 1u, V90_MP_CRC_BITS);
	if (received_crc != pcm_control_frame_crc(sequence, crc_start_bit))
		return V90_MP_CRC_MISMATCH;

	decode_common(sequence, v90_mp_record_mutable_common(&decoded));
	if (decoded.type == V90_MP_RECORD_TYPE1)
		pcm_mp_read_precoder(sequence, decoded.parameters.type1.precoder_coefficients);
	if (!v90_mp_record_is_valid(&decoded))
		return V90_MP_INVALID_FRAME;
	*record = decoded;
	return V90_MP_OK;
}

const char *v90_mp_result_name(enum v90_mp_result result)
{
	switch (result) {
	case V90_MP_OK:
		return "ok";
	case V90_MP_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_MP_INVALID_PARAMETERS:
		return "invalid parameters";
	case V90_MP_NEED_OUTPUT:
		return "need output";
	case V90_MP_INVALID_FRAME:
		return "invalid frame";
	case V90_MP_CRC_MISMATCH:
		return "CRC mismatch";
	}
	return "unknown";
}
