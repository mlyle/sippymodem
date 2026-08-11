// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v44_dictionary_decoder.h"

#include <stdbool.h>
#include <stdlib.h>

enum {
	V44_FIRST_CODEWORD = 4,
	V44_MAX_EXTENSION_LENGTH = 253,
};

enum v44_previous_code_type {
	V44_PREVIOUS_NONE = 0,
	V44_PREVIOUS_ORDINAL,
	V44_PREVIOUS_CODEWORD,
	V44_PREVIOUS_EXTENSION,
};

struct v44_decoded_string {
	uint32_t last_history_index;
	uint32_t length;
};

struct v44_previous_code {
	enum v44_previous_code_type type;
	uint32_t codeword;
	uint32_t history_start;
	uint32_t length;
	uint8_t first;
};

struct sippymodem_v44_dictionary_decoder {
	struct sippymodem_v44_parameters parameters;
	struct v44_decoded_string *strings;
	uint8_t *history;
	uint8_t *output;
	struct v44_previous_code previous;
	uint32_t next_codeword;
	size_t history_length;
	size_t output_capacity;
	size_t output_head;
	size_t output_tail;
	size_t output_length;
};

static bool previous_code_is_valid(
	const struct sippymodem_v44_dictionary_decoder *decoder)
{
	const struct v44_previous_code *previous = &decoder->previous;

	switch (previous->type) {
	case V44_PREVIOUS_NONE:
	case V44_PREVIOUS_EXTENSION:
		return previous->length == 0;
	case V44_PREVIOUS_ORDINAL:
		return previous->length == 1 &&
		       previous->history_start < decoder->history_length &&
		       decoder->history[previous->history_start] == previous->first;
	case V44_PREVIOUS_CODEWORD:
		return previous->codeword >= V44_FIRST_CODEWORD &&
		       previous->codeword < decoder->next_codeword &&
		       previous->length >= 2 &&
		       previous->length <= decoder->parameters.max_string_length &&
		       previous->history_start < decoder->history_length &&
		       previous->length <=
			       decoder->history_length - previous->history_start &&
		       decoder->history[previous->history_start] == previous->first;
	}
	return false;
}

static bool decoder_is_valid(
	const struct sippymodem_v44_dictionary_decoder *decoder)
{
	return decoder != NULL &&
	       sippymodem_v44_parameters_are_valid(&decoder->parameters) &&
	       decoder->strings != NULL && decoder->history != NULL &&
	       decoder->output != NULL &&
	       decoder->output_capacity >= decoder->parameters.max_string_length &&
	       decoder->output_head < decoder->output_capacity &&
	       decoder->output_tail < decoder->output_capacity &&
	       decoder->output_length <= decoder->output_capacity &&
	       decoder->history_length <= decoder->parameters.history_size &&
	       decoder->next_codeword >= V44_FIRST_CODEWORD &&
	       decoder->next_codeword <= decoder->parameters.codeword_limit &&
	       previous_code_is_valid(decoder);
}

static void reset_dictionary(
	struct sippymodem_v44_dictionary_decoder *decoder)
{
	decoder->next_codeword = V44_FIRST_CODEWORD;
	decoder->history_length = 0;
	decoder->previous = (struct v44_previous_code){ 0 };
}

struct sippymodem_v44_dictionary_decoder *
sippymodem_v44_dictionary_decoder_create(
	const struct sippymodem_v44_parameters *parameters,
	size_t output_capacity)
{
	struct sippymodem_v44_dictionary_decoder *decoder;

	if (!sippymodem_v44_parameters_are_valid(parameters) ||
	    output_capacity < parameters->max_string_length)
		return NULL;
	decoder = calloc(1, sizeof(*decoder));
	if (decoder == NULL)
		return NULL;
	decoder->parameters = *parameters;
	decoder->output_capacity = output_capacity;
	decoder->strings = calloc(
		parameters->codeword_limit, sizeof(*decoder->strings));
	decoder->history = malloc(parameters->history_size);
	decoder->output = malloc(output_capacity);
	if (decoder->strings == NULL || decoder->history == NULL ||
	    decoder->output == NULL) {
		sippymodem_v44_dictionary_decoder_destroy(decoder);
		return NULL;
	}
	reset_dictionary(decoder);
	return decoder;
}

void sippymodem_v44_dictionary_decoder_destroy(
	struct sippymodem_v44_dictionary_decoder *decoder)
{
	if (decoder == NULL)
		return;
	free(decoder->strings);
	free(decoder->history);
	free(decoder->output);
	free(decoder);
}

