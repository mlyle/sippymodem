// SPDX-License-Identifier: BSD-3-Clause

#include "v90_info0.h"

#include <string.h>

enum {
	SYMBOL_RATE_2743_BIT = 0,
	SYMBOL_RATE_2800_BIT = 1,
	SYMBOL_RATE_3429_BIT = 2,
	LOW_CARRIER_3000_BIT = 3,
	HIGH_CARRIER_3000_BIT = 4,
	LOW_CARRIER_3200_BIT = 5,
	HIGH_CARRIER_3200_BIT = 6,
	SYMBOL_RATE_3429_PERMITTED_BIT = 7,
	POWER_REDUCTION_BIT = 8,
	MAXIMUM_SYMBOL_RATE_DIFFERENCE_FIRST_BIT = 9,
	MAXIMUM_SYMBOL_RATE_DIFFERENCE_BITS = 3,
	CME_MODEM_BIT = 12,
	CONSTELLATION_1664_BIT = 13,
	ACKNOWLEDGMENT_BIT = 16,
	NOMINAL_TRANSMIT_POWER_FIRST_BIT = 17,
	NOMINAL_TRANSMIT_POWER_BITS = 4,
	MAXIMUM_TRANSMIT_POWER_FIRST_BIT = 21,
	MAXIMUM_TRANSMIT_POWER_BITS = 5,
	MEASURE_AT_CODEC_BIT = 26,
	PCM_LAW_BIT = 27,
	UPSTREAM_SYMBOL_RATE_3429_BIT = 28,
	NOMINAL_TRANSMIT_POWER_MAX = 15,
	MAXIMUM_TRANSMIT_POWER_MAX = 31,
};

static uint8_t payload_bit(const uint8_t *payload, size_t index)
{
	return (uint8_t)((payload[index / 8u] >>
			  (7u - index % 8u)) &
			 1u);
}

static void set_payload_bit(uint8_t *payload, size_t index, bool value)
{
	if (value)
		payload[index / 8u] |=
			(uint8_t)(1u << (7u - index % 8u));
}

static uint8_t decode_field(
	const uint8_t *payload, size_t first_bit, size_t bit_count)
{
	uint8_t value = 0;

	for (size_t index = 0; index < bit_count; ++index)
		value |= (uint8_t)(payload_bit(payload, first_bit + index) <<
				   index);
	return value;
}

static void encode_field(
	uint8_t *payload, size_t first_bit, size_t bit_count, uint8_t value)
{
	for (size_t index = 0; index < bit_count; ++index)
		set_payload_bit(
			payload, first_bit + index,
			(value & (uint8_t)(1u << index)) != 0);
}

bool v90_info0_is_valid(const struct v90_info0 *info0)
{
	return info0 != NULL &&
	       info0->maximum_symbol_rate_difference_steps <=
		       V90_INFO0_MAX_SYMBOL_RATE_DIFFERENCE_STEPS;
}

bool v90_info0d_is_valid(const struct v90_info0d *info0d)
{
	return info0d != NULL && v90_info0_is_valid(&info0d->common) &&
	       info0d->nominal_transmit_power <=
		       NOMINAL_TRANSMIT_POWER_MAX &&
	       info0d->maximum_transmit_power <=
		       MAXIMUM_TRANSMIT_POWER_MAX &&
	       (info0d->pcm_law == V90_PCM_MU_LAW ||
		info0d->pcm_law == V90_PCM_A_LAW);
}

