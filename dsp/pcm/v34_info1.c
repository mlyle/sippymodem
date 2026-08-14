// SPDX-License-Identifier: BSD-3-Clause

#include "v34_info1.h"

#include "packed_bits.h"

#include <string.h>

enum {
	V34_INFO1_POWER_REDUCTION_FIRST_BIT = 0,
	V34_INFO1_POWER_REDUCTION_BITS = 3,
	V34_INFO1_ADDITIONAL_REDUCTION_FIRST_BIT = 3,
	V34_INFO1_ADDITIONAL_REDUCTION_BITS = 3,
	V34_INFO1_MD_LENGTH_FIRST_BIT = 6,
	V34_INFO1_MD_LENGTH_BITS = 7,
	V34_INFO1_PROBE_RESULT_FIRST_BIT = 13,
	V34_INFO1_PROBE_RESULT_BITS = 9,
	V34_INFO1_HIGH_CARRIER_BIT = 0,
	V34_INFO1_PREEMPHASIS_FIRST_BIT = 1,
	V34_INFO1_PREEMPHASIS_BITS = 4,
	V34_INFO1_DATA_RATE_FIRST_BIT = 5,
	V34_INFO1_DATA_RATE_BITS = 4,
	V34_INFO1C_FREQUENCY_OFFSET_FIRST_BIT = 67,
	V34_INFO1A_ANSWER_SYMBOL_RATE_FIRST_BIT = 22,
	V34_INFO1A_CALL_SYMBOL_RATE_FIRST_BIT = 25,
	V34_INFO1_SYMBOL_RATE_BITS = 3,
	V34_INFO1A_FREQUENCY_OFFSET_FIRST_BIT = 28,
	V34_INFO1_FREQUENCY_OFFSET_BITS = 10,
	V34_INFO1_FREQUENCY_OFFSET_MODULUS = 1024,
};

static bool common_is_valid(
	const struct v34_info1_common *common)
{
	return common->minimum_peer_power_reduction_db <=
			V34_INFO1_MAX_POWER_REDUCTION_DB &&
	       common->additional_peer_power_reduction_db <=
			V34_INFO1_MAX_POWER_REDUCTION_DB &&
	       common->local_md_length_35_ms_units <=
			V34_INFO1_MAX_MD_LENGTH_35_MS_UNITS &&
	       common->frequency_offset_1_50_hz_units >=
			V34_INFO1_FREQUENCY_OFFSET_UNAVAILABLE &&
	       common->frequency_offset_1_50_hz_units <=
			V34_INFO1_MAX_FREQUENCY_OFFSET_1_50_HZ_UNITS;
}

static bool probe_result_is_valid(
	const struct v34_info1_probe_result *result)
{
	return result->preemphasis_filter_index <=
			V34_INFO1_MAX_PREEMPHASIS_FILTER_INDEX &&
	       result->projected_data_rate_2400_bps_units <=
			V34_INFO1_MAX_DATA_RATE_2400_BPS_UNITS;
}

bool v34_info1c_is_valid(
	const struct v34_info1c *info1c)
{
	if (info1c == NULL || !common_is_valid(&info1c->common))
		return false;
	for (size_t index = 0; index < V34_SYMBOL_RATE_COUNT;
	     ++index)
		if (!probe_result_is_valid(&info1c->answer_transmit[index]))
			return false;
	return true;
}

static bool symbol_rate_is_valid(enum v34_symbol_rate rate)
{
	return (unsigned int)rate < V34_SYMBOL_RATE_COUNT;
}

bool v34_info1a_is_valid(
	const struct v34_info1a *info1a)
{
	return info1a != NULL && common_is_valid(&info1a->common) &&
	       probe_result_is_valid(&info1a->call_transmit) &&
	       symbol_rate_is_valid(info1a->answer_to_call_symbol_rate) &&
	       symbol_rate_is_valid(info1a->call_to_answer_symbol_rate);
}

static void encode_common_prefix(
	uint8_t *payload, const struct v34_info1_common *common)
{
	packed_msb_write_lsb_field(
		payload, V34_INFO1_POWER_REDUCTION_FIRST_BIT,
		V34_INFO1_POWER_REDUCTION_BITS,
		common->minimum_peer_power_reduction_db);
	packed_msb_write_lsb_field(
		payload, V34_INFO1_ADDITIONAL_REDUCTION_FIRST_BIT,
		V34_INFO1_ADDITIONAL_REDUCTION_BITS,
		common->additional_peer_power_reduction_db);
	packed_msb_write_lsb_field(
		payload, V34_INFO1_MD_LENGTH_FIRST_BIT,
		V34_INFO1_MD_LENGTH_BITS, common->local_md_length_35_ms_units);
}

