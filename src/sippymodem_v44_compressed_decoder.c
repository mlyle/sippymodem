// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v44_compressed_decoder.h"

#include "sippymodem_v44_dictionary_decoder.h"

#include <stdbool.h>
#include <stdlib.h>

enum {
	V44_CONTROL_ETM = 0,
	V44_CONTROL_FLUSH = 1,
	V44_CONTROL_STEPUP = 2,
	V44_CONTROL_REINIT = 3,
	V44_FIRST_CODEWORD = 4,
	V44_INITIAL_CODEWORD_BITS = 6,
	V44_INITIAL_ORDINAL_BITS = 7,
};

enum v44_parse_state {
	V44_PARSE_PREFIX = 0,
	V44_PARSE_AFTER_CODEWORD_PREFIX,
	V44_PARSE_CODE,
	V44_PARSE_ORDINAL,
	V44_PARSE_EXTENSION_FIRST,
	V44_PARSE_EXTENSION_PAIR,
	V44_PARSE_EXTENSION_BRANCH,
	V44_PARSE_EXTENSION_THREE,
	V44_PARSE_EXTENSION_LONG,
};

enum v44_pending_type {
	V44_PENDING_NONE = 0,
	V44_PENDING_ORDINAL,
	V44_PENDING_CODEWORD,
	V44_PENDING_EXTENSION,
};

struct sippymodem_v44_compressed_decoder {
	struct sippymodem_v44_parameters parameters;
	struct sippymodem_v44_bit_reader *reader;
	struct sippymodem_v44_dictionary_decoder *dictionary;
	enum v44_parse_state state;
	enum v44_pending_type pending_type;
	uint32_t pending_value;
	unsigned int codeword_bits;
	unsigned int ordinal_bits;
	bool last_code_was_codeword;
	bool stepup_pending;
	bool transparent;
	bool failed;
};

static unsigned int maximum_codeword_bits(
	const struct sippymodem_v44_parameters *parameters)
{
	unsigned int bits = V44_INITIAL_CODEWORD_BITS;

	while ((UINT32_C(1) << bits) < parameters->codeword_limit)
		bits++;
	return bits;
}

static bool decoder_is_valid(
	const struct sippymodem_v44_compressed_decoder *decoder)
{
	return decoder != NULL &&
	       sippymodem_v44_parameters_are_valid(&decoder->parameters) &&
	       decoder->reader != NULL && decoder->dictionary != NULL &&
	       decoder->state >= V44_PARSE_PREFIX &&
	       decoder->state <= V44_PARSE_EXTENSION_LONG &&
	       decoder->pending_type >= V44_PENDING_NONE &&
	       decoder->pending_type <= V44_PENDING_EXTENSION &&
	       (decoder->pending_type == V44_PENDING_NONE ||
		decoder->state == V44_PARSE_PREFIX) &&
	       decoder->codeword_bits >= V44_INITIAL_CODEWORD_BITS &&
	       decoder->codeword_bits <= maximum_codeword_bits(
		       &decoder->parameters) &&
	       decoder->ordinal_bits >= V44_INITIAL_ORDINAL_BITS &&
	       decoder->ordinal_bits <= 8 &&
	       !(decoder->last_code_was_codeword && decoder->stepup_pending) &&
	       !(decoder->transparent &&
		 (decoder->failed || decoder->pending_type != V44_PENDING_NONE ||
		  decoder->state != V44_PARSE_PREFIX || decoder->stepup_pending));
}

struct sippymodem_v44_compressed_decoder *
sippymodem_v44_compressed_decoder_create(
	const struct sippymodem_v44_parameters *parameters,
	size_t output_capacity)
{
	struct sippymodem_v44_compressed_decoder *decoder;

	if (!sippymodem_v44_parameters_are_valid(parameters))
		return NULL;
	decoder = calloc(1, sizeof(*decoder));
	if (decoder == NULL)
		return NULL;
	decoder->parameters = *parameters;
	decoder->codeword_bits = V44_INITIAL_CODEWORD_BITS;
	decoder->ordinal_bits = V44_INITIAL_ORDINAL_BITS;
	decoder->reader = sippymodem_v44_bit_reader_create();
	decoder->dictionary = sippymodem_v44_dictionary_decoder_create(
		parameters, output_capacity);
	if (decoder->reader == NULL || decoder->dictionary == NULL) {
		sippymodem_v44_compressed_decoder_destroy(decoder);
		return NULL;
	}
	return decoder;
}

void sippymodem_v44_compressed_decoder_destroy(
	struct sippymodem_v44_compressed_decoder *decoder)
{
	if (decoder == NULL)
		return;
	sippymodem_v44_dictionary_decoder_destroy(decoder->dictionary);
	sippymodem_v44_bit_reader_destroy(decoder->reader);
	free(decoder);
}