enum sippymodem_v44_dictionary_result
sippymodem_v44_dictionary_decoder_reinitialize(
	struct sippymodem_v44_dictionary_decoder *decoder)
{
	if (!decoder_is_valid(decoder))
		return SIPPYMODEM_V44_DICTIONARY_INVALID_ARGUMENT;
	reset_dictionary(decoder);
	return SIPPYMODEM_V44_DICTIONARY_OK;
}

static bool previous_accepts_append(
	const struct sippymodem_v44_dictionary_decoder *decoder)
{
	return (decoder->previous.type == V44_PREVIOUS_ORDINAL ||
		decoder->previous.type == V44_PREVIOUS_CODEWORD) &&
	       decoder->previous.length < decoder->parameters.max_string_length;
}

static enum sippymodem_v44_dictionary_result reserve_decoded_string(
	const struct sippymodem_v44_dictionary_decoder *decoder,
	size_t length,
	bool creates_string)
{
	if (length == 0 || length > decoder->parameters.max_string_length ||
	    length > decoder->parameters.history_size - decoder->history_length ||
	    (creates_string &&
	     decoder->next_codeword >= decoder->parameters.codeword_limit))
		return SIPPYMODEM_V44_DICTIONARY_PROCEDURAL_ERROR;
	if (length > decoder->output_capacity - decoder->output_length)
		return SIPPYMODEM_V44_DICTIONARY_NEED_OUTPUT;
	return SIPPYMODEM_V44_DICTIONARY_OK;
}

static void emit_decoded_byte(
	struct sippymodem_v44_dictionary_decoder *decoder,
	uint8_t value)
{
	decoder->history[decoder->history_length++] = value;
	decoder->output[decoder->output_tail++] = value;
	if (decoder->output_tail == decoder->output_capacity)
		decoder->output_tail = 0;
	decoder->output_length++;
}

static void create_appended_string(
	struct sippymodem_v44_dictionary_decoder *decoder,
	uint32_t current_start)
{
	struct v44_decoded_string *string =
		&decoder->strings[decoder->next_codeword++];

	string->last_history_index = current_start;
	string->length = decoder->previous.length + 1;
}

enum sippymodem_v44_dictionary_result
sippymodem_v44_dictionary_decoder_decode_ordinal(
	struct sippymodem_v44_dictionary_decoder *decoder,
	uint8_t ordinal)
{
	bool append;
	uint32_t start;
	enum sippymodem_v44_dictionary_result result;

	if (!decoder_is_valid(decoder))
		return SIPPYMODEM_V44_DICTIONARY_INVALID_ARGUMENT;
	append = previous_accepts_append(decoder);
	result = reserve_decoded_string(decoder, 1, append);
	if (result != SIPPYMODEM_V44_DICTIONARY_OK)
		return result;
	start = (uint32_t)decoder->history_length;
	emit_decoded_byte(decoder, ordinal);
	if (append)
		create_appended_string(decoder, start);
	decoder->previous = (struct v44_previous_code){
		.type = V44_PREVIOUS_ORDINAL,
		.history_start = start,
		.length = 1,
		.first = ordinal,
	};
	return SIPPYMODEM_V44_DICTIONARY_OK;
}

static bool decoded_string_is_valid(
	const struct sippymodem_v44_dictionary_decoder *decoder,
	const struct v44_decoded_string *string)
{
	return string->length >= 2 &&
	       string->length <= decoder->parameters.max_string_length &&
	       string->last_history_index < decoder->history_length &&
	       string->length <= string->last_history_index + 1;
}