static void encode_frequency_offset(
	uint8_t *payload, size_t first_bit,
	const struct v34_info1_common *common)
{
	uint16_t frequency_offset =
		(uint16_t)(common->frequency_offset_1_50_hz_units < 0 ?
				   common->frequency_offset_1_50_hz_units +
					   V34_INFO1_FREQUENCY_OFFSET_MODULUS :
				   common->frequency_offset_1_50_hz_units);

	packed_msb_write_lsb_field(
		payload, first_bit,
		V34_INFO1_FREQUENCY_OFFSET_BITS, frequency_offset);
}

static void encode_probe_result(
	uint8_t *payload, size_t first_bit,
	const struct v34_info1_probe_result *result)
{
	packed_msb_set_bit(
		payload, first_bit + V34_INFO1_HIGH_CARRIER_BIT,
		result->high_carrier);
	packed_msb_write_lsb_field(
		payload, first_bit + V34_INFO1_PREEMPHASIS_FIRST_BIT,
		V34_INFO1_PREEMPHASIS_BITS, result->preemphasis_filter_index);
	packed_msb_write_lsb_field(
		payload, first_bit + V34_INFO1_DATA_RATE_FIRST_BIT,
		V34_INFO1_DATA_RATE_BITS,
		result->projected_data_rate_2400_bps_units);
}

enum v34_info1_result v34_info1c_encode(
	const struct v34_info1c *info1c, uint8_t *payload,
	size_t payload_capacity)
{
	uint8_t encoded[V34_INFO1C_PAYLOAD_BYTES] = { 0 };

	if (info1c == NULL || payload == NULL ||
	    payload_capacity < V34_INFO1C_PAYLOAD_BYTES)
		return V34_INFO1_INVALID_ARGUMENT;
	if (!v34_info1c_is_valid(info1c))
		return V34_INFO1_INVALID_PARAMETERS;
	encode_common_prefix(encoded, &info1c->common);
	for (size_t index = 0; index < V34_SYMBOL_RATE_COUNT;
	     ++index)
		encode_probe_result(
			encoded,
			V34_INFO1_PROBE_RESULT_FIRST_BIT +
				index * V34_INFO1_PROBE_RESULT_BITS,
			&info1c->answer_transmit[index]);
	encode_frequency_offset(
		encoded, V34_INFO1C_FREQUENCY_OFFSET_FIRST_BIT,
		&info1c->common);
	memcpy(payload, encoded, sizeof(encoded));
	return V34_INFO1_OK;
}

static int16_t decode_frequency_offset(
	const uint8_t *payload, size_t first_bit)
{
	uint16_t encoded = (uint16_t)packed_msb_read_lsb_field(
		payload, first_bit, V34_INFO1_FREQUENCY_OFFSET_BITS);

	return encoded >= V34_INFO1_FREQUENCY_OFFSET_MODULUS / 2 ?
		       (int16_t)((int)encoded -
				 V34_INFO1_FREQUENCY_OFFSET_MODULUS) :
		       (int16_t)encoded;
}

static struct v34_info1_common decode_common(
	const uint8_t *payload, size_t frequency_offset_first_bit)
{
	return (struct v34_info1_common){
		.minimum_peer_power_reduction_db =
			(uint8_t)packed_msb_read_lsb_field(
				payload, V34_INFO1_POWER_REDUCTION_FIRST_BIT,
				V34_INFO1_POWER_REDUCTION_BITS),
		.additional_peer_power_reduction_db =
			(uint8_t)packed_msb_read_lsb_field(
				payload,
				V34_INFO1_ADDITIONAL_REDUCTION_FIRST_BIT,
				V34_INFO1_ADDITIONAL_REDUCTION_BITS),
		.local_md_length_35_ms_units =
			(uint8_t)packed_msb_read_lsb_field(
				payload, V34_INFO1_MD_LENGTH_FIRST_BIT,
				V34_INFO1_MD_LENGTH_BITS),
		.frequency_offset_1_50_hz_units = decode_frequency_offset(
			payload, frequency_offset_first_bit),
	};
}

static struct v34_info1_probe_result decode_probe_result(
	const uint8_t *payload, size_t first_bit)
{
	return (struct v34_info1_probe_result){
		.high_carrier = packed_msb_bit(
			payload, first_bit + V34_INFO1_HIGH_CARRIER_BIT) != 0,
		.preemphasis_filter_index =
			(uint8_t)packed_msb_read_lsb_field(
				payload,
				first_bit + V34_INFO1_PREEMPHASIS_FIRST_BIT,
				V34_INFO1_PREEMPHASIS_BITS),
		.projected_data_rate_2400_bps_units =
			(uint8_t)packed_msb_read_lsb_field(
				payload, first_bit + V34_INFO1_DATA_RATE_FIRST_BIT,
				V34_INFO1_DATA_RATE_BITS),
	};
}

