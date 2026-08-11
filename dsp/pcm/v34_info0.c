// SPDX-License-Identifier: BSD-3-Clause

#include "v34_info0.h"

#include <string.h>

enum {
	V34_INFO0_SYMBOL_RATE_2743_BIT = 0,
	V34_INFO0_SYMBOL_RATE_2800_BIT = 1,
	V34_INFO0_SYMBOL_RATE_3429_BIT = 2,
	V34_INFO0_LOW_CARRIER_3000_BIT = 3,
	V34_INFO0_HIGH_CARRIER_3000_BIT = 4,
	V34_INFO0_LOW_CARRIER_3200_BIT = 5,
	V34_INFO0_HIGH_CARRIER_3200_BIT = 6,
	V34_INFO0_SYMBOL_RATE_3429_TRANSMIT_PERMITTED_BIT = 7,
	V34_INFO0_TRANSMIT_POWER_REDUCTION_BIT = 8,
	V34_INFO0_MAXIMUM_SYMBOL_RATE_DIFFERENCE_STEPS_FIRST_BIT = 9,
	V34_INFO0_MAXIMUM_SYMBOL_RATE_DIFFERENCE_STEPS_BITS = 3,
	V34_INFO0_CME_MODEM_BIT = 12,
	V34_INFO0_CONSTELLATION_1664_BIT = 13,
	V34_INFO0_TRANSMIT_CLOCK_FIRST_BIT = 14,
	V34_INFO0_TRANSMIT_CLOCK_BITS = 2,
	V34_INFO0_ACKNOWLEDGMENT_BIT = 16,
};

static bool clock_source_is_valid(
	enum sippymodem_v34_info0_clock_source source)
{
	return source == SIPPYMODEM_V34_INFO0_CLOCK_INTERNAL ||
	       source == SIPPYMODEM_V34_INFO0_CLOCK_RECEIVE_TIMING ||
	       source == SIPPYMODEM_V34_INFO0_CLOCK_EXTERNAL;
}

bool sippymodem_v34_info0_is_valid(
	const struct sippymodem_v34_info0 *info0)
{
	return info0 != NULL &&
	       info0->maximum_symbol_rate_difference_steps <=
		       SIPPYMODEM_V34_INFO0_MAX_SYMBOL_RATE_DIFFERENCE_STEPS &&
	       clock_source_is_valid(info0->transmit_clock_source);
}

static uint8_t payload_bit(const uint8_t *payload, size_t bit_index)
{
	return (uint8_t)((payload[bit_index / 8u] >>
			  (7u - bit_index % 8u)) &
			 1u);
}

static void set_payload_bit(
	uint8_t *payload, size_t bit_index, bool value)
{
	if (value)
		payload[bit_index / 8u] |=
			(uint8_t)(1u << (7u - bit_index % 8u));
}

static uint8_t decode_lsb_first_field(
	const uint8_t *payload, size_t first_bit, size_t bit_count)
{
	uint8_t value = 0;

	for (size_t index = 0; index < bit_count; ++index)
		value |= (uint8_t)(payload_bit(payload, first_bit + index)
				   << index);
	return value;
}

static void encode_lsb_first_field(
	uint8_t *payload, size_t first_bit, size_t bit_count, uint8_t value)
{
	for (size_t index = 0; index < bit_count; ++index)
		set_payload_bit(payload, first_bit + index,
				(value & (uint8_t)(1u << index)) != 0);
}

enum sippymodem_v34_info0_result sippymodem_v34_info0_encode(
	const struct sippymodem_v34_info0 *info0, uint8_t *payload,
	size_t payload_capacity)
{
	uint8_t encoded[SIPPYMODEM_V34_INFO0_PAYLOAD_BYTES] = { 0 };

	if (info0 == NULL || payload == NULL ||
	    payload_capacity < SIPPYMODEM_V34_INFO0_PAYLOAD_BYTES)
		return SIPPYMODEM_V34_INFO0_INVALID_ARGUMENT;
	if (!sippymodem_v34_info0_is_valid(info0))
		return SIPPYMODEM_V34_INFO0_INVALID_CAPABILITIES;

	set_payload_bit(encoded, V34_INFO0_SYMBOL_RATE_2743_BIT,
			info0->symbol_rate_2743_supported);
	set_payload_bit(encoded, V34_INFO0_SYMBOL_RATE_2800_BIT,
			info0->symbol_rate_2800_supported);
	set_payload_bit(encoded, V34_INFO0_SYMBOL_RATE_3429_BIT,
			info0->symbol_rate_3429_supported);
	set_payload_bit(encoded, V34_INFO0_LOW_CARRIER_3000_BIT,
			info0->low_carrier_3000_supported);
	set_payload_bit(encoded, V34_INFO0_HIGH_CARRIER_3000_BIT,
			info0->high_carrier_3000_supported);
	set_payload_bit(encoded, V34_INFO0_LOW_CARRIER_3200_BIT,
			info0->low_carrier_3200_supported);
	set_payload_bit(encoded, V34_INFO0_HIGH_CARRIER_3200_BIT,
			info0->high_carrier_3200_supported);
	set_payload_bit(
		encoded, V34_INFO0_SYMBOL_RATE_3429_TRANSMIT_PERMITTED_BIT,
		info0->symbol_rate_3429_transmit_permitted);
	set_payload_bit(encoded, V34_INFO0_TRANSMIT_POWER_REDUCTION_BIT,
			info0->transmit_power_reduction_supported);
	encode_lsb_first_field(
		encoded,
		V34_INFO0_MAXIMUM_SYMBOL_RATE_DIFFERENCE_STEPS_FIRST_BIT,
		V34_INFO0_MAXIMUM_SYMBOL_RATE_DIFFERENCE_STEPS_BITS,
		info0->maximum_symbol_rate_difference_steps);
	set_payload_bit(
		encoded, V34_INFO0_CME_MODEM_BIT, info0->cme_modem);
	set_payload_bit(encoded, V34_INFO0_CONSTELLATION_1664_BIT,
			info0->constellation_1664_supported);
	encode_lsb_first_field(
		encoded, V34_INFO0_TRANSMIT_CLOCK_FIRST_BIT,
		V34_INFO0_TRANSMIT_CLOCK_BITS,
		(uint8_t)info0->transmit_clock_source);
	set_payload_bit(encoded, V34_INFO0_ACKNOWLEDGMENT_BIT,
			info0->acknowledges_peer_info0);

	memcpy(payload, encoded, sizeof(encoded));
	return SIPPYMODEM_V34_INFO0_OK;
}

