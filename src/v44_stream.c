// SPDX-License-Identifier: BSD-3-Clause

#include "v44_stream.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
	V44_MAX_FIELD_BITS = 16,
	V44_MIN_OUTPUT_CAPACITY = 2,
};

struct sippymodem_v44_bit_writer {
	uint32_t bits;
	unsigned int bit_count;
	uint8_t *output;
	size_t output_capacity;
	size_t output_head;
	size_t output_tail;
	size_t output_length;
};

struct sippymodem_v44_bit_reader {
	uint32_t bits;
	unsigned int bit_count;
	unsigned int pending_field_bits;
};

struct sippymodem_v44_parameters sippymodem_v44_default_parameters(void)
{
	const struct sippymodem_v44_parameters parameters = {
		.codeword_limit = SIPPYMODEM_V44_DEFAULT_CODEWORDS,
		.max_string_length = SIPPYMODEM_V44_DEFAULT_STRING_LENGTH,
		.history_size = SIPPYMODEM_V44_DEFAULT_HISTORY_SIZE,
	};

	return parameters;
}

bool sippymodem_v44_parameters_are_valid(
	const struct sippymodem_v44_parameters *parameters)
{
	return parameters != NULL &&
	       parameters->codeword_limit >= SIPPYMODEM_V44_MIN_CODEWORDS &&
	       parameters->codeword_limit <= SIPPYMODEM_V44_MAX_CODEWORDS &&
	       parameters->max_string_length >=
		       SIPPYMODEM_V44_MIN_STRING_LENGTH &&
	       parameters->max_string_length <=
		       SIPPYMODEM_V44_MAX_STRING_LENGTH &&
	       parameters->history_size >= SIPPYMODEM_V44_MIN_HISTORY_SIZE &&
	       parameters->history_size <= SIPPYMODEM_V44_MAX_HISTORY_SIZE;
}

struct sippymodem_v44_bit_writer *sippymodem_v44_bit_writer_create(
	size_t output_capacity)
{
	struct sippymodem_v44_bit_writer *writer;

	if (output_capacity < V44_MIN_OUTPUT_CAPACITY)
		return NULL;
	writer = calloc(1, sizeof(*writer));
	if (writer == NULL)
		return NULL;
	writer->output = malloc(output_capacity);
	if (writer->output == NULL) {
		free(writer);
		return NULL;
	}
	writer->output_capacity = output_capacity;
	return writer;
}

void sippymodem_v44_bit_writer_destroy(
	struct sippymodem_v44_bit_writer *writer)
{
	if (writer == NULL)
		return;
	free(writer->output);
	free(writer);
}

static uint32_t v44_low_mask(unsigned int bit_count)
{
	return (UINT32_C(1) << bit_count) - UINT32_C(1);
}

static void v44_writer_emit_byte(
	struct sippymodem_v44_bit_writer *writer, uint8_t value)
{
	writer->output[writer->output_tail] = value;
	writer->output_tail++;
	if (writer->output_tail == writer->output_capacity)
		writer->output_tail = 0;
	writer->output_length++;
}

enum sippymodem_v44_stream_result sippymodem_v44_bit_writer_put(
	struct sippymodem_v44_bit_writer *writer,
	uint32_t value,
	unsigned int bit_count)
{
	size_t emitted_bytes;

	if (writer == NULL || bit_count > V44_MAX_FIELD_BITS)
		return SIPPYMODEM_V44_STREAM_INVALID_ARGUMENT;
	if (bit_count == 0)
		return SIPPYMODEM_V44_STREAM_OK;

	emitted_bytes = (writer->bit_count + bit_count) / 8U;
	if (emitted_bytes > writer->output_capacity - writer->output_length)
		return SIPPYMODEM_V44_STREAM_NEED_OUTPUT;

	writer->bits |= (value & v44_low_mask(bit_count)) << writer->bit_count;
	writer->bit_count += bit_count;
	while (writer->bit_count >= 8) {
		v44_writer_emit_byte(writer, (uint8_t)writer->bits);
		writer->bits >>= 8;
		writer->bit_count -= 8;
	}
	return SIPPYMODEM_V44_STREAM_OK;
}

enum sippymodem_v44_stream_result sippymodem_v44_bit_writer_align(
	struct sippymodem_v44_bit_writer *writer)
{
	unsigned int padding;

	if (writer == NULL)
		return SIPPYMODEM_V44_STREAM_INVALID_ARGUMENT;
	padding = (8U - writer->bit_count) & 7U;
	return sippymodem_v44_bit_writer_put(writer, 0, padding);
}