enum v34_info1_result v34_info1c_decode(
	const uint8_t *payload, size_t payload_capacity,
	size_t payload_bit_count, struct v34_info1c *info1c)
{
	struct v34_info1c decoded = { 0 };

	if (payload == NULL || info1c == NULL ||
	    payload_capacity < V34_INFO1C_PAYLOAD_BYTES)
		return V34_INFO1_INVALID_ARGUMENT;
	if (payload_bit_count != V34_INFO1C_PAYLOAD_BITS ||
	    !packed_msb_tail_is_zero(
		    payload, payload_bit_count, V34_INFO1C_PAYLOAD_BYTES))
		return V34_INFO1_INVALID_PAYLOAD;
	decoded.common = decode_common(
		payload, V34_INFO1C_FREQUENCY_OFFSET_FIRST_BIT);
	for (size_t index = 0; index < V34_SYMBOL_RATE_COUNT;
	     ++index)
		decoded.answer_transmit[index] = decode_probe_result(
			payload,
			V34_INFO1_PROBE_RESULT_FIRST_BIT +
				index * V34_INFO1_PROBE_RESULT_BITS);
	if (!v34_info1c_is_valid(&decoded))
		return V34_INFO1_INVALID_PAYLOAD;
	*info1c = decoded;
	return V34_INFO1_OK;
}

enum v34_info1_result v34_info1a_encode(
	const struct v34_info1a *info1a, uint8_t *payload,
	size_t payload_capacity)
{
	uint8_t encoded[V34_INFO1A_PAYLOAD_BYTES] = { 0 };
	if (info1a == NULL || payload == NULL ||
	    payload_capacity < V34_INFO1A_PAYLOAD_BYTES)
		return V34_INFO1_INVALID_ARGUMENT;
	if (!v34_info1a_is_valid(info1a))
		return V34_INFO1_INVALID_PARAMETERS;
	encode_common_prefix(encoded, &info1a->common);
	encode_probe_result(
		encoded, V34_INFO1_PROBE_RESULT_FIRST_BIT,
		&info1a->call_transmit);
	packed_msb_write_lsb_field(
		encoded, V34_INFO1A_ANSWER_SYMBOL_RATE_FIRST_BIT,
		V34_INFO1_SYMBOL_RATE_BITS,
		(uint16_t)info1a->answer_to_call_symbol_rate);
	packed_msb_write_lsb_field(
		encoded, V34_INFO1A_CALL_SYMBOL_RATE_FIRST_BIT,
		V34_INFO1_SYMBOL_RATE_BITS,
		(uint16_t)info1a->call_to_answer_symbol_rate);
	encode_frequency_offset(
		encoded, V34_INFO1A_FREQUENCY_OFFSET_FIRST_BIT,
		&info1a->common);
	memcpy(payload, encoded, sizeof(encoded));
	return V34_INFO1_OK;
}

enum v34_info1_result v34_info1a_decode(
	const uint8_t *payload, size_t payload_capacity,
	size_t payload_bit_count, struct v34_info1a *info1a)
{
	struct v34_info1a decoded = { 0 };

	if (payload == NULL || info1a == NULL ||
	    payload_capacity < V34_INFO1A_PAYLOAD_BYTES)
		return V34_INFO1_INVALID_ARGUMENT;
	if (payload_bit_count != V34_INFO1A_PAYLOAD_BITS ||
	    !packed_msb_tail_is_zero(
		    payload, payload_bit_count, V34_INFO1A_PAYLOAD_BYTES))
		return V34_INFO1_INVALID_PAYLOAD;
	decoded.common = decode_common(
		payload, V34_INFO1A_FREQUENCY_OFFSET_FIRST_BIT);
	decoded.call_transmit = decode_probe_result(
		payload, V34_INFO1_PROBE_RESULT_FIRST_BIT);
	decoded.answer_to_call_symbol_rate =
		(enum v34_symbol_rate)packed_msb_read_lsb_field(
			payload, V34_INFO1A_ANSWER_SYMBOL_RATE_FIRST_BIT,
			V34_INFO1_SYMBOL_RATE_BITS);
	decoded.call_to_answer_symbol_rate =
		(enum v34_symbol_rate)packed_msb_read_lsb_field(
			payload, V34_INFO1A_CALL_SYMBOL_RATE_FIRST_BIT,
			V34_INFO1_SYMBOL_RATE_BITS);
	if (!v34_info1a_is_valid(&decoded))
		return V34_INFO1_INVALID_PAYLOAD;
	*info1a = decoded;
	return V34_INFO1_OK;
}

const char *v34_info1_result_name(
	enum v34_info1_result result)
{
	switch (result) {
	case V34_INFO1_OK:
		return "ok";
	case V34_INFO1_INVALID_ARGUMENT:
		return "invalid argument";
	case V34_INFO1_INVALID_PARAMETERS:
		return "invalid parameters";
	case V34_INFO1_INVALID_PAYLOAD:
		return "invalid payload";
	}
	return "unknown";
}
