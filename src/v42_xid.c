// SPDX-License-Identifier: BSD-3-Clause

#include "v42_xid.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum {
	V42_XID_PARAMETER_NEGOTIATION = 0x80,
	V42_XID_PRIVATE_NEGOTIATION = 0xf0,
	V42_XID_USER_DATA = 0xff,
};

static bool parameter_field_is_valid(const uint8_t *field, size_t length)
{
	size_t offset = 0;

	if (length != 0 && field == NULL)
		return false;
	while (offset < length) {
		size_t value_length;

		if (length - offset < 2)
			return false;
		value_length = field[offset + 1U];
		offset += 2;
		if (value_length > length - offset)
			return false;
		offset += value_length;
	}
	return true;
}

static bool fields_are_valid(
	const struct sippymodem_v42_xid_fields *fields)
{
	if (fields == NULL)
		return false;
	return fields->parameter_negotiation_length <= UINT16_MAX &&
	       fields->private_negotiation_length <= UINT16_MAX &&
	       parameter_field_is_valid(
		       fields->parameter_negotiation,
		       fields->parameter_negotiation_length) &&
	       parameter_field_is_valid(
		       fields->private_negotiation,
		       fields->private_negotiation_length) &&
	       (fields->user_data_length == 0 ||
		(fields->user_data != NULL && fields->user_data_length > 1 &&
		 fields->user_data[0] == V42_XID_USER_DATA &&
		 parameter_field_is_valid(
			 fields->user_data + 1,
			 fields->user_data_length - 1U)));
}

static bool add_size(size_t *total, size_t amount)
{
	if (amount > SIZE_MAX - *total)
		return false;
	*total += amount;
	return true;
}

static bool encoded_size(
	const struct sippymodem_v42_xid_fields *fields, size_t *size)
{
	size_t total = 1;

	if (fields->parameter_negotiation_length != 0 &&
	    (!add_size(&total, 3) ||
	     !add_size(&total, fields->parameter_negotiation_length)))
		return false;
	if (fields->private_negotiation_length != 0 &&
	    (!add_size(&total, 3) ||
	     !add_size(&total, fields->private_negotiation_length)))
		return false;
	if (fields->user_data_length != 0 &&
	    !add_size(&total, fields->user_data_length))
		return false;
	*size = total;
	return true;
}

static void put_data_link_group(
	uint8_t *output,
	size_t *offset,
	uint8_t identifier,
	const uint8_t *field,
	size_t length)
{
	output[(*offset)++] = identifier;
	output[(*offset)++] = (uint8_t)(length >> 8);
	output[(*offset)++] = (uint8_t)length;
	memcpy(output + *offset, field, length);
	*offset += length;
}

enum sippymodem_v42_xid_result sippymodem_v42_xid_encode(
	const struct sippymodem_v42_xid_fields *fields,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length)
{
	size_t required;
	size_t offset = 0;

	if (!fields_are_valid(fields) || output_length == NULL ||
	    (output_capacity != 0 && output == NULL) ||
	    !encoded_size(fields, &required))
		return SIPPYMODEM_V42_XID_INVALID_ARGUMENT;
	if (required > output_capacity)
		return SIPPYMODEM_V42_XID_NEED_OUTPUT;
	output[offset++] = SIPPYMODEM_V42_XID_GENERAL_FORMAT;
	if (fields->parameter_negotiation_length != 0)
		put_data_link_group(
			output, &offset, V42_XID_PARAMETER_NEGOTIATION,
			fields->parameter_negotiation,
			fields->parameter_negotiation_length);
	if (fields->private_negotiation_length != 0)
		put_data_link_group(
			output, &offset, V42_XID_PRIVATE_NEGOTIATION,
			fields->private_negotiation,
			fields->private_negotiation_length);
	if (fields->user_data_length != 0) {
		memcpy(
			output + offset, fields->user_data,
			fields->user_data_length);
		offset += fields->user_data_length;
	}
	*output_length = offset;
	return SIPPYMODEM_V42_XID_OK;
}

static bool group_follows(uint8_t identifier, bool have_previous,
			  uint8_t previous)
{
	return !have_previous || identifier > previous;
}

enum sippymodem_v42_xid_result sippymodem_v42_xid_decode(
	const uint8_t *input,
	size_t input_length,
	struct sippymodem_v42_xid_fields *fields)
{
	struct sippymodem_v42_xid_fields decoded = { 0 };
	size_t offset = 1;
	uint8_t previous = 0;
	bool have_previous = false;

	if (input == NULL || fields == NULL)
		return SIPPYMODEM_V42_XID_INVALID_ARGUMENT;
	if (input_length == 0 ||
	    input[0] != SIPPYMODEM_V42_XID_GENERAL_FORMAT)
		return SIPPYMODEM_V42_XID_MALFORMED;
	while (offset < input_length) {
		const uint8_t identifier = input[offset++];
		const uint8_t *field;
		size_t field_length;

		if (identifier == V42_XID_USER_DATA) {
			field = input + offset - 1U;
			field_length = input_length - offset + 1U;
			if (field_length <= 1 ||
			    !parameter_field_is_valid(
				    field + 1, field_length - 1U))
				return SIPPYMODEM_V42_XID_MALFORMED;
			decoded.user_data = field;
			decoded.user_data_length = field_length;
			offset = input_length;
			continue;
		}
		if (!group_follows(identifier, have_previous, previous) ||
		    input_length - offset < 2)
			return SIPPYMODEM_V42_XID_MALFORMED;
		field_length = (size_t)input[offset] << 8 |
			       input[offset + 1U];
		offset += 2;
		if (field_length == 0 || field_length > input_length - offset)
			return SIPPYMODEM_V42_XID_MALFORMED;
		field = input + offset;
		if (!parameter_field_is_valid(field, field_length))
			return SIPPYMODEM_V42_XID_MALFORMED;
		if (identifier == V42_XID_PARAMETER_NEGOTIATION) {
			decoded.parameter_negotiation = field;
			decoded.parameter_negotiation_length = field_length;
		} else if (identifier == V42_XID_PRIVATE_NEGOTIATION) {
			decoded.private_negotiation = field;
			decoded.private_negotiation_length = field_length;
		}
		offset += field_length;
		previous = identifier;
		have_previous = true;
	}
	*fields = decoded;
	return SIPPYMODEM_V42_XID_OK;
}

const char *sippymodem_v42_xid_result_name(
	enum sippymodem_v42_xid_result result)
{
	switch (result) {
	case SIPPYMODEM_V42_XID_OK:
		return "ok";
	case SIPPYMODEM_V42_XID_NEED_OUTPUT:
		return "need output";
	case SIPPYMODEM_V42_XID_MALFORMED:
		return "malformed";
	case SIPPYMODEM_V42_XID_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