static enum sippymodem_v44_compressed_result fail_decoder(
	struct sippymodem_v44_compressed_decoder *decoder)
{
	decoder->failed = true;
	return SIPPYMODEM_V44_COMPRESSED_PROCEDURAL_ERROR;
}

static enum sippymodem_v44_compressed_result reinitialize_decoder(
	struct sippymodem_v44_compressed_decoder *decoder)
{
	if (sippymodem_v44_dictionary_decoder_reinitialize(
		    decoder->dictionary) != SIPPYMODEM_V44_DICTIONARY_OK)
		return fail_decoder(decoder);
	decoder->state = V44_PARSE_PREFIX;
	decoder->pending_type = V44_PENDING_NONE;
	decoder->pending_value = 0;
	decoder->codeword_bits = V44_INITIAL_CODEWORD_BITS;
	decoder->ordinal_bits = V44_INITIAL_ORDINAL_BITS;
	decoder->last_code_was_codeword = false;
	decoder->stepup_pending = false;
	decoder->transparent = false;
	return SIPPYMODEM_V44_COMPRESSED_OK;
}

enum sippymodem_v44_compressed_result
sippymodem_v44_compressed_decoder_enter(
	struct sippymodem_v44_compressed_decoder *decoder)
{
	if (!decoder_is_valid(decoder))
		return SIPPYMODEM_V44_COMPRESSED_INVALID_ARGUMENT;
	if (decoder->failed)
		return SIPPYMODEM_V44_COMPRESSED_PROCEDURAL_ERROR;
	if (!decoder->transparent ||
	    !sippymodem_v44_bit_reader_at_boundary(decoder->reader))
		return SIPPYMODEM_V44_COMPRESSED_INVALID_ARGUMENT;
	return reinitialize_decoder(decoder);
}

static enum sippymodem_v44_compressed_result read_bits(
	struct sippymodem_v44_compressed_decoder *decoder,
	const uint8_t **input,
	size_t *input_length,
	unsigned int bit_count,
	uint32_t *value)
{
	enum sippymodem_v44_stream_result result =
		sippymodem_v44_bit_reader_get(
			decoder->reader, input, input_length, bit_count, value);

	if (result == SIPPYMODEM_V44_STREAM_OK)
		return SIPPYMODEM_V44_COMPRESSED_OK;
	if (result == SIPPYMODEM_V44_STREAM_NEED_INPUT)
		return SIPPYMODEM_V44_COMPRESSED_NEED_INPUT;
	return fail_decoder(decoder);
}

static enum sippymodem_v44_compressed_result apply_pending(
	struct sippymodem_v44_compressed_decoder *decoder)
{
	enum sippymodem_v44_dictionary_result dictionary_result;
	enum v44_pending_type pending_type = decoder->pending_type;

	switch (pending_type) {
	case V44_PENDING_ORDINAL:
		dictionary_result =
			sippymodem_v44_dictionary_decoder_decode_ordinal(
				decoder->dictionary,
				(uint8_t)decoder->pending_value);
		break;
	case V44_PENDING_CODEWORD:
		dictionary_result =
			sippymodem_v44_dictionary_decoder_decode_codeword(
				decoder->dictionary, decoder->pending_value);
		break;
	case V44_PENDING_EXTENSION:
		dictionary_result =
			sippymodem_v44_dictionary_decoder_decode_extension(
				decoder->dictionary, decoder->pending_value);
		break;
	case V44_PENDING_NONE:
		return SIPPYMODEM_V44_COMPRESSED_OK;
	default:
		return fail_decoder(decoder);
	}
	if (dictionary_result == SIPPYMODEM_V44_DICTIONARY_NEED_OUTPUT)
		return SIPPYMODEM_V44_COMPRESSED_NEED_OUTPUT;
	if (dictionary_result != SIPPYMODEM_V44_DICTIONARY_OK)
		return fail_decoder(decoder);
	decoder->pending_type = V44_PENDING_NONE;
	decoder->pending_value = 0;
	decoder->last_code_was_codeword =
		pending_type == V44_PENDING_CODEWORD;
	return SIPPYMODEM_V44_COMPRESSED_OK;
}

static enum sippymodem_v44_compressed_result queue_decoded_code(
	struct sippymodem_v44_compressed_decoder *decoder,
	enum v44_pending_type type,
	uint32_t value)
{
	decoder->state = V44_PARSE_PREFIX;
	decoder->pending_type = type;
	decoder->pending_value = value;
	return apply_pending(decoder);
}

static enum sippymodem_v44_compressed_result apply_stepup(
	struct sippymodem_v44_compressed_decoder *decoder,
	bool ordinal)
{
	if (ordinal) {
		if (decoder->ordinal_bits >= 8)
			return fail_decoder(decoder);
		decoder->ordinal_bits = 8;
	} else {
		if (decoder->codeword_bits >=
		    maximum_codeword_bits(&decoder->parameters))
			return fail_decoder(decoder);
		decoder->codeword_bits++;
	}
	decoder->stepup_pending = false;
	return SIPPYMODEM_V44_COMPRESSED_OK;
}

