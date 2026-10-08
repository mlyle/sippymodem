// SPDX-License-Identifier: BSD-3-Clause

#include "v44_stream.h"

#include "byte_queue.h"

#include <stdint.h>
#include <stdlib.h>

enum {
	V44_MAX_FIELD_BITS = 16,
	V44_MIN_OUTPUT_CAPACITY = 2,
};

struct v44_bit_writer {
	uint32_t bits;
	unsigned int bit_count;
	struct byte_queue output;
};

struct v44_bit_reader {
	uint32_t bits;
	unsigned int bit_count;
	unsigned int pending_field_bits;
};

struct v44_parameters v44_default_parameters(void)
{
	const struct v44_parameters parameters = {
		.codeword_limit = V44_DEFAULT_CODEWORDS,
		.max_string_length = V44_DEFAULT_STRING_LENGTH,
		.history_size = V44_DEFAULT_HISTORY_SIZE,
	};

	return parameters;
}

bool v44_parameters_are_valid(
	const struct v44_parameters *parameters)
{
	return parameters != NULL &&
	       parameters->codeword_limit >= V44_MIN_CODEWORDS &&
	       parameters->codeword_limit <= V44_MAX_CODEWORDS &&
	       parameters->max_string_length >=
		       V44_MIN_STRING_LENGTH &&
	       parameters->max_string_length <=
		       V44_MAX_STRING_LENGTH &&
	       parameters->history_size >= V44_MIN_HISTORY_SIZE &&
	       parameters->history_size <= V44_MAX_HISTORY_SIZE;
}

struct v44_bit_writer *v44_bit_writer_create(
	size_t output_capacity)
{
	struct v44_bit_writer *writer;

	if (output_capacity < V44_MIN_OUTPUT_CAPACITY)
		return NULL;
	writer = calloc(1, sizeof(*writer));
	if (writer == NULL)
		return NULL;
	writer->output.storage = malloc(output_capacity);
	if (writer->output.storage == NULL) {
		free(writer);
		return NULL;
	}
	writer->output.capacity = output_capacity;
	return writer;
}

void v44_bit_writer_destroy(
	struct v44_bit_writer *writer)
{
	if (writer == NULL)
		return;
	free(writer->output.storage);
	free(writer);
}

static uint32_t v44_low_mask(unsigned int bit_count)
{
	return (UINT32_C(1) << bit_count) - UINT32_C(1);
}

enum v44_stream_result v44_bit_writer_put(
	struct v44_bit_writer *writer,
	uint32_t value,
	unsigned int bit_count)
{
	size_t emitted_bytes;
	uint8_t octets[2];

	if (writer == NULL || bit_count > V44_MAX_FIELD_BITS)
		return V44_STREAM_INVALID_ARGUMENT;
	if (bit_count == 0)
		return V44_STREAM_OK;

	emitted_bytes = (writer->bit_count + bit_count) / 8U;
	if (emitted_bytes > writer->output.capacity - writer->output.count)
		return V44_STREAM_NEED_OUTPUT;

	writer->bits |= (value & v44_low_mask(bit_count)) << writer->bit_count;
	writer->bit_count += bit_count;
	for (size_t index = 0; index < emitted_bytes; ++index) {
		octets[index] = (uint8_t)writer->bits;
		writer->bits >>= 8;
		writer->bit_count -= 8;
	}
	/* The complete field was reserved before changing the bit accumulator. */
	(void)byte_queue_write(&writer->output, octets, emitted_bytes);
	return V44_STREAM_OK;
}

enum v44_stream_result v44_bit_writer_align(
	struct v44_bit_writer *writer)
{
	unsigned int padding;

	if (writer == NULL)
		return V44_STREAM_INVALID_ARGUMENT;
	padding = (8U - writer->bit_count) & 7U;
	return v44_bit_writer_put(writer, 0, padding);
}

size_t v44_bit_writer_pending(
	const struct v44_bit_writer *writer)
{
	return writer != NULL ? writer->output.count : 0;
}

size_t v44_bit_writer_read(
	struct v44_bit_writer *writer,
	uint8_t *output,
	size_t output_capacity)
{
	if (writer == NULL || output == NULL || output_capacity == 0)
		return 0;
	return byte_queue_read(&writer->output, output, output_capacity);
}

bool v44_bit_writer_at_octet_boundary(
	const struct v44_bit_writer *writer)
{
	return writer != NULL && writer->bit_count == 0;
}

struct v44_bit_reader *v44_bit_reader_create(void)
{
	return calloc(1, sizeof(struct v44_bit_reader));
}

void v44_bit_reader_destroy(
	struct v44_bit_reader *reader)
{
	free(reader);
}

enum v44_stream_result v44_bit_reader_get(
	struct v44_bit_reader *reader,
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
		return V44_STREAM_INVALID_ARGUMENT;
	if (bit_count == 0) {
		*value = 0;
		return V44_STREAM_OK;
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
		return V44_STREAM_NEED_INPUT;
	}

	*value = reader->bits & v44_low_mask(bit_count);
	reader->bits >>= bit_count;
	reader->bit_count -= bit_count;
	reader->pending_field_bits = 0;
	return V44_STREAM_OK;
}

enum v44_stream_result v44_bit_reader_align(
	struct v44_bit_reader *reader)
{
	unsigned int padding;

	if (reader == NULL || reader->pending_field_bits != 0)
		return V44_STREAM_INVALID_ARGUMENT;
	padding = reader->bit_count & 7U;
	if ((reader->bits & v44_low_mask(padding)) != 0)
		return V44_STREAM_INVALID_ARGUMENT;
	reader->bits >>= padding;
	reader->bit_count -= padding;
	return V44_STREAM_OK;
}

bool v44_bit_reader_at_boundary(
	const struct v44_bit_reader *reader)
{
	return reader != NULL && reader->bit_count == 0 &&
	       reader->pending_field_bits == 0;
}

const char *v44_stream_result_name(
	enum v44_stream_result result)
{
	switch (result) {
	case V44_STREAM_OK:
		return "ok";
	case V44_STREAM_NEED_INPUT:
		return "need input";
	case V44_STREAM_NEED_OUTPUT:
		return "need output";
	case V44_STREAM_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
