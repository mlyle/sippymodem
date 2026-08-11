// SPDX-License-Identifier: BSD-3-Clause

#include "v44_xid.h"

#include <string.h>

enum {
	V44_XID_USER_DATA = 0xff,
	V44_XID_PARAMETER_SET = 0x40,
	V44_XID_CAPABILITY = 0x41,
	V44_XID_REQUEST = 0x42,
	V44_XID_TX_CODEWORDS = 0x43,
	V44_XID_RX_CODEWORDS = 0x44,
	V44_XID_TX_STRING = 0x45,
	V44_XID_RX_STRING = 0x46,
	V44_XID_TX_HISTORY = 0x47,
	V44_XID_RX_HISTORY = 0x48,
	V44_XID_FIRST_PARAMETER = V44_XID_PARAMETER_SET,
	V44_XID_LAST_PARAMETER = V44_XID_RX_HISTORY,
	V44_XID_BASE_PARAMETERS = 0x003,
	V44_XID_PROPOSAL_PARAMETERS = 0x1fc,
	V44_XID_CAPABILITY_RESERVED = 0x3e,
	V44_XID_REQUEST_TRANSMIT = 0x01,
	V44_XID_REQUEST_RECEIVE = 0x02,
	V44_XID_REQUEST_RESERVED = 0xfc,
};

static bool offer_is_valid(const struct v44_offer *offer)
{
	return offer != NULL &&
	       v44_parameters_are_valid(
		       &offer->transmit.parameters) &&
	       v44_parameters_are_valid(&offer->receive.parameters);
}

struct v44_offer v44_default_offer(void)
{
	const struct v44_parameters parameters =
		v44_default_parameters();
	const struct v44_offer offer = {
		.transmit = { .enabled = true, .parameters = parameters },
		.receive = { .enabled = true, .parameters = parameters },
	};

	return offer;
}

static void put_parameter(
	uint8_t *output,
	size_t *length,
	uint8_t identifier,
	uint32_t value,
	size_t value_length)
{
	output[(*length)++] = identifier;
	output[(*length)++] = (uint8_t)value_length;
	for (size_t index = value_length; index != 0; --index)
		output[(*length)++] =
			(uint8_t)(value >> ((index - 1U) * 8U));
}

enum v44_xid_result v44_xid_encode(
	const struct v44_offer *offer,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length)
{
	uint8_t encoded[V44_XID_OFFER_SIZE];
	size_t length = 0;
	uint8_t request;

	if (!offer_is_valid(offer) || output == NULL || output_length == NULL)
		return V44_XID_INVALID_ARGUMENT;
	if (output_capacity < sizeof(encoded))
		return V44_XID_NEED_OUTPUT;

	request = (offer->transmit.enabled ? V44_XID_REQUEST_TRANSMIT : 0U) |
		  (offer->receive.enabled ? V44_XID_REQUEST_RECEIVE : 0U);
	encoded[length++] = V44_XID_USER_DATA;
	put_parameter(
		encoded, &length, V44_XID_PARAMETER_SET, UINT32_C(0x563434), 3);
	put_parameter(encoded, &length, V44_XID_CAPABILITY, 0, 1);
	put_parameter(encoded, &length, V44_XID_REQUEST, request, 1);
	put_parameter(
		encoded, &length, V44_XID_TX_CODEWORDS,
		offer->transmit.parameters.codeword_limit, 2);
	put_parameter(
		encoded, &length, V44_XID_RX_CODEWORDS,
		offer->receive.parameters.codeword_limit, 2);
	put_parameter(
		encoded, &length, V44_XID_TX_STRING,
		offer->transmit.parameters.max_string_length, 1);
	put_parameter(
		encoded, &length, V44_XID_RX_STRING,
		offer->receive.parameters.max_string_length, 1);
	put_parameter(
		encoded, &length, V44_XID_TX_HISTORY,
		offer->transmit.parameters.history_size, 2);
	put_parameter(
		encoded, &length, V44_XID_RX_HISTORY,
		offer->receive.parameters.history_size, 2);
	if (length != sizeof(encoded))
		return V44_XID_INVALID_ARGUMENT;
	memcpy(output, encoded, sizeof(encoded));
	*output_length = sizeof(encoded);
	return V44_XID_OK;
}

static uint32_t get_parameter_value(const uint8_t *input, size_t length)
{
	uint32_t value = 0;

	for (size_t index = 0; index < length; ++index)
		value = (value << 8) | input[index];
	return value;
}

static bool parameter_length_is_valid(uint8_t identifier, size_t length)
{
	switch (identifier) {
	case V44_XID_PARAMETER_SET:
		return length == 3;
	case V44_XID_CAPABILITY:
	case V44_XID_REQUEST:
	case V44_XID_TX_STRING:
	case V44_XID_RX_STRING:
		return length == 1;
	case V44_XID_TX_CODEWORDS:
	case V44_XID_RX_CODEWORDS:
	case V44_XID_TX_HISTORY:
	case V44_XID_RX_HISTORY:
		return length == 2;
	default:
		return true;
	}
}