enum sippymodem_v44_dictionary_result
sippymodem_v44_dictionary_decoder_decode_codeword(
	struct sippymodem_v44_dictionary_decoder *decoder,
	uint32_t codeword)
{
	const struct v44_decoded_string *string = NULL;
	uint32_t source;
	uint32_t length;
	uint32_t start;
	uint8_t first;
	bool append;
	bool next;
	enum sippymodem_v44_dictionary_result result;

	if (!decoder_is_valid(decoder))
		return SIPPYMODEM_V44_DICTIONARY_INVALID_ARGUMENT;
	if (codeword < V44_FIRST_CODEWORD ||
	    codeword > decoder->next_codeword ||
	    codeword >= decoder->parameters.codeword_limit)
		return SIPPYMODEM_V44_DICTIONARY_PROCEDURAL_ERROR;
	next = codeword == decoder->next_codeword;
	append = previous_accepts_append(decoder);
	if (next) {
		if (!append)
			return SIPPYMODEM_V44_DICTIONARY_PROCEDURAL_ERROR;
		source = decoder->previous.history_start;
		length = decoder->previous.length + 1;
		first = decoder->previous.first;
	} else {
		string = &decoder->strings[codeword];
		if (!decoded_string_is_valid(decoder, string))
			return SIPPYMODEM_V44_DICTIONARY_PROCEDURAL_ERROR;
		length = string->length;
		source = string->last_history_index + 1 - length;
		first = decoder->history[source];
	}
	result = reserve_decoded_string(decoder, length, append);
	if (result != SIPPYMODEM_V44_DICTIONARY_OK)
		return result;
	start = (uint32_t)decoder->history_length;
	if (next) {
		for (uint32_t index = 0; index < length - 1; ++index)
			emit_decoded_byte(decoder, decoder->history[source + index]);
		emit_decoded_byte(decoder, first);
	} else {
		for (uint32_t index = 0; index < length; ++index)
			emit_decoded_byte(decoder, decoder->history[source + index]);
	}
	if (append)
		create_appended_string(decoder, start);
	decoder->previous = (struct v44_previous_code){
		.type = V44_PREVIOUS_CODEWORD,
		.codeword = codeword,
		.history_start = start,
		.length = length,
		.first = first,
	};
	return SIPPYMODEM_V44_DICTIONARY_OK;
}

enum sippymodem_v44_dictionary_result
sippymodem_v44_dictionary_decoder_decode_extension(
	struct sippymodem_v44_dictionary_decoder *decoder,
	uint32_t extension_length)
{
	const struct v44_decoded_string *base;
	uint32_t source;
	uint32_t total_length;
	enum sippymodem_v44_dictionary_result result;

	if (!decoder_is_valid(decoder))
		return SIPPYMODEM_V44_DICTIONARY_INVALID_ARGUMENT;
	if (decoder->previous.type != V44_PREVIOUS_CODEWORD ||
	    extension_length == 0 ||
	    extension_length > V44_MAX_EXTENSION_LENGTH ||
	    extension_length > decoder->parameters.max_string_length -
		decoder->previous.length)
		return SIPPYMODEM_V44_DICTIONARY_PROCEDURAL_ERROR;
	base = &decoder->strings[decoder->previous.codeword];
	if (!decoded_string_is_valid(decoder, base) ||
	    base->last_history_index + 1 >= decoder->history_length)
		return SIPPYMODEM_V44_DICTIONARY_PROCEDURAL_ERROR;
	source = base->last_history_index + 1;
	total_length = decoder->previous.length + extension_length;
	result = reserve_decoded_string(decoder, extension_length, true);
	if (result != SIPPYMODEM_V44_DICTIONARY_OK)
		return result;
	for (uint32_t index = 0; index < extension_length; ++index)
		emit_decoded_byte(decoder, decoder->history[source + index]);
	decoder->strings[decoder->next_codeword++] =
		(struct v44_decoded_string){
			.last_history_index =
				(uint32_t)decoder->history_length - 1,
			.length = total_length,
		};
	decoder->previous = (struct v44_previous_code){
		.type = V44_PREVIOUS_EXTENSION,
	};
	return SIPPYMODEM_V44_DICTIONARY_OK;
}

size_t sippymodem_v44_dictionary_decoder_pending(
	const struct sippymodem_v44_dictionary_decoder *decoder)
{
	return decoder_is_valid(decoder) ? decoder->output_length : 0;
}

size_t sippymodem_v44_dictionary_decoder_read(
	struct sippymodem_v44_dictionary_decoder *decoder,
	uint8_t *output,
	size_t output_capacity)
{
	size_t count;

	if (!decoder_is_valid(decoder) || output == NULL ||
	    output_capacity == 0)
		return 0;
	count = decoder->output_length < output_capacity ?
		decoder->output_length : output_capacity;
	for (size_t index = 0; index < count; ++index) {
		output[index] = decoder->output[decoder->output_head++];
		if (decoder->output_head == decoder->output_capacity)
			decoder->output_head = 0;
	}
	decoder->output_length -= count;
	if (decoder->output_length == 0) {
		decoder->output_head = 0;
		decoder->output_tail = 0;
	}
	return count;
}

const char *sippymodem_v44_dictionary_result_name(
	enum sippymodem_v44_dictionary_result result)
{
	switch (result) {
	case SIPPYMODEM_V44_DICTIONARY_OK:
		return "ok";
	case SIPPYMODEM_V44_DICTIONARY_NEED_OUTPUT:
		return "need output";
	case SIPPYMODEM_V44_DICTIONARY_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_V44_DICTIONARY_PROCEDURAL_ERROR:
		return "procedural error";
	}
	return "unknown";
}
