// SPDX-License-Identifier: BSD-3-Clause

#include "v92_info1d.h"

#include <string.h>

static struct v34_info1_probe_result *repurposed_probe(
	struct v34_info1c *info1d)
{
	return &info1d->answer_transmit[V34_SYMBOL_RATE_3429];
}

static const struct v34_info1_probe_result *const_repurposed_probe(
	const struct v34_info1c *info1d)
{
	return &info1d->answer_transmit[V34_SYMBOL_RATE_3429];
}

bool v92_info1d_is_valid(const struct v92_info1d *info1d)
{
	return info1d != NULL && v34_info1c_is_valid(&info1d->v34) &&
	       !const_repurposed_probe(&info1d->v34)->high_carrier;
}

static enum v92_info1d_result map_v34_result(
	enum v34_info1_result result)
{
	switch (result) {
	case V34_INFO1_OK:
		return V92_INFO1D_OK;
	case V34_INFO1_INVALID_ARGUMENT:
		return V92_INFO1D_INVALID_ARGUMENT;
	case V34_INFO1_INVALID_PARAMETERS:
		return V92_INFO1D_INVALID_PARAMETERS;
	case V34_INFO1_INVALID_PAYLOAD:
		return V92_INFO1D_INVALID_PAYLOAD;
	}
	return V92_INFO1D_INVALID_PAYLOAD;
}

enum v92_info1d_result v92_info1d_encode(
	const struct v92_info1d *info1d, uint8_t *payload,
	size_t payload_capacity)
{
	struct v34_info1c encoded;
	uint8_t bytes[V92_INFO1D_PAYLOAD_BYTES];
	enum v34_info1_result result;

	if (info1d == NULL || payload == NULL ||
	    payload_capacity < sizeof(bytes))
		return V92_INFO1D_INVALID_ARGUMENT;
	if (!v92_info1d_is_valid(info1d))
		return V92_INFO1D_INVALID_PARAMETERS;
	encoded = info1d->v34;
	repurposed_probe(&encoded)->high_carrier =
		info1d->pcm_upstream_supported;
	result = v34_info1c_encode(&encoded, bytes, sizeof(bytes));
	if (result != V34_INFO1_OK)
		return map_v34_result(result);
	memcpy(payload, bytes, sizeof(bytes));
	return V92_INFO1D_OK;
}

enum v92_info1d_result v92_info1d_decode(
	const uint8_t *payload, size_t payload_capacity,
	size_t payload_bit_count, struct v92_info1d *info1d)
{
	struct v92_info1d decoded = { 0 };
	enum v34_info1_result result;

	if (payload == NULL || info1d == NULL)
		return V92_INFO1D_INVALID_ARGUMENT;
	result = v34_info1c_decode(
		payload, payload_capacity, payload_bit_count, &decoded.v34);
	if (result != V34_INFO1_OK)
		return map_v34_result(result);
	decoded.pcm_upstream_supported =
		repurposed_probe(&decoded.v34)->high_carrier;
	repurposed_probe(&decoded.v34)->high_carrier = false;
	if (!v92_info1d_is_valid(&decoded))
		return V92_INFO1D_INVALID_PAYLOAD;
	*info1d = decoded;
	return V92_INFO1D_OK;
}

const char *v92_info1d_result_name(enum v92_info1d_result result)
{
	switch (result) {
	case V92_INFO1D_OK:
		return "ok";
	case V92_INFO1D_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_INFO1D_INVALID_PARAMETERS:
		return "invalid parameters";
	case V92_INFO1D_INVALID_PAYLOAD:
		return "invalid payload";
	}
	return "unknown";
}
