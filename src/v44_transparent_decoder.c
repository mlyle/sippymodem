// SPDX-License-Identifier: BSD-3-Clause

#include "v44_transparent_decoder.h"

#include <stdbool.h>
#include <stdlib.h>

enum {
	V44_COMMAND_ECM = 0,
	V44_COMMAND_EID = 1,
	V44_COMMAND_EPM = 2,
	V44_ESCAPE_STEP = 51,
};

enum v44_transparent_state {
	V44_TRANSPARENT_COMPRESSED = 0,
	V44_TRANSPARENT_ACTIVE,
	V44_TRANSPARENT_PARAMETER,
	V44_TRANSPARENT_FAILED,
};

struct v44_transparent_decoder {
	enum v44_transparent_state state;
	uint8_t escape;
	bool escape_pending;
};

static bool decoder_is_valid(
	const struct v44_transparent_decoder *decoder)
{
	return decoder != NULL && decoder->state >= V44_TRANSPARENT_COMPRESSED &&
	       decoder->state <= V44_TRANSPARENT_FAILED &&
	       (decoder->state == V44_TRANSPARENT_ACTIVE ||
		!decoder->escape_pending);
}

struct v44_transparent_decoder *
v44_transparent_decoder_create(void)
{
	return calloc(1, sizeof(struct v44_transparent_decoder));
}

void v44_transparent_decoder_destroy(
	struct v44_transparent_decoder *decoder)
{
	if (decoder == NULL)
		return;
	free(decoder);
}

static enum v44_transparent_result fail_decoder(
	struct v44_transparent_decoder *decoder)
{
	decoder->escape_pending = false;
	decoder->state = V44_TRANSPARENT_FAILED;
	return V44_TRANSPARENT_PROCEDURAL_ERROR;
}

static enum v44_transparent_result latched_result(
	const struct v44_transparent_decoder *decoder)
{
	switch (decoder->state) {
	case V44_TRANSPARENT_ACTIVE:
		return V44_TRANSPARENT_OK;
	case V44_TRANSPARENT_COMPRESSED:
		return V44_TRANSPARENT_ENTER_COMPRESSED;
	case V44_TRANSPARENT_PARAMETER:
		return V44_TRANSPARENT_ENTER_PARAMETER;
	case V44_TRANSPARENT_FAILED:
		return V44_TRANSPARENT_PROCEDURAL_ERROR;
	}
	return V44_TRANSPARENT_PROCEDURAL_ERROR;
}

static void consume_byte(const uint8_t **input, size_t *input_length)
{
	(*input)++;
	(*input_length)--;
}

enum v44_transparent_result
v44_transparent_decoder_process(
	struct v44_transparent_decoder *decoder,
	const uint8_t **input,
	size_t *input_length,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length)
{
	enum v44_transparent_result result;
	uint8_t value;

	if (!decoder_is_valid(decoder) || input == NULL ||
	    input_length == NULL || output_length == NULL ||
	    (*input_length != 0 && *input == NULL) ||
	    (output_capacity != 0 && output == NULL))
		return V44_TRANSPARENT_INVALID_ARGUMENT;
	*output_length = 0;
	result = latched_result(decoder);
	if (result != V44_TRANSPARENT_OK)
		return result;
	if (*input_length == 0)
		return V44_TRANSPARENT_NEED_INPUT;

	value = **input;
	if (!decoder->escape_pending && value != decoder->escape) {
		if (output_capacity == 0)
			return V44_TRANSPARENT_NEED_OUTPUT;
		output[0] = value;
		*output_length = 1;
		consume_byte(input, input_length);
		return V44_TRANSPARENT_OK;
	}
	if (!decoder->escape_pending) {
		decoder->escape_pending = true;
		consume_byte(input, input_length);
		if (*input_length == 0)
			return V44_TRANSPARENT_NEED_INPUT;
		value = **input;
	}

	if (value == V44_COMMAND_EID) {
		if (output_capacity == 0)
			return V44_TRANSPARENT_NEED_OUTPUT;
		output[0] = decoder->escape;
		*output_length = 1;
		decoder->escape = (uint8_t)(decoder->escape + V44_ESCAPE_STEP);
		decoder->escape_pending = false;
		consume_byte(input, input_length);
		return V44_TRANSPARENT_OK;
	}

	decoder->escape_pending = false;
	consume_byte(input, input_length);
	if (value == V44_COMMAND_ECM) {
		decoder->state = V44_TRANSPARENT_COMPRESSED;
		return V44_TRANSPARENT_ENTER_COMPRESSED;
	}
	if (value == V44_COMMAND_EPM) {
		decoder->state = V44_TRANSPARENT_PARAMETER;
		return V44_TRANSPARENT_ENTER_PARAMETER;
	}
	return fail_decoder(decoder);
}

enum v44_transparent_result
v44_transparent_decoder_enter(
	struct v44_transparent_decoder *decoder)
{
	if (!decoder_is_valid(decoder))
		return V44_TRANSPARENT_INVALID_ARGUMENT;
	if (decoder->state == V44_TRANSPARENT_FAILED)
		return V44_TRANSPARENT_PROCEDURAL_ERROR;
	if (decoder->state != V44_TRANSPARENT_COMPRESSED &&
	    decoder->state != V44_TRANSPARENT_PARAMETER)
		return V44_TRANSPARENT_INVALID_ARGUMENT;
	decoder->state = V44_TRANSPARENT_ACTIVE;
	return V44_TRANSPARENT_OK;
}

enum v44_transparent_result
v44_transparent_decoder_finish(
	struct v44_transparent_decoder *decoder)
{
	enum v44_transparent_result result;

	if (!decoder_is_valid(decoder))
		return V44_TRANSPARENT_INVALID_ARGUMENT;
	result = latched_result(decoder);
	if (result != V44_TRANSPARENT_OK)
		return result;
	return decoder->escape_pending ? fail_decoder(decoder) :
					 V44_TRANSPARENT_OK;
}

const char *v44_transparent_result_name(
	enum v44_transparent_result result)
{
	switch (result) {
	case V44_TRANSPARENT_OK:
		return "ok";
	case V44_TRANSPARENT_NEED_INPUT:
		return "need input";
	case V44_TRANSPARENT_NEED_OUTPUT:
		return "need output";
	case V44_TRANSPARENT_ENTER_COMPRESSED:
		return "enter compressed";
	case V44_TRANSPARENT_ENTER_PARAMETER:
		return "enter parameter";
	case V44_TRANSPARENT_INVALID_ARGUMENT:
		return "invalid argument";
	case V44_TRANSPARENT_PROCEDURAL_ERROR:
		return "procedural error";
	}
	return "unknown";
}
