// SPDX-License-Identifier: BSD-3-Clause

#include "v92_info0.h"

#include "packed_bits.h"

#include <string.h>

enum {
	/* Payload bit zero is INFO0 wire bit 12. */
	INFO0_WIRE_BIT_26 = 14,
	INFO0_WIRE_BIT_27 = 15,
};

static bool extension_is_valid(bool capable, bool short_requested)
{
	return capable || !short_requested;
}

bool v92_info0a_is_valid(const struct v92_info0a *info0a)
{
	return info0a != NULL && v90_info0_is_valid(&info0a->v90) &&
	       extension_is_valid(
		       info0a->v92_capable, info0a->short_phase2_requested);
}

bool v92_info0d_is_valid(const struct v92_info0d *info0d)
{
	return info0d != NULL && v90_info0d_is_valid(&info0d->v90) &&
	       extension_is_valid(
		       info0d->v92_capable, info0d->short_phase2_requested);
}

static void set_payload_bit(uint8_t *payload, size_t bit, bool value)
{
	uint8_t mask = (uint8_t)(1u << (7u - bit % 8u));

	if (value)
		payload[bit / 8u] |= mask;
	else
		payload[bit / 8u] &= (uint8_t)~mask;
}

static enum v92_info0_result map_v90_result(enum v90_info0_result result)
{
	switch (result) {
	case V90_INFO0_OK:
		return V92_INFO0_OK;
	case V90_INFO0_INVALID_ARGUMENT:
		return V92_INFO0_INVALID_ARGUMENT;
	case V90_INFO0_INVALID_CAPABILITIES:
		return V92_INFO0_INVALID_CAPABILITIES;
	case V90_INFO0_INVALID_PAYLOAD:
		return V92_INFO0_INVALID_PAYLOAD;
	}
	return V92_INFO0_INVALID_PAYLOAD;
}

enum v92_info0_result v92_info0a_encode(
	const struct v92_info0a *info0a, uint8_t *payload,
	size_t payload_capacity)
{
	uint8_t encoded[V90_INFO0A_PAYLOAD_BYTES];
	enum v90_info0_result result;

	if (info0a == NULL || payload == NULL ||
	    payload_capacity < sizeof(encoded))
		return V92_INFO0_INVALID_ARGUMENT;
	if (!v92_info0a_is_valid(info0a))
		return V92_INFO0_INVALID_CAPABILITIES;
	result = v90_info0a_encode(&info0a->v90, encoded, sizeof(encoded));
	if (result != V90_INFO0_OK)
		return map_v90_result(result);
	set_payload_bit(encoded, INFO0_WIRE_BIT_26, info0a->v92_capable);
	set_payload_bit(
		encoded, INFO0_WIRE_BIT_27, info0a->short_phase2_requested);
	memcpy(payload, encoded, sizeof(encoded));
	return V92_INFO0_OK;
}

enum v92_info0_result v92_info0a_decode(
	const uint8_t *payload, size_t payload_capacity,
	size_t payload_bit_count, struct v92_info0a *info0a)
{
	struct v92_info0a decoded = { 0 };
	enum v90_info0_result result;

	if (payload == NULL || info0a == NULL)
		return V92_INFO0_INVALID_ARGUMENT;
	result = v90_info0a_decode(
		payload, payload_capacity, payload_bit_count, &decoded.v90);
	if (result != V90_INFO0_OK)
		return map_v90_result(result);
	decoded.v92_capable = packed_msb_bit(payload, INFO0_WIRE_BIT_26);
	decoded.short_phase2_requested =
		packed_msb_bit(payload, INFO0_WIRE_BIT_27);
	if (!v92_info0a_is_valid(&decoded))
		return V92_INFO0_INVALID_PAYLOAD;
	*info0a = decoded;
	return V92_INFO0_OK;
}

enum v92_info0_result v92_info0d_encode(
	const struct v92_info0d *info0d, uint8_t *payload,
	size_t payload_capacity)
{
	uint8_t encoded[V90_INFO0D_PAYLOAD_BYTES];
	enum v90_info0_result result;

	if (info0d == NULL || payload == NULL ||
	    payload_capacity < sizeof(encoded))
		return V92_INFO0_INVALID_ARGUMENT;
	if (!v92_info0d_is_valid(info0d))
		return V92_INFO0_INVALID_CAPABILITIES;
	result = v90_info0d_encode(&info0d->v90, encoded, sizeof(encoded));
	if (result != V90_INFO0_OK)
		return map_v90_result(result);
	set_payload_bit(
		encoded, INFO0_WIRE_BIT_26, info0d->short_phase2_requested);
	set_payload_bit(encoded, INFO0_WIRE_BIT_27, info0d->v92_capable);
	memcpy(payload, encoded, sizeof(encoded));
	return V92_INFO0_OK;
}

enum v92_info0_result v92_info0d_decode(
	const uint8_t *payload, size_t payload_capacity,
	size_t payload_bit_count, struct v92_info0d *info0d)
{
	struct v92_info0d decoded = { 0 };
	enum v90_info0_result result;

	if (payload == NULL || info0d == NULL)
		return V92_INFO0_INVALID_ARGUMENT;
	result = v90_info0d_decode(
		payload, payload_capacity, payload_bit_count, &decoded.v90);
	if (result != V90_INFO0_OK)
		return map_v90_result(result);
	decoded.short_phase2_requested =
		packed_msb_bit(payload, INFO0_WIRE_BIT_26);
	decoded.v92_capable = packed_msb_bit(payload, INFO0_WIRE_BIT_27);
	if (!v92_info0d_is_valid(&decoded))
		return V92_INFO0_INVALID_PAYLOAD;
	*info0d = decoded;
	return V92_INFO0_OK;
}

const char *v92_info0_result_name(enum v92_info0_result result)
{
	switch (result) {
	case V92_INFO0_OK:
		return "ok";
	case V92_INFO0_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_INFO0_INVALID_CAPABILITIES:
		return "invalid capabilities";
	case V92_INFO0_INVALID_PAYLOAD:
		return "invalid payload";
	}
	return "unknown";
}
