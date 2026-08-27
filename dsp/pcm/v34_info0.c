// SPDX-License-Identifier: BSD-3-Clause

#include "v34_info0.h"

#include "packed_bits.h"

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
	enum v34_info0_clock_source source)
{
	return source == V34_INFO0_CLOCK_INTERNAL ||
	       source == V34_INFO0_CLOCK_RECEIVE_TIMING ||
	       source == V34_INFO0_CLOCK_EXTERNAL;
}

bool v34_info0_is_valid(
	const struct v34_info0 *info0)
{
	return info0 != NULL &&
	       info0->maximum_symbol_rate_difference_steps <=
		       V34_INFO0_MAX_SYMBOL_RATE_DIFFERENCE_STEPS &&
	       clock_source_is_valid(info0->transmit_clock_source);
}

enum v34_info0_result v34_info0_encode(
	const struct v34_info0 *info0, uint8_t *payload,
	size_t payload_capacity)
{
	uint8_t encoded[V34_INFO0_PAYLOAD_BYTES] = { 0 };

	if (info0 == NULL || payload == NULL ||
	    payload_capacity < V34_INFO0_PAYLOAD_BYTES)
		return V34_INFO0_INVALID_ARGUMENT;
	if (!v34_info0_is_valid(info0))
		return V34_INFO0_INVALID_CAPABILITIES;

	packed_msb_set_bit(encoded, V34_INFO0_SYMBOL_RATE_2743_BIT,
			info0->symbol_rate_2743_supported);
	packed_msb_set_bit(encoded, V34_INFO0_SYMBOL_RATE_2800_BIT,
			info0->symbol_rate_2800_supported);
	packed_msb_set_bit(encoded, V34_INFO0_SYMBOL_RATE_3429_BIT,
			info0->symbol_rate_3429_supported);
	packed_msb_set_bit(encoded, V34_INFO0_LOW_CARRIER_3000_BIT,
			info0->low_carrier_3000_supported);
	packed_msb_set_bit(encoded, V34_INFO0_HIGH_CARRIER_3000_BIT,
			info0->high_carrier_3000_supported);
	packed_msb_set_bit(encoded, V34_INFO0_LOW_CARRIER_3200_BIT,
			info0->low_carrier_3200_supported);
	packed_msb_set_bit(encoded, V34_INFO0_HIGH_CARRIER_3200_BIT,
			info0->high_carrier_3200_supported);
	packed_msb_set_bit(
		encoded, V34_INFO0_SYMBOL_RATE_3429_TRANSMIT_PERMITTED_BIT,
		info0->symbol_rate_3429_transmit_permitted);
	packed_msb_set_bit(encoded, V34_INFO0_TRANSMIT_POWER_REDUCTION_BIT,
			info0->transmit_power_reduction_supported);
	packed_msb_write_lsb_field(
		encoded,
		V34_INFO0_MAXIMUM_SYMBOL_RATE_DIFFERENCE_STEPS_FIRST_BIT,
		V34_INFO0_MAXIMUM_SYMBOL_RATE_DIFFERENCE_STEPS_BITS,
		info0->maximum_symbol_rate_difference_steps);
	packed_msb_set_bit(
		encoded, V34_INFO0_CME_MODEM_BIT, info0->cme_modem);
	packed_msb_set_bit(encoded, V34_INFO0_CONSTELLATION_1664_BIT,
			info0->constellation_1664_supported);
	packed_msb_write_lsb_field(
		encoded, V34_INFO0_TRANSMIT_CLOCK_FIRST_BIT,
		V34_INFO0_TRANSMIT_CLOCK_BITS,
		(uint8_t)info0->transmit_clock_source);
	packed_msb_set_bit(encoded, V34_INFO0_ACKNOWLEDGMENT_BIT,
			info0->acknowledges_peer_info0);

	memcpy(payload, encoded, sizeof(encoded));
	return V34_INFO0_OK;
}

enum v34_info0_result v34_info0_decode(
	const uint8_t *payload, size_t payload_capacity,
	size_t payload_bit_count, struct v34_info0 *info0)
{
	struct v34_info0 decoded = { 0 };

	if (payload == NULL || info0 == NULL ||
	    payload_capacity < V34_INFO0_PAYLOAD_BYTES)
		return V34_INFO0_INVALID_ARGUMENT;
	if (payload_bit_count != V34_INFO0_PAYLOAD_BITS ||
	    !packed_msb_tail_is_zero(
		    payload, payload_bit_count, V34_INFO0_PAYLOAD_BYTES))
		return V34_INFO0_INVALID_PAYLOAD;

	decoded.symbol_rate_2743_supported =
		packed_msb_bit(payload, V34_INFO0_SYMBOL_RATE_2743_BIT) != 0;
	decoded.symbol_rate_2800_supported =
		packed_msb_bit(payload, V34_INFO0_SYMBOL_RATE_2800_BIT) != 0;
	decoded.symbol_rate_3429_supported =
		packed_msb_bit(payload, V34_INFO0_SYMBOL_RATE_3429_BIT) != 0;
	decoded.low_carrier_3000_supported =
		packed_msb_bit(payload, V34_INFO0_LOW_CARRIER_3000_BIT) != 0;
	decoded.high_carrier_3000_supported =
		packed_msb_bit(payload, V34_INFO0_HIGH_CARRIER_3000_BIT) != 0;
	decoded.low_carrier_3200_supported =
		packed_msb_bit(payload, V34_INFO0_LOW_CARRIER_3200_BIT) != 0;
	decoded.high_carrier_3200_supported =
		packed_msb_bit(payload, V34_INFO0_HIGH_CARRIER_3200_BIT) != 0;
	decoded.symbol_rate_3429_transmit_permitted =
		packed_msb_bit(
			payload,
			V34_INFO0_SYMBOL_RATE_3429_TRANSMIT_PERMITTED_BIT) != 0;
	decoded.transmit_power_reduction_supported =
		packed_msb_bit(payload, V34_INFO0_TRANSMIT_POWER_REDUCTION_BIT) !=
		0;
	decoded.maximum_symbol_rate_difference_steps =
		(uint8_t)packed_msb_read_lsb_field(
			payload,
			V34_INFO0_MAXIMUM_SYMBOL_RATE_DIFFERENCE_STEPS_FIRST_BIT,
			V34_INFO0_MAXIMUM_SYMBOL_RATE_DIFFERENCE_STEPS_BITS);
	decoded.cme_modem =
		packed_msb_bit(payload, V34_INFO0_CME_MODEM_BIT) != 0;
	decoded.constellation_1664_supported =
		packed_msb_bit(payload, V34_INFO0_CONSTELLATION_1664_BIT) != 0;
	decoded.transmit_clock_source =
		(enum v34_info0_clock_source)
			(uint8_t)packed_msb_read_lsb_field(
				payload, V34_INFO0_TRANSMIT_CLOCK_FIRST_BIT,
				V34_INFO0_TRANSMIT_CLOCK_BITS);
	decoded.acknowledges_peer_info0 =
		packed_msb_bit(payload, V34_INFO0_ACKNOWLEDGMENT_BIT) != 0;

	if (!v34_info0_is_valid(&decoded))
		return V34_INFO0_INVALID_PAYLOAD;
	*info0 = decoded;
	return V34_INFO0_OK;
}