static enum sippymodem_v44_compressed_result align_reader(
	struct sippymodem_v44_compressed_decoder *decoder)
{
	if (sippymodem_v44_bit_reader_align(decoder->reader) !=
	    SIPPYMODEM_V44_STREAM_OK)
		return fail_decoder(decoder);
	return SIPPYMODEM_V44_COMPRESSED_OK;
}

static enum sippymodem_v44_compressed_result process_control(
	struct sippymodem_v44_compressed_decoder *decoder,
	uint32_t control)
{
	decoder->state = V44_PARSE_PREFIX;
	decoder->last_code_was_codeword = false;
	switch (control) {
	case V44_CONTROL_ETM:
		if (align_reader(decoder) != SIPPYMODEM_V44_COMPRESSED_OK)
			return SIPPYMODEM_V44_COMPRESSED_PROCEDURAL_ERROR;
		decoder->transparent = true;
		return SIPPYMODEM_V44_COMPRESSED_ENTER_TRANSPARENT;
	case V44_CONTROL_FLUSH:
		if (align_reader(decoder) != SIPPYMODEM_V44_COMPRESSED_OK)
			return SIPPYMODEM_V44_COMPRESSED_PROCEDURAL_ERROR;
		return SIPPYMODEM_V44_COMPRESSED_FLUSHED;
	case V44_CONTROL_STEPUP:
		decoder->stepup_pending = true;
		return SIPPYMODEM_V44_COMPRESSED_OK;
	case V44_CONTROL_REINIT:
		return reinitialize_decoder(decoder);
	default:
		return fail_decoder(decoder);
	}
}

static unsigned int extension_value_bits(
	const struct sippymodem_v44_compressed_decoder *decoder)
{
	if (decoder->parameters.max_string_length <= 46)
		return 5;
	if (decoder->parameters.max_string_length <= 78)
		return 6;
	if (decoder->parameters.max_string_length <= 142)
		return 7;
	return 8;
}

static enum sippymodem_v44_compressed_result queue_extension(
	struct sippymodem_v44_compressed_decoder *decoder,
	uint32_t length)
{
	if (length == 0 ||
	    length > decoder->parameters.max_string_length - 2)
		return fail_decoder(decoder);
	return queue_decoded_code(decoder, V44_PENDING_EXTENSION, length);
}