size_t sippymodem_v44_bit_writer_pending(
	const struct sippymodem_v44_bit_writer *writer)
{
	return writer != NULL ? writer->output_length : 0;
}

size_t sippymodem_v44_bit_writer_read(
	struct sippymodem_v44_bit_writer *writer,
	uint8_t *output,
	size_t output_capacity)
{
	size_t count;
	size_t first;

	if (writer == NULL || output == NULL || output_capacity == 0)
		return 0;
	count = writer->output_length < output_capacity ?
		writer->output_length : output_capacity;
	if (count == 0)
		return 0;

	first = writer->output_capacity - writer->output_head;
	if (first > count)
		first = count;
	memcpy(output, writer->output + writer->output_head, first);
	if (first < count)
		memcpy(output + first, writer->output, count - first);

	writer->output_head += first;
	if (writer->output_head == writer->output_capacity)
		writer->output_head = 0;
	if (first < count)
		writer->output_head = count - first;
	writer->output_length -= count;
	if (writer->output_length == 0) {
		writer->output_head = 0;
		writer->output_tail = 0;
	}
	return count;
}

bool sippymodem_v44_bit_writer_at_octet_boundary(
	const struct sippymodem_v44_bit_writer *writer)
{
	return writer != NULL && writer->bit_count == 0;
}

struct sippymodem_v44_bit_reader *sippymodem_v44_bit_reader_create(void)
{
	return calloc(1, sizeof(struct sippymodem_v44_bit_reader));
}

void sippymodem_v44_bit_reader_destroy(
	struct sippymodem_v44_bit_reader *reader)
{
	free(reader);
}

enum sippymodem_v44_stream_result sippymodem_v44_bit_reader_get(
	struct sippymodem_v44_bit_reader *reader,
	const uint8_t **input,
	size_t *input_length,
	unsigned int bit_count,
	uint32_t *value)
{
	if (reader == NULL || input == NULL || input_length == NULL ||
	    value == NULL || bit_count > V44_MAX_FIELD_BITS ||
	    (*input_length != 0 && *input == NULL) ||
	    (reader->pending_field_bits != 0 &&
	     reader->pending_field_bits != bit_count))
		return SIPPYMODEM_V44_STREAM_INVALID_ARGUMENT;
	if (bit_count == 0) {
		*value = 0;
		return SIPPYMODEM_V44_STREAM_OK;
	}

	while (reader->bit_count < bit_count && *input_length != 0) {
		reader->bits |= (uint32_t)**input << reader->bit_count;
		(*input)++;
		(*input_length)--;
		reader->bit_count += 8;
	}
	if (reader->bit_count < bit_count) {
		if (reader->bit_count != 0)
			reader->pending_field_bits = bit_count;
		return SIPPYMODEM_V44_STREAM_NEED_INPUT;
	}

	*value = reader->bits & v44_low_mask(bit_count);
	reader->bits >>= bit_count;
	reader->bit_count -= bit_count;
	reader->pending_field_bits = 0;
	return SIPPYMODEM_V44_STREAM_OK;
}

enum sippymodem_v44_stream_result sippymodem_v44_bit_reader_align(
	struct sippymodem_v44_bit_reader *reader)
{
	unsigned int padding;

	if (reader == NULL || reader->pending_field_bits != 0)
		return SIPPYMODEM_V44_STREAM_INVALID_ARGUMENT;
	padding = reader->bit_count & 7U;
	if ((reader->bits & v44_low_mask(padding)) != 0)
		return SIPPYMODEM_V44_STREAM_INVALID_ARGUMENT;
	reader->bits >>= padding;
	reader->bit_count -= padding;
	return SIPPYMODEM_V44_STREAM_OK;
}

bool sippymodem_v44_bit_reader_at_boundary(
	const struct sippymodem_v44_bit_reader *reader)
{
	return reader != NULL && reader->bit_count == 0 &&
	       reader->pending_field_bits == 0;
}

const char *sippymodem_v44_stream_result_name(
	enum sippymodem_v44_stream_result result)
{
	switch (result) {
	case SIPPYMODEM_V44_STREAM_OK:
		return "ok";
	case SIPPYMODEM_V44_STREAM_NEED_INPUT:
		return "need input";
	case SIPPYMODEM_V44_STREAM_NEED_OUTPUT:
		return "need output";
	case SIPPYMODEM_V44_STREAM_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
