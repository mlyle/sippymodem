// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v44_decoder.h"

#include "sippymodem_v44_compressed_decoder.h"
#include "sippymodem_v44_transparent_decoder.h"

#include <stdbool.h>
#include <stdlib.h>

enum v44_decode_mode {
	V44_DECODE_COMPRESSED = 0,
	V44_DECODE_TRANSPARENT,
	V44_DECODE_PARAMETER,
	V44_DECODE_FAILED,
};

struct sippymodem_v44_decoder {
	struct sippymodem_v44_compressed_decoder *compressed;
	struct sippymodem_v44_transparent_decoder *transparent;
	enum v44_decode_mode mode;
};

static bool decoder_is_valid(const struct sippymodem_v44_decoder *decoder)
{
	return decoder != NULL && decoder->compressed != NULL &&
	       decoder->transparent != NULL &&
	       decoder->mode >= V44_DECODE_COMPRESSED &&
	       decoder->mode <= V44_DECODE_FAILED;
}

struct sippymodem_v44_decoder *sippymodem_v44_decoder_create(
	const struct sippymodem_v44_parameters *parameters,
	size_t compressed_output_capacity)
{
	struct sippymodem_v44_decoder *decoder;

	if (!sippymodem_v44_parameters_are_valid(parameters))
		return NULL;
	decoder = calloc(1, sizeof(*decoder));
	if (decoder == NULL)
		return NULL;
	decoder->compressed = sippymodem_v44_compressed_decoder_create(
		parameters, compressed_output_capacity);
	decoder->transparent = sippymodem_v44_transparent_decoder_create();
	if (decoder->compressed == NULL || decoder->transparent == NULL) {
		sippymodem_v44_decoder_destroy(decoder);
		return NULL;
	}
	return decoder;
}

void sippymodem_v44_decoder_destroy(
	struct sippymodem_v44_decoder *decoder)
{
	if (decoder == NULL)
		return;
	sippymodem_v44_transparent_decoder_destroy(decoder->transparent);
	sippymodem_v44_compressed_decoder_destroy(decoder->compressed);
	free(decoder);
}

static enum sippymodem_v44_decode_result fail_decoder(
	struct sippymodem_v44_decoder *decoder)
{
	decoder->mode = V44_DECODE_FAILED;
	return SIPPYMODEM_V44_DECODE_PROCEDURAL_ERROR;
}

static enum sippymodem_v44_decode_result drain_compressed(
	struct sippymodem_v44_decoder *decoder,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length)
{
	if (output_capacity == 0)
		return SIPPYMODEM_V44_DECODE_NEED_OUTPUT;
	*output_length = sippymodem_v44_compressed_decoder_read(
		decoder->compressed, output, output_capacity);
	return *output_length != 0 ? SIPPYMODEM_V44_DECODE_OK :
				     fail_decoder(decoder);
}

static enum sippymodem_v44_decode_result process_compressed(
	struct sippymodem_v44_decoder *decoder,
	const uint8_t **input,
	size_t *input_length,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length)
{
	enum sippymodem_v44_compressed_result result =
		sippymodem_v44_compressed_decoder_process(
			decoder->compressed, input, input_length);

	switch (result) {
	case SIPPYMODEM_V44_COMPRESSED_OK:
	case SIPPYMODEM_V44_COMPRESSED_NEED_OUTPUT:
		if (sippymodem_v44_compressed_decoder_pending(
			    decoder->compressed) != 0)
			return drain_compressed(
				decoder, output, output_capacity, output_length);
		return result == SIPPYMODEM_V44_COMPRESSED_OK ?
			       SIPPYMODEM_V44_DECODE_OK : fail_decoder(decoder);
	case SIPPYMODEM_V44_COMPRESSED_NEED_INPUT:
		return SIPPYMODEM_V44_DECODE_NEED_INPUT;
	case SIPPYMODEM_V44_COMPRESSED_FLUSHED:
		return sippymodem_v44_compressed_decoder_pending(
			       decoder->compressed) == 0 ?
			       SIPPYMODEM_V44_DECODE_FLUSHED : fail_decoder(decoder);
	case SIPPYMODEM_V44_COMPRESSED_ENTER_TRANSPARENT:
		if (sippymodem_v44_compressed_decoder_pending(
			    decoder->compressed) != 0 ||
		    sippymodem_v44_transparent_decoder_enter(
			    decoder->transparent) !=
			    SIPPYMODEM_V44_TRANSPARENT_OK)
			return fail_decoder(decoder);
		decoder->mode = V44_DECODE_TRANSPARENT;
		return SIPPYMODEM_V44_DECODE_OK;
	case SIPPYMODEM_V44_COMPRESSED_INVALID_ARGUMENT:
	case SIPPYMODEM_V44_COMPRESSED_PROCEDURAL_ERROR:
		return fail_decoder(decoder);
	}
	return fail_decoder(decoder);
}