static void encode_common(
	const struct v90_info0 *info0, uint8_t *payload)
{
	set_payload_bit(payload, SYMBOL_RATE_2743_BIT,
			info0->symbol_rate_2743_supported);
	set_payload_bit(payload, SYMBOL_RATE_2800_BIT,
			info0->symbol_rate_2800_supported);
	set_payload_bit(payload, SYMBOL_RATE_3429_BIT,
			info0->symbol_rate_3429_supported);
	set_payload_bit(payload, LOW_CARRIER_3000_BIT,
			info0->low_carrier_3000_supported);
	set_payload_bit(payload, HIGH_CARRIER_3000_BIT,
			info0->high_carrier_3000_supported);
	set_payload_bit(payload, LOW_CARRIER_3200_BIT,
			info0->low_carrier_3200_supported);
	set_payload_bit(payload, HIGH_CARRIER_3200_BIT,
			info0->high_carrier_3200_supported);
	set_payload_bit(payload, SYMBOL_RATE_3429_PERMITTED_BIT,
			info0->symbol_rate_3429_transmit_permitted);
	set_payload_bit(payload, POWER_REDUCTION_BIT,
			info0->transmit_power_reduction_supported);
	encode_field(
		payload, MAXIMUM_SYMBOL_RATE_DIFFERENCE_FIRST_BIT,
		MAXIMUM_SYMBOL_RATE_DIFFERENCE_BITS,
		info0->maximum_symbol_rate_difference_steps);
	set_payload_bit(payload, CME_MODEM_BIT, info0->cme_modem);
	set_payload_bit(payload, CONSTELLATION_1664_BIT,
			info0->constellation_1664_supported);
	set_payload_bit(payload, ACKNOWLEDGMENT_BIT,
			info0->acknowledges_peer_info0);
}

static void decode_common(
	const uint8_t *payload, struct v90_info0 *info0)
{
	*info0 = (struct v90_info0){
		.symbol_rate_2743_supported =
			payload_bit(payload, SYMBOL_RATE_2743_BIT) != 0,
		.symbol_rate_2800_supported =
			payload_bit(payload, SYMBOL_RATE_2800_BIT) != 0,
		.symbol_rate_3429_supported =
			payload_bit(payload, SYMBOL_RATE_3429_BIT) != 0,
		.low_carrier_3000_supported =
			payload_bit(payload, LOW_CARRIER_3000_BIT) != 0,
		.high_carrier_3000_supported =
			payload_bit(payload, HIGH_CARRIER_3000_BIT) != 0,
		.low_carrier_3200_supported =
			payload_bit(payload, LOW_CARRIER_3200_BIT) != 0,
		.high_carrier_3200_supported =
			payload_bit(payload, HIGH_CARRIER_3200_BIT) != 0,
		.symbol_rate_3429_transmit_permitted =
			payload_bit(payload, SYMBOL_RATE_3429_PERMITTED_BIT) != 0,
		.transmit_power_reduction_supported =
			payload_bit(payload, POWER_REDUCTION_BIT) != 0,
		.maximum_symbol_rate_difference_steps = decode_field(
			payload, MAXIMUM_SYMBOL_RATE_DIFFERENCE_FIRST_BIT,
			MAXIMUM_SYMBOL_RATE_DIFFERENCE_BITS),
		.cme_modem = payload_bit(payload, CME_MODEM_BIT) != 0,
		.constellation_1664_supported =
			payload_bit(payload, CONSTELLATION_1664_BIT) != 0,
		.acknowledges_peer_info0 =
			payload_bit(payload, ACKNOWLEDGMENT_BIT) != 0,
	};
}

static bool padding_is_zero(
	const uint8_t *payload, size_t payload_bit_count)
{
	size_t remainder = payload_bit_count % 8u;

	return remainder == 0 ||
	       (payload[payload_bit_count / 8u] &
		(uint8_t)((1u << (8u - remainder)) - 1u)) == 0;
}

enum v90_info0_result v90_info0a_encode(
	const struct v90_info0 *info0a, uint8_t *payload,
	size_t payload_capacity)
{
	uint8_t encoded[V90_INFO0A_PAYLOAD_BYTES] = { 0 };

	if (info0a == NULL || payload == NULL ||
	    payload_capacity < sizeof(encoded))
		return V90_INFO0_INVALID_ARGUMENT;
	if (!v90_info0_is_valid(info0a))
		return V90_INFO0_INVALID_CAPABILITIES;
	encode_common(info0a, encoded);
	memcpy(payload, encoded, sizeof(encoded));
	return V90_INFO0_OK;
}

