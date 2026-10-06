// SPDX-License-Identifier: BSD-3-Clause

#include "v90_info0.h"

#include "packed_bits.h"

#include <string.h>

enum {
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

bool v90_info0_is_valid(const struct v34_info0 *info0)
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

bool v90_info0a_equal(const struct v34_info0 *left, const struct v34_info0 *right)
{
	if (left == NULL || right == NULL)
		return false;
	struct v34_info0 normalized_left = *left, normalized_right = *right;

	normalized_left.transmit_clock_source = V34_INFO0_CLOCK_INTERNAL;
	normalized_right.transmit_clock_source = V34_INFO0_CLOCK_INTERNAL;
	return v34_info0_equal(&normalized_left, &normalized_right);
}

bool v90_info0d_equal(
	const struct v90_info0d *left, const struct v90_info0d *right)
{
	return v90_info0d_is_valid(left) && v90_info0d_is_valid(right) &&
	       v90_info0a_equal(&left->common, &right->common) &&
	       left->nominal_transmit_power == right->nominal_transmit_power &&
	       left->maximum_transmit_power == right->maximum_transmit_power &&
	       left->measure_power_at_codec_output ==
		       right->measure_power_at_codec_output &&
	       left->pcm_law == right->pcm_law &&
	       left->upstream_symbol_rate_3429_supported ==
		       right->upstream_symbol_rate_3429_supported;
}

static bool encode_common(const struct v34_info0 *info0, uint8_t *payload)
{
	struct v34_info0 common = *info0;

	common.transmit_clock_source = V34_INFO0_CLOCK_INTERNAL;
	return v34_info0_encode(&common, payload, V34_INFO0_PAYLOAD_BYTES) == V34_INFO0_OK;
}

static bool decode_common(const uint8_t *payload, struct v34_info0 *info0)
{
	/* Exclude PCM extensions and reserved bits from the V.34 record. */
	uint8_t common[V34_INFO0_PAYLOAD_BYTES] = {
		payload[0], payload[1] & 0xfcu, payload[2] & 0x80u,
	};

	return v34_info0_decode(common, sizeof(common), V34_INFO0_PAYLOAD_BITS,
		info0) == V34_INFO0_OK;
}

enum v90_info0_result v90_info0a_encode(
	const struct v34_info0 *info0a, uint8_t *payload,
	size_t payload_capacity)
{
	uint8_t encoded[V90_INFO0A_PAYLOAD_BYTES] = { 0 };

	if (info0a == NULL || payload == NULL ||
	    payload_capacity < sizeof(encoded))
		return V90_INFO0_INVALID_ARGUMENT;
	if (!v90_info0_is_valid(info0a))
		return V90_INFO0_INVALID_CAPABILITIES;
	if (!encode_common(info0a, encoded))
		return V90_INFO0_INVALID_CAPABILITIES;
	memcpy(payload, encoded, sizeof(encoded));
	return V90_INFO0_OK;
}

enum v90_info0_result v90_info0a_decode(
	const uint8_t *payload, size_t payload_capacity,
	size_t payload_bit_count, struct v34_info0 *info0a)
{
	struct v34_info0 decoded;

	if (payload == NULL || info0a == NULL ||
	    payload_capacity < V90_INFO0A_PAYLOAD_BYTES)
		return V90_INFO0_INVALID_ARGUMENT;
	if (payload_bit_count != V90_INFO0A_PAYLOAD_BITS ||
	    !packed_msb_tail_is_zero(
		    payload, payload_bit_count, V90_INFO0A_PAYLOAD_BYTES))
		return V90_INFO0_INVALID_PAYLOAD;
	if (!decode_common(payload, &decoded))
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
	if (!encode_common(&info0d->common, encoded))
		return V90_INFO0_INVALID_CAPABILITIES;
	packed_msb_write_lsb_field(
		encoded, NOMINAL_TRANSMIT_POWER_FIRST_BIT,
		NOMINAL_TRANSMIT_POWER_BITS, info0d->nominal_transmit_power);
	packed_msb_write_lsb_field(
		encoded, MAXIMUM_TRANSMIT_POWER_FIRST_BIT,
		MAXIMUM_TRANSMIT_POWER_BITS, info0d->maximum_transmit_power);
	packed_msb_set_bit(encoded, MEASURE_AT_CODEC_BIT,
			info0d->measure_power_at_codec_output);
	packed_msb_set_bit(encoded, PCM_LAW_BIT,
			info0d->pcm_law == V90_PCM_A_LAW);
	packed_msb_set_bit(encoded, UPSTREAM_SYMBOL_RATE_3429_BIT,
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
	    !packed_msb_tail_is_zero(
		    payload, payload_bit_count, V90_INFO0D_PAYLOAD_BYTES))
		return V90_INFO0_INVALID_PAYLOAD;
	if (!decode_common(payload, &decoded.common))
		return V90_INFO0_INVALID_PAYLOAD;
	decoded.nominal_transmit_power =
		(uint8_t)packed_msb_read_lsb_field(
		payload, NOMINAL_TRANSMIT_POWER_FIRST_BIT,
		NOMINAL_TRANSMIT_POWER_BITS);
	decoded.maximum_transmit_power =
		(uint8_t)packed_msb_read_lsb_field(
		payload, MAXIMUM_TRANSMIT_POWER_FIRST_BIT,
		MAXIMUM_TRANSMIT_POWER_BITS);
	decoded.measure_power_at_codec_output =
		packed_msb_bit(payload, MEASURE_AT_CODEC_BIT) != 0;
	decoded.pcm_law = packed_msb_bit(payload, PCM_LAW_BIT) != 0 ?
				  V90_PCM_A_LAW : V90_PCM_MU_LAW;
	decoded.upstream_symbol_rate_3429_supported =
		packed_msb_bit(payload, UPSTREAM_SYMBOL_RATE_3429_BIT) != 0;
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