static bool payload_padding_is_zero(const uint8_t *payload)
{
	return (payload[SIPPYMODEM_V34_INFO0_PAYLOAD_BYTES - 1u] &
		UINT8_C(0x7f)) == 0;
}

enum sippymodem_v34_info0_result sippymodem_v34_info0_decode(
	const uint8_t *payload, size_t payload_capacity,
	size_t payload_bit_count, struct sippymodem_v34_info0 *info0)
{
	struct sippymodem_v34_info0 decoded = { 0 };

	if (payload == NULL || info0 == NULL ||
	    payload_capacity < SIPPYMODEM_V34_INFO0_PAYLOAD_BYTES)
		return SIPPYMODEM_V34_INFO0_INVALID_ARGUMENT;
	if (payload_bit_count != SIPPYMODEM_V34_INFO0_PAYLOAD_BITS ||
	    !payload_padding_is_zero(payload))
		return SIPPYMODEM_V34_INFO0_INVALID_PAYLOAD;

	decoded.symbol_rate_2743_supported =
		payload_bit(payload, V34_INFO0_SYMBOL_RATE_2743_BIT) != 0;
	decoded.symbol_rate_2800_supported =
		payload_bit(payload, V34_INFO0_SYMBOL_RATE_2800_BIT) != 0;
	decoded.symbol_rate_3429_supported =
		payload_bit(payload, V34_INFO0_SYMBOL_RATE_3429_BIT) != 0;
	decoded.low_carrier_3000_supported =
		payload_bit(payload, V34_INFO0_LOW_CARRIER_3000_BIT) != 0;
	decoded.high_carrier_3000_supported =
		payload_bit(payload, V34_INFO0_HIGH_CARRIER_3000_BIT) != 0;
	decoded.low_carrier_3200_supported =
		payload_bit(payload, V34_INFO0_LOW_CARRIER_3200_BIT) != 0;
	decoded.high_carrier_3200_supported =
		payload_bit(payload, V34_INFO0_HIGH_CARRIER_3200_BIT) != 0;
	decoded.symbol_rate_3429_transmit_permitted =
		payload_bit(
			payload,
			V34_INFO0_SYMBOL_RATE_3429_TRANSMIT_PERMITTED_BIT) != 0;
	decoded.transmit_power_reduction_supported =
		payload_bit(payload, V34_INFO0_TRANSMIT_POWER_REDUCTION_BIT) !=
		0;
	decoded.maximum_symbol_rate_difference_steps =
		decode_lsb_first_field(
			payload,
			V34_INFO0_MAXIMUM_SYMBOL_RATE_DIFFERENCE_STEPS_FIRST_BIT,
			V34_INFO0_MAXIMUM_SYMBOL_RATE_DIFFERENCE_STEPS_BITS);
	decoded.cme_modem =
		payload_bit(payload, V34_INFO0_CME_MODEM_BIT) != 0;
	decoded.constellation_1664_supported =
		payload_bit(payload, V34_INFO0_CONSTELLATION_1664_BIT) != 0;
	decoded.transmit_clock_source =
		(enum sippymodem_v34_info0_clock_source)
			decode_lsb_first_field(
				payload, V34_INFO0_TRANSMIT_CLOCK_FIRST_BIT,
				V34_INFO0_TRANSMIT_CLOCK_BITS);
	decoded.acknowledges_peer_info0 =
		payload_bit(payload, V34_INFO0_ACKNOWLEDGMENT_BIT) != 0;

	if (!sippymodem_v34_info0_is_valid(&decoded))
		return SIPPYMODEM_V34_INFO0_INVALID_PAYLOAD;
	*info0 = decoded;
	return SIPPYMODEM_V34_INFO0_OK;
}

const char *sippymodem_v34_info0_result_name(
	enum sippymodem_v34_info0_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_INFO0_OK:
		return "ok";
	case SIPPYMODEM_V34_INFO0_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_V34_INFO0_INVALID_CAPABILITIES:
		return "invalid capabilities";
	case SIPPYMODEM_V34_INFO0_INVALID_PAYLOAD:
		return "invalid payload";
	}
	return "unknown";
}