static enum sippymodem_v44_decode_result process_transparent(
	struct sippymodem_v44_decoder *decoder,
	const uint8_t **input,
	size_t *input_length,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length)
{
	enum sippymodem_v44_transparent_result result =
		sippymodem_v44_transparent_decoder_process(
			decoder->transparent, input, input_length, output,
			output_capacity, output_length);

	switch (result) {
	case SIPPYMODEM_V44_TRANSPARENT_OK:
		return SIPPYMODEM_V44_DECODE_OK;
	case SIPPYMODEM_V44_TRANSPARENT_NEED_INPUT:
		return SIPPYMODEM_V44_DECODE_NEED_INPUT;
	case SIPPYMODEM_V44_TRANSPARENT_NEED_OUTPUT:
		return SIPPYMODEM_V44_DECODE_NEED_OUTPUT;
	case SIPPYMODEM_V44_TRANSPARENT_ENTER_COMPRESSED:
		if (sippymodem_v44_compressed_decoder_enter(
			    decoder->compressed) != SIPPYMODEM_V44_COMPRESSED_OK)
			return fail_decoder(decoder);
		decoder->mode = V44_DECODE_COMPRESSED;
		return SIPPYMODEM_V44_DECODE_OK;
	case SIPPYMODEM_V44_TRANSPARENT_ENTER_PARAMETER:
		decoder->mode = V44_DECODE_PARAMETER;
		return SIPPYMODEM_V44_DECODE_ENTER_PARAMETER;
	case SIPPYMODEM_V44_TRANSPARENT_INVALID_ARGUMENT:
	case SIPPYMODEM_V44_TRANSPARENT_PROCEDURAL_ERROR:
		return fail_decoder(decoder);
	}
	return fail_decoder(decoder);
}

enum sippymodem_v44_decode_result sippymodem_v44_decoder_process(
	struct sippymodem_v44_decoder *decoder,
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
		return SIPPYMODEM_V44_DECODE_INVALID_ARGUMENT;
	*output_length = 0;
	if (decoder->mode == V44_DECODE_FAILED)
		return SIPPYMODEM_V44_DECODE_PROCEDURAL_ERROR;
	if (decoder->mode == V44_DECODE_PARAMETER)
		return SIPPYMODEM_V44_DECODE_ENTER_PARAMETER;
	if (sippymodem_v44_compressed_decoder_pending(decoder->compressed) != 0)
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

enum sippymodem_v44_decode_result
sippymodem_v44_decoder_resume_transparent(
	struct sippymodem_v44_decoder *decoder)
{
	if (!decoder_is_valid(decoder))
		return SIPPYMODEM_V44_DECODE_INVALID_ARGUMENT;
	if (decoder->mode == V44_DECODE_FAILED)
		return SIPPYMODEM_V44_DECODE_PROCEDURAL_ERROR;
	if (decoder->mode != V44_DECODE_PARAMETER)
		return SIPPYMODEM_V44_DECODE_INVALID_ARGUMENT;
	if (sippymodem_v44_transparent_decoder_enter(decoder->transparent) !=
	    SIPPYMODEM_V44_TRANSPARENT_OK)
		return fail_decoder(decoder);
	decoder->mode = V44_DECODE_TRANSPARENT;
	return SIPPYMODEM_V44_DECODE_OK;
}

enum sippymodem_v44_decode_result sippymodem_v44_decoder_finish(
	struct sippymodem_v44_decoder *decoder)
{
	if (!decoder_is_valid(decoder))
		return SIPPYMODEM_V44_DECODE_INVALID_ARGUMENT;
	if (decoder->mode == V44_DECODE_FAILED)
		return SIPPYMODEM_V44_DECODE_PROCEDURAL_ERROR;
	if (decoder->mode == V44_DECODE_PARAMETER)
		return SIPPYMODEM_V44_DECODE_ENTER_PARAMETER;
	if (sippymodem_v44_compressed_decoder_pending(decoder->compressed) != 0)
		return SIPPYMODEM_V44_DECODE_NEED_OUTPUT;
	if (decoder->mode == V44_DECODE_COMPRESSED)
		return sippymodem_v44_compressed_decoder_finish(
			       decoder->compressed) == SIPPYMODEM_V44_COMPRESSED_OK ?
			       SIPPYMODEM_V44_DECODE_OK : fail_decoder(decoder);
	return sippymodem_v44_transparent_decoder_finish(
		       decoder->transparent) == SIPPYMODEM_V44_TRANSPARENT_OK ?
		       SIPPYMODEM_V44_DECODE_OK : fail_decoder(decoder);
}

const char *sippymodem_v44_decode_result_name(
	enum sippymodem_v44_decode_result result)
{
	switch (result) {
	case SIPPYMODEM_V44_DECODE_OK:
		return "ok";
	case SIPPYMODEM_V44_DECODE_NEED_INPUT:
		return "need input";
	case SIPPYMODEM_V44_DECODE_NEED_OUTPUT:
		return "need output";
	case SIPPYMODEM_V44_DECODE_FLUSHED:
		return "flushed";
	case SIPPYMODEM_V44_DECODE_ENTER_PARAMETER:
		return "enter parameter";
	case SIPPYMODEM_V44_DECODE_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_V44_DECODE_PROCEDURAL_ERROR:
		return "procedural error";
	}
	return "unknown";
}