enum sippymodem_v44_compressed_result
sippymodem_v44_compressed_decoder_process(
	struct sippymodem_v44_compressed_decoder *decoder,
	const uint8_t **input,
	size_t *input_length)
{
	uint32_t value;
	enum sippymodem_v44_compressed_result result;

	if (!decoder_is_valid(decoder) || input == NULL ||
	    input_length == NULL || (*input_length != 0 && *input == NULL))
		return SIPPYMODEM_V44_COMPRESSED_INVALID_ARGUMENT;
	if (decoder->failed)
		return SIPPYMODEM_V44_COMPRESSED_PROCEDURAL_ERROR;
	if (decoder->transparent)
		return SIPPYMODEM_V44_COMPRESSED_ENTER_TRANSPARENT;
	if (decoder->pending_type != V44_PENDING_NONE)
		return apply_pending(decoder);

	for (;;) {
		switch (decoder->state) {
		case V44_PARSE_PREFIX:
			result = read_bits(decoder, input, input_length, 1, &value);
			if (result != SIPPYMODEM_V44_COMPRESSED_OK)
				return result;
			if (value != 0) {
				if (decoder->stepup_pending &&
				    apply_stepup(decoder, false) !=
					    SIPPYMODEM_V44_COMPRESSED_OK)
					return SIPPYMODEM_V44_COMPRESSED_PROCEDURAL_ERROR;
				decoder->state = V44_PARSE_CODE;
			} else if (decoder->last_code_was_codeword &&
				   !decoder->stepup_pending) {
				decoder->state = V44_PARSE_AFTER_CODEWORD_PREFIX;
			} else {
				if (decoder->stepup_pending &&
				    apply_stepup(decoder, true) !=
					    SIPPYMODEM_V44_COMPRESSED_OK)
					return SIPPYMODEM_V44_COMPRESSED_PROCEDURAL_ERROR;
				decoder->state = V44_PARSE_ORDINAL;
			}
			break;
		case V44_PARSE_AFTER_CODEWORD_PREFIX:
			result = read_bits(decoder, input, input_length, 1, &value);
			if (result != SIPPYMODEM_V44_COMPRESSED_OK)
				return result;
			decoder->state = value != 0 ? V44_PARSE_EXTENSION_FIRST :
						    V44_PARSE_ORDINAL;
			break;
		case V44_PARSE_CODE:
			result = read_bits(
				decoder, input, input_length,
				decoder->codeword_bits, &value);
			if (result != SIPPYMODEM_V44_COMPRESSED_OK)
				return result;
			if (value < V44_FIRST_CODEWORD)
				return process_control(decoder, value);
			return queue_decoded_code(
				decoder, V44_PENDING_CODEWORD, value);
		case V44_PARSE_ORDINAL:
			result = read_bits(
				decoder, input, input_length,
				decoder->ordinal_bits, &value);
			if (result != SIPPYMODEM_V44_COMPRESSED_OK)
				return result;
			return queue_decoded_code(
				decoder, V44_PENDING_ORDINAL, value);
		case V44_PARSE_EXTENSION_FIRST:
			result = read_bits(decoder, input, input_length, 1, &value);
			if (result != SIPPYMODEM_V44_COMPRESSED_OK)
				return result;
			if (value != 0)
				return queue_extension(decoder, 1);
			decoder->state = V44_PARSE_EXTENSION_PAIR;
			break;
		case V44_PARSE_EXTENSION_PAIR:
			result = read_bits(decoder, input, input_length, 2, &value);
			if (result != SIPPYMODEM_V44_COMPRESSED_OK)
				return result;
			if (value != 0)
				return queue_extension(decoder, value + 1);
			decoder->state = V44_PARSE_EXTENSION_BRANCH;
			break;
		case V44_PARSE_EXTENSION_BRANCH:
			result = read_bits(decoder, input, input_length, 1, &value);
			if (result != SIPPYMODEM_V44_COMPRESSED_OK)
				return result;
			decoder->state = value != 0 ? V44_PARSE_EXTENSION_LONG :
						    V44_PARSE_EXTENSION_THREE;
			break;
		case V44_PARSE_EXTENSION_THREE:
			result = read_bits(decoder, input, input_length, 3, &value);
			if (result != SIPPYMODEM_V44_COMPRESSED_OK)
				return result;
			return queue_extension(decoder, value + 5);
		case V44_PARSE_EXTENSION_LONG:
			result = read_bits(
				decoder, input, input_length,
				extension_value_bits(decoder), &value);
			if (result != SIPPYMODEM_V44_COMPRESSED_OK)
				return result;
			return queue_extension(decoder, value + 13);
		}
	}
}

enum sippymodem_v44_compressed_result
sippymodem_v44_compressed_decoder_finish(
	struct sippymodem_v44_compressed_decoder *decoder)
{
	if (!decoder_is_valid(decoder))
		return SIPPYMODEM_V44_COMPRESSED_INVALID_ARGUMENT;
	if (decoder->failed)
		return SIPPYMODEM_V44_COMPRESSED_PROCEDURAL_ERROR;
	if (decoder->transparent)
		return SIPPYMODEM_V44_COMPRESSED_ENTER_TRANSPARENT;
	if (decoder->pending_type != V44_PENDING_NONE)
		return SIPPYMODEM_V44_COMPRESSED_NEED_OUTPUT;
	if (decoder->state != V44_PARSE_PREFIX || decoder->stepup_pending ||
	    !sippymodem_v44_bit_reader_at_boundary(decoder->reader))
		return fail_decoder(decoder);
	return SIPPYMODEM_V44_COMPRESSED_OK;
}

size_t sippymodem_v44_compressed_decoder_pending(
	const struct sippymodem_v44_compressed_decoder *decoder)
{
	return decoder_is_valid(decoder) ?
		       sippymodem_v44_dictionary_decoder_pending(
			       decoder->dictionary) :
		       0;
}

size_t sippymodem_v44_compressed_decoder_read(
	struct sippymodem_v44_compressed_decoder *decoder,
	uint8_t *output,
	size_t output_capacity)
{
	return decoder_is_valid(decoder) ?
		       sippymodem_v44_dictionary_decoder_read(
			       decoder->dictionary, output, output_capacity) :
		       0;
}

const char *sippymodem_v44_compressed_result_name(
	enum sippymodem_v44_compressed_result result)
{
	switch (result) {
	case SIPPYMODEM_V44_COMPRESSED_OK:
		return "ok";
	case SIPPYMODEM_V44_COMPRESSED_NEED_INPUT:
		return "need input";
	case SIPPYMODEM_V44_COMPRESSED_NEED_OUTPUT:
		return "need output";
	case SIPPYMODEM_V44_COMPRESSED_FLUSHED:
		return "flushed";
	case SIPPYMODEM_V44_COMPRESSED_ENTER_TRANSPARENT:
		return "enter transparent";
	case SIPPYMODEM_V44_COMPRESSED_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_V44_COMPRESSED_PROCEDURAL_ERROR:
		return "procedural error";
	}
	return "unknown";
}
