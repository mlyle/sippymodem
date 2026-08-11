// SPDX-License-Identifier: BSD-3-Clause

#include "v44_decoder.h"

#include "v44_compressed_decoder.h"
#include "v44_transparent_decoder.h"

#include <stdbool.h>
#include <stdlib.h>

enum v44_decode_mode {
	V44_DECODE_COMPRESSED = 0,
	V44_DECODE_TRANSPARENT,
	V44_DECODE_PARAMETER,
	V44_DECODE_FAILED,
};

struct v44_decoder {
	struct v44_compressed_decoder *compressed;
	struct v44_transparent_decoder *transparent;
	enum v44_decode_mode mode;
};

static bool decoder_is_valid(const struct v44_decoder *decoder)
{
	return decoder != NULL && decoder->compressed != NULL &&
	       decoder->transparent != NULL &&
	       decoder->mode >= V44_DECODE_COMPRESSED &&
	       decoder->mode <= V44_DECODE_FAILED;
}

struct v44_decoder *v44_decoder_create(
	const struct v44_parameters *parameters,
	size_t compressed_output_capacity)
{
	struct v44_decoder *decoder;

	if (!v44_parameters_are_valid(parameters))
		return NULL;
	decoder = calloc(1, sizeof(*decoder));
	if (decoder == NULL)
		return NULL;
	decoder->compressed = v44_compressed_decoder_create(
		parameters, compressed_output_capacity);
	decoder->transparent = v44_transparent_decoder_create();
	if (decoder->compressed == NULL || decoder->transparent == NULL) {
		v44_decoder_destroy(decoder);
		return NULL;
	}
	return decoder;
}

void v44_decoder_destroy(
	struct v44_decoder *decoder)
{
	if (decoder == NULL)
		return;
	v44_transparent_decoder_destroy(decoder->transparent);
	v44_compressed_decoder_destroy(decoder->compressed);
	free(decoder);
}

static enum v44_decode_result fail_decoder(
	struct v44_decoder *decoder)
{
	decoder->mode = V44_DECODE_FAILED;
	return V44_DECODE_PROCEDURAL_ERROR;
}

static enum v44_decode_result drain_compressed(
	struct v44_decoder *decoder,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length)
{
	if (output_capacity == 0)
		return V44_DECODE_NEED_OUTPUT;
	*output_length = v44_compressed_decoder_read(
		decoder->compressed, output, output_capacity);
	return *output_length != 0 ? V44_DECODE_OK :
				     fail_decoder(decoder);
}

static enum v44_decode_result process_compressed(
	struct v44_decoder *decoder,
	const uint8_t **input,
	size_t *input_length,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length)
{
	enum v44_compressed_result result =
		v44_compressed_decoder_process(
			decoder->compressed, input, input_length);

	switch (result) {
	case V44_COMPRESSED_OK:
	case V44_COMPRESSED_NEED_OUTPUT:
		if (v44_compressed_decoder_pending(
			    decoder->compressed) != 0)
			return drain_compressed(
				decoder, output, output_capacity, output_length);
		return result == V44_COMPRESSED_OK ?
			       V44_DECODE_OK : fail_decoder(decoder);
	case V44_COMPRESSED_NEED_INPUT:
		return V44_DECODE_NEED_INPUT;
	case V44_COMPRESSED_FLUSHED:
		return v44_compressed_decoder_pending(
			       decoder->compressed) == 0 ?
			       V44_DECODE_FLUSHED : fail_decoder(decoder);
	case V44_COMPRESSED_ENTER_TRANSPARENT:
		if (v44_compressed_decoder_pending(
			    decoder->compressed) != 0 ||
		    v44_transparent_decoder_enter(
			    decoder->transparent) !=
			    V44_TRANSPARENT_OK)
			return fail_decoder(decoder);
		decoder->mode = V44_DECODE_TRANSPARENT;
		return V44_DECODE_OK;
	case V44_COMPRESSED_INVALID_ARGUMENT:
	case V44_COMPRESSED_PROCEDURAL_ERROR:
		return fail_decoder(decoder);
	}
	return fail_decoder(decoder);
}