enum v90_info0_result v90_info0a_decode(
	const uint8_t *payload, size_t payload_capacity,
	size_t payload_bit_count, struct v90_info0 *info0a)
{
	struct v90_info0 decoded;

	if (payload == NULL || info0a == NULL ||
	    payload_capacity < V90_INFO0A_PAYLOAD_BYTES)
		return V90_INFO0_INVALID_ARGUMENT;
	if (payload_bit_count != V90_INFO0A_PAYLOAD_BITS ||
	    !padding_is_zero(payload, payload_bit_count))
		return V90_INFO0_INVALID_PAYLOAD;
	decode_common(payload, &decoded);
	if (!v90_info0_is_valid(&decoded))
		return V90_INFO0_INVALID_PAYLOAD;
	*info0a = decoded;
	return V90_INFO0_OK;
}

enum v90_info0_result v90_info0d_encode(
	const struct v90_info0d *info0d, uint8_t *payload,
	size_t payload_capacity)
{
	uint8_t encoded[V90_INFO0D_PAYLOAD_BYTES] = { 0 };

	if (info0d == NULL || payload == NULL ||
	    payload_capacity < sizeof(encoded))
		return V90_INFO0_INVALID_ARGUMENT;
	if (!v90_info0d_is_valid(info0d))
		return V90_INFO0_INVALID_CAPABILITIES;
	encode_common(&info0d->common, encoded);
	encode_field(
		encoded, NOMINAL_TRANSMIT_POWER_FIRST_BIT,
		NOMINAL_TRANSMIT_POWER_BITS, info0d->nominal_transmit_power);
	encode_field(
		encoded, MAXIMUM_TRANSMIT_POWER_FIRST_BIT,
		MAXIMUM_TRANSMIT_POWER_BITS, info0d->maximum_transmit_power);
	set_payload_bit(encoded, MEASURE_AT_CODEC_BIT,
			info0d->measure_power_at_codec_output);
	set_payload_bit(encoded, PCM_LAW_BIT,
			info0d->pcm_law == V90_PCM_A_LAW);
	set_payload_bit(encoded, UPSTREAM_SYMBOL_RATE_3429_BIT,
			info0d->upstream_symbol_rate_3429_supported);
	memcpy(payload, encoded, sizeof(encoded));
	return V90_INFO0_OK;
}

enum v90_info0_result v90_info0d_decode(
	const uint8_t *payload, size_t payload_capacity,
	size_t payload_bit_count, struct v90_info0d *info0d)
{
	struct v90_info0d decoded = { 0 };

	if (payload == NULL || info0d == NULL ||
	    payload_capacity < V90_INFO0D_PAYLOAD_BYTES)
		return V90_INFO0_INVALID_ARGUMENT;
	if (payload_bit_count != V90_INFO0D_PAYLOAD_BITS ||
	    !padding_is_zero(payload, payload_bit_count))
		return V90_INFO0_INVALID_PAYLOAD;
	decode_common(payload, &decoded.common);
	decoded.nominal_transmit_power = decode_field(
		payload, NOMINAL_TRANSMIT_POWER_FIRST_BIT,
		NOMINAL_TRANSMIT_POWER_BITS);
	decoded.maximum_transmit_power = decode_field(
		payload, MAXIMUM_TRANSMIT_POWER_FIRST_BIT,
		MAXIMUM_TRANSMIT_POWER_BITS);
	decoded.measure_power_at_codec_output =
		payload_bit(payload, MEASURE_AT_CODEC_BIT) != 0;
	decoded.pcm_law = payload_bit(payload, PCM_LAW_BIT) != 0 ?
				  V90_PCM_A_LAW : V90_PCM_MU_LAW;
	decoded.upstream_symbol_rate_3429_supported =
		payload_bit(payload, UPSTREAM_SYMBOL_RATE_3429_BIT) != 0;
	if (!v90_info0d_is_valid(&decoded))
		return V90_INFO0_INVALID_PAYLOAD;
	*info0d = decoded;
	return V90_INFO0_OK;
}

const char *v90_info0_result_name(enum v90_info0_result result)
{
	switch (result) {
	case V90_INFO0_OK:
		return "ok";
	case V90_INFO0_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_INFO0_INVALID_CAPABILITIES:
		return "invalid capabilities";
	case V90_INFO0_INVALID_PAYLOAD:
		return "invalid payload";
	}
	return "unknown";
}
