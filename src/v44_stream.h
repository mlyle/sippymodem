// SPDX-License-Identifier: BSD-3-Clause

#ifndef V44_STREAM_H
#define V44_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V44_MIN_CODEWORDS = 256,
	V44_MAX_CODEWORDS = 65535,
	V44_DEFAULT_CODEWORDS = 1024,
	V44_MIN_STRING_LENGTH = 32,
	V44_MAX_STRING_LENGTH = 255,
	V44_DEFAULT_STRING_LENGTH = 255,
	V44_MIN_HISTORY_SIZE = 512,
	V44_MAX_HISTORY_SIZE = 65535,
	V44_DEFAULT_HISTORY_SIZE = 3072,
};

struct v44_parameters {
	uint32_t codeword_limit;
	uint32_t max_string_length;
	uint32_t history_size;
};

struct v44_parameters v44_default_parameters(void);
bool v44_parameters_are_valid(
	const struct v44_parameters *parameters);

/* V.44 6.6.2/Table 4: the long extension field is 5..8 bits.
 * The caller has validated the negotiated maximum string length (32..255). */
static inline unsigned int v44_extension_value_bits(uint32_t max_string_length)
{
	if (max_string_length <= 46)
		return 5;
	if (max_string_length <= 78)
		return 6;
	if (max_string_length <= 142)
		return 7;
	return 8;
}

enum v44_stream_result {
	V44_STREAM_OK = 0,
	V44_STREAM_NEED_INPUT,
	V44_STREAM_NEED_OUTPUT,
	V44_STREAM_INVALID_ARGUMENT,
};

struct v44_bit_writer;
struct v44_bit_reader;

/*
 * A writer owns a bounded byte queue.  Its capacity must be at least two
 * bytes, enough to accept any one V.44 field after the queue is drained.
 */
struct v44_bit_writer *v44_bit_writer_create(
	size_t output_capacity);
void v44_bit_writer_destroy(
	struct v44_bit_writer *writer);

/* Values are transferred least-significant bit first; at most 16 bits. */
enum v44_stream_result v44_bit_writer_put(
	struct v44_bit_writer *writer,
	uint32_t value,
	unsigned int bit_count);

/* Pad the current byte with zero bits. */
enum v44_stream_result v44_bit_writer_align(
	struct v44_bit_writer *writer);

size_t v44_bit_writer_pending(
	const struct v44_bit_writer *writer);
size_t v44_bit_writer_read(
	struct v44_bit_writer *writer,
	uint8_t *output,
	size_t output_capacity);

/* True when the next field starts at bit one of a new octet. */
bool v44_bit_writer_at_octet_boundary(
	const struct v44_bit_writer *writer);

struct v44_bit_reader *v44_bit_reader_create(void);
void v44_bit_reader_destroy(
	struct v44_bit_reader *reader);

/*
 * Input is consumed as needed.  NEED_INPUT retains a partial field and leaves
 * value unchanged; resume it with the same bit_count and the next input chunk.
 */
enum v44_stream_result v44_bit_reader_get(
	struct v44_bit_reader *reader,
	const uint8_t **input,
	size_t *input_length,
	unsigned int bit_count,
	uint32_t *value);

/* Validate zero padding and discard it through the next byte boundary. */
enum v44_stream_result v44_bit_reader_align(
	struct v44_bit_reader *reader);

/* True only when no buffered or partially collected field bits remain. */
bool v44_bit_reader_at_boundary(
	const struct v44_bit_reader *reader);

#endif