static enum v44_decode_result process_transparent(
	struct v44_decoder *decoder,
	const uint8_t **input,
	size_t *input_length,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length)
{
	enum v44_transparent_result result =
		v44_transparent_decoder_process(
			decoder->transparent, input, input_length, output,
			output_capacity, output_length);

	switch (result) {
	case V44_TRANSPARENT_OK:
		return V44_DECODE_OK;
	case V44_TRANSPARENT_NEED_INPUT:
		return V44_DECODE_NEED_INPUT;
	case V44_TRANSPARENT_NEED_OUTPUT:
		return V44_DECODE_NEED_OUTPUT;
	case V44_TRANSPARENT_ENTER_COMPRESSED:
		if (v44_compressed_decoder_enter(
			    decoder->compressed) != V44_COMPRESSED_OK)
			return fail_decoder(decoder);
		decoder->mode = V44_DECODE_COMPRESSED;
		return V44_DECODE_OK;
	case V44_TRANSPARENT_ENTER_PARAMETER:
		decoder->mode = V44_DECODE_PARAMETER;
		return V44_DECODE_ENTER_PARAMETER;
	case V44_TRANSPARENT_INVALID_ARGUMENT:
	case V44_TRANSPARENT_PROCEDURAL_ERROR:
		return fail_decoder(decoder);
	}
	return fail_decoder(decoder);
}

enum v44_decode_result v44_decoder_process(
	struct v44_decoder *decoder,
	const uint8_t **input,
	size_t *input_length,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length)
{
	if (!decoder_is_valid(decoder) || input == NULL ||
	    input_length == NULL || output_length == NULL ||
	    (*input_length != 0 && *input == NULL) ||
	    (output_capacity != 0 && output == NULL))
		return V44_DECODE_INVALID_ARGUMENT;
	*output_length = 0;
	if (decoder->mode == V44_DECODE_FAILED)
		return V44_DECODE_PROCEDURAL_ERROR;
	if (decoder->mode == V44_DECODE_PARAMETER)
		return V44_DECODE_ENTER_PARAMETER;
	if (v44_compressed_decoder_pending(decoder->compressed) != 0)
		return drain_compressed(
			decoder, output, output_capacity, output_length);
	if (decoder->mode == V44_DECODE_COMPRESSED)
		return process_compressed(
			decoder, input, input_length, output, output_capacity,
			output_length);
	return process_transparent(
		decoder, input, input_length, output, output_capacity,
		output_length);
}

enum v44_decode_result
v44_decoder_resume_transparent(
	struct v44_decoder *decoder)
{
	if (!decoder_is_valid(decoder))
		return V44_DECODE_INVALID_ARGUMENT;
	if (decoder->mode == V44_DECODE_FAILED)
		return V44_DECODE_PROCEDURAL_ERROR;
	if (decoder->mode != V44_DECODE_PARAMETER)
		return V44_DECODE_INVALID_ARGUMENT;
	if (v44_transparent_decoder_enter(decoder->transparent) !=
	    V44_TRANSPARENT_OK)
		return fail_decoder(decoder);
	decoder->mode = V44_DECODE_TRANSPARENT;
	return V44_DECODE_OK;
}

enum v44_decode_result v44_decoder_finish(
	struct v44_decoder *decoder)
{
	if (!decoder_is_valid(decoder))
		return V44_DECODE_INVALID_ARGUMENT;
	if (decoder->mode == V44_DECODE_FAILED)
		return V44_DECODE_PROCEDURAL_ERROR;
	if (decoder->mode == V44_DECODE_PARAMETER)
		return V44_DECODE_ENTER_PARAMETER;
	if (v44_compressed_decoder_pending(decoder->compressed) != 0)
		return V44_DECODE_NEED_OUTPUT;
	if (decoder->mode == V44_DECODE_COMPRESSED)
		return v44_compressed_decoder_finish(
			       decoder->compressed) == V44_COMPRESSED_OK ?
			       V44_DECODE_OK : fail_decoder(decoder);
	return v44_transparent_decoder_finish(
		       decoder->transparent) == V44_TRANSPARENT_OK ?
		       V44_DECODE_OK : fail_decoder(decoder);
}

const char *v44_decode_result_name(
	enum v44_decode_result result)
{
	switch (result) {
	case V44_DECODE_OK:
		return "ok";
	case V44_DECODE_NEED_INPUT:
		return "need input";
	case V44_DECODE_NEED_OUTPUT:
		return "need output";
	case V44_DECODE_FLUSHED:
		return "flushed";
	case V44_DECODE_ENTER_PARAMETER:
		return "enter parameter";
	case V44_DECODE_INVALID_ARGUMENT:
		return "invalid argument";
	case V44_DECODE_PROCEDURAL_ERROR:
		return "procedural error";
	}
	return "unknown";
}
