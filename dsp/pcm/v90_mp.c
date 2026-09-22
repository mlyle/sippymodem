// SPDX-License-Identifier: BSD-3-Clause

#include "v90_mp.h"

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
	V90_MP_TYPE1_FIRST_COEFFICIENT_BIT = 52,
	V90_MP_COEFFICIENT_COMPONENT_BITS = 16,
	V90_MP_FIELD_STRIDE_BITS = 17,
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
	if (parameters == NULL ||
	    !v90_mp_parameters_are_valid(&parameters->common))
		return false;
	for (size_t tap = 0; tap < V34_PRECODER_TAP_COUNT; ++tap) {
		if (parameters->precoder_coefficients[tap].real == INT16_MIN ||
		    parameters->precoder_coefficients[tap].imaginary ==
			    INT16_MIN)
			return false;
	}
	return true;
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

static struct v90_mp_parameters *mutable_record_parameters(
	struct v90_mp_record *record)
{
	return record->type == V90_MP_RECORD_TYPE0 ?
		       &record->parameters.type0 :
		       &record->parameters.type1.common;
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
	parameters = mutable_record_parameters(&limited);
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

static const struct v90_mp_parameters *record_parameters(
	const struct v90_mp_record *record)
{
	return record->type == V90_MP_RECORD_TYPE0 ?
		       &record->parameters.type0 :
		       &record->parameters.type1.common;
}

bool v90_mp_record_is_initial(const struct v90_mp_record *record)
{
	return v90_mp_record_is_valid(record) &&
	       !record_parameters(record)->acknowledges_cp;
}

bool v90_mp_record_acknowledges_cp(
	const struct v90_mp_record *record)
{
	return v90_mp_record_is_valid(record) &&
	       record_parameters(record)->acknowledges_cp;
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

static bool record_coefficients_equal(
	const struct v90_mp_record *left,
	const struct v90_mp_record *right)
{
	if (left->type == V90_MP_RECORD_TYPE0)
		return true;
	for (size_t tap = 0; tap < V34_PRECODER_TAP_COUNT; ++tap) {
		const struct v34_q14_coefficient *left_coefficient =
			&left->parameters.type1.precoder_coefficients[tap];
		const struct v34_q14_coefficient *right_coefficient =
			&right->parameters.type1.precoder_coefficients[tap];

		if (left_coefficient->real != right_coefficient->real ||
		    left_coefficient->imaginary !=
			    right_coefficient->imaginary)
			return false;
	}
	return true;
}

static bool records_equal(
	const struct v90_mp_record *left,
	const struct v90_mp_record *right,
	bool compare_acknowledgement)
{
	return v90_mp_record_is_valid(left) &&
	       v90_mp_record_is_valid(right) && left->type == right->type &&
	       common_parameters_equal(
		       record_parameters(left), record_parameters(right),
		       compare_acknowledgement) &&
	       record_coefficients_equal(left, right);
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

static const struct v90_mp_parameters *record_common(
	const struct v90_mp_record *record)
{
	return record->type == V90_MP_RECORD_TYPE0 ?
		       &record->parameters.type0 :
		       &record->parameters.type1.common;
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

static size_t coefficient_component_first_bit(
	size_t tap, bool imaginary)
{
	return V90_MP_TYPE1_FIRST_COEFFICIENT_BIT +
	       (tap * 2u + (imaginary ? 1u : 0u)) *
		       V90_MP_FIELD_STRIDE_BITS;
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
		encoded, record_common(record),
		record->type == V90_MP_RECORD_TYPE1);
	if (record->type == V90_MP_RECORD_TYPE1) {
		for (size_t tap = 0; tap < V34_PRECODER_TAP_COUNT; ++tap) {
			packed_msb_write_lsb_field(
				encoded,
				coefficient_component_first_bit(tap, false),
				V90_MP_COEFFICIENT_COMPONENT_BITS,
				(uint16_t)record->parameters.type1
					.precoder_coefficients[tap].real);
			packed_msb_write_lsb_field(
				encoded,
				coefficient_component_first_bit(tap, true),
				V90_MP_COEFFICIENT_COMPONENT_BITS,
				(uint16_t)record->parameters.type1
					.precoder_coefficients[tap].imaginary);
		}
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

static int16_t decode_twos_complement(uint16_t value)
{
	if (value <= INT16_MAX)
		return (int16_t)value;
	return (int16_t)(-1 - (int32_t)(UINT16_MAX - value));
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

	if (decoded.type == V90_MP_RECORD_TYPE0) {
		decode_common(sequence, &decoded.parameters.type0);
	} else {
		decode_common(sequence, &decoded.parameters.type1.common);
		for (size_t tap = 0; tap < V34_PRECODER_TAP_COUNT; ++tap) {
			decoded.parameters.type1.precoder_coefficients[tap].real =
				decode_twos_complement(
					(uint16_t)packed_msb_read_lsb_field(
						sequence,
						coefficient_component_first_bit(
							tap, false),
						V90_MP_COEFFICIENT_COMPONENT_BITS));
			decoded.parameters.type1.precoder_coefficients[tap]
				.imaginary =
				decode_twos_complement(
					(uint16_t)packed_msb_read_lsb_field(
						sequence,
						coefficient_component_first_bit(
							tap, true),
						V90_MP_COEFFICIENT_COMPONENT_BITS));
		}
	}
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