static enum v44_xid_result apply_parameter(
	struct v44_offer *offer,
	uint8_t identifier,
	uint32_t value)
{
	switch (identifier) {
	case V44_XID_PARAMETER_SET:
		return value == UINT32_C(0x563434) ?
			       V44_XID_OK :
			       V44_XID_MALFORMED;
	case V44_XID_CAPABILITY:
		if ((value & V44_XID_CAPABILITY_RESERVED) != 0)
			return V44_XID_MALFORMED;
		return V44_XID_OK;
	case V44_XID_REQUEST:
		if ((value & V44_XID_REQUEST_RESERVED) != 0)
			return V44_XID_MALFORMED;
		offer->transmit.enabled =
			(value & V44_XID_REQUEST_TRANSMIT) != 0;
		offer->receive.enabled =
			(value & V44_XID_REQUEST_RECEIVE) != 0;
		return V44_XID_OK;
	case V44_XID_TX_CODEWORDS:
		offer->transmit.parameters.codeword_limit = value;
		return V44_XID_OK;
	case V44_XID_RX_CODEWORDS:
		offer->receive.parameters.codeword_limit = value;
		return V44_XID_OK;
	case V44_XID_TX_STRING:
		offer->transmit.parameters.max_string_length = value;
		return V44_XID_OK;
	case V44_XID_RX_STRING:
		offer->receive.parameters.max_string_length = value;
		return V44_XID_OK;
	case V44_XID_TX_HISTORY:
		offer->transmit.parameters.history_size = value;
		return V44_XID_OK;
	case V44_XID_RX_HISTORY:
		offer->receive.parameters.history_size = value;
		return V44_XID_OK;
	default:
		return V44_XID_OK;
	}
}

enum v44_xid_result v44_xid_decode(
	const uint8_t *input,
	size_t input_length,
	struct v44_offer *offer)
{
	struct v44_offer decoded =
		v44_default_offer();
	uint32_t seen = 0;
	size_t offset = 1;

	if (input == NULL || offer == NULL)
		return V44_XID_INVALID_ARGUMENT;
	if (input_length < 3 || input[0] != V44_XID_USER_DATA ||
	    input[1] != V44_XID_PARAMETER_SET)
		return V44_XID_MALFORMED;
	while (offset < input_length) {
		enum v44_xid_result result;
		uint8_t identifier;
		size_t value_length;
		uint32_t value;

		if (input_length - offset < 2)
			return V44_XID_MALFORMED;
		identifier = input[offset++];
		value_length = input[offset++];
		if (value_length > input_length - offset ||
		    !parameter_length_is_valid(identifier, value_length))
			return V44_XID_MALFORMED;
		if (identifier >= V44_XID_FIRST_PARAMETER &&
		    identifier <= V44_XID_LAST_PARAMETER) {
			unsigned int bit = identifier - V44_XID_FIRST_PARAMETER;

			if ((seen & (UINT32_C(1) << bit)) != 0)
				return V44_XID_MALFORMED;
			seen |= UINT32_C(1) << bit;
		}
		value = get_parameter_value(input + offset, value_length);
		result = apply_parameter(&decoded, identifier, value);
		if (result != V44_XID_OK)
			return result;
		offset += value_length;
	}
	if ((seen & V44_XID_BASE_PARAMETERS) != V44_XID_BASE_PARAMETERS ||
	    ((seen & V44_XID_PROPOSAL_PARAMETERS) != 0 &&
	     (seen & V44_XID_PROPOSAL_PARAMETERS) !=
		     V44_XID_PROPOSAL_PARAMETERS) ||
	    !offer_is_valid(&decoded))
		return V44_XID_MALFORMED;
	*offer = decoded;
	return V44_XID_OK;
}

static uint32_t lesser(uint32_t left, uint32_t right)
{
	return left < right ? left : right;
}

static struct v44_parameters select_parameters(
	const struct v44_parameters *sender,
	const struct v44_parameters *receiver)
{
	const struct v44_parameters selected = {
		.codeword_limit = lesser(
			sender->codeword_limit, receiver->codeword_limit),
		.max_string_length = lesser(
			sender->max_string_length, receiver->max_string_length),
		.history_size = lesser(sender->history_size, receiver->history_size),
	};

	return selected;
}

enum v44_xid_result v44_xid_negotiate(
	const struct v44_offer *local,
	const struct v44_offer *peer,
	struct v44_offer *negotiated)
{
	struct v44_offer selected;

	if (!offer_is_valid(local) || !offer_is_valid(peer) || negotiated == NULL)
		return V44_XID_INVALID_ARGUMENT;
	selected.transmit.enabled =
		local->transmit.enabled && peer->receive.enabled;
	selected.transmit.parameters = select_parameters(
		&local->transmit.parameters, &peer->receive.parameters);
	selected.receive.enabled =
		local->receive.enabled && peer->transmit.enabled;
	selected.receive.parameters = select_parameters(
		&local->receive.parameters, &peer->transmit.parameters);
	*negotiated = selected;
	return V44_XID_OK;
}

const char *v44_xid_result_name(
	enum v44_xid_result result)
{
	switch (result) {
	case V44_XID_OK:
		return "ok";
	case V44_XID_NEED_OUTPUT:
		return "need output";
	case V44_XID_MALFORMED:
		return "malformed";
	case V44_XID_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
