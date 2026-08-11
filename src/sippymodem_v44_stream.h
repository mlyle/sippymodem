// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V44_STREAM_H
#define SIPPYMODEM_V44_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V44_MIN_CODEWORDS = 256,
	SIPPYMODEM_V44_MAX_CODEWORDS = 65535,
	SIPPYMODEM_V44_DEFAULT_CODEWORDS = 1024,
	SIPPYMODEM_V44_MIN_STRING_LENGTH = 32,
	SIPPYMODEM_V44_MAX_STRING_LENGTH = 255,
	SIPPYMODEM_V44_DEFAULT_STRING_LENGTH = 255,
	SIPPYMODEM_V44_MIN_HISTORY_SIZE = 512,
	SIPPYMODEM_V44_MAX_HISTORY_SIZE = 65535,
	SIPPYMODEM_V44_DEFAULT_HISTORY_SIZE = 3072,
};

struct sippymodem_v44_parameters {
	uint32_t codeword_limit;
	uint32_t max_string_length;
	uint32_t history_size;
};

struct sippymodem_v44_parameters sippymodem_v44_default_parameters(void);
bool sippymodem_v44_parameters_are_valid(
	const struct sippymodem_v44_parameters *parameters);

enum sippymodem_v44_stream_result {
	SIPPYMODEM_V44_STREAM_OK = 0,
	SIPPYMODEM_V44_STREAM_NEED_INPUT,
	SIPPYMODEM_V44_STREAM_NEED_OUTPUT,
	SIPPYMODEM_V44_STREAM_INVALID_ARGUMENT,
};

struct sippymodem_v44_bit_writer;
struct sippymodem_v44_bit_reader;

/*
 * A writer owns a bounded byte queue.  Its capacity must be at least two
 * bytes, enough to accept any one V.44 field after the queue is drained.
 */
struct sippymodem_v44_bit_writer *sippymodem_v44_bit_writer_create(
	size_t output_capacity);
void sippymodem_v44_bit_writer_destroy(
	struct sippymodem_v44_bit_writer *writer);

/* Values are transferred least-significant bit first; at most 16 bits. */
enum sippymodem_v44_stream_result sippymodem_v44_bit_writer_put(
	struct sippymodem_v44_bit_writer *writer,
	uint32_t value,
	unsigned int bit_count);

/* Pad the current byte with zero bits. */
enum sippymodem_v44_stream_result sippymodem_v44_bit_writer_align(
	struct sippymodem_v44_bit_writer *writer);

size_t sippymodem_v44_bit_writer_pending(
	const struct sippymodem_v44_bit_writer *writer);
size_t sippymodem_v44_bit_writer_read(
	struct sippymodem_v44_bit_writer *writer,
	uint8_t *output,
	size_t output_capacity);

struct sippymodem_v44_bit_reader *sippymodem_v44_bit_reader_create(void);
void sippymodem_v44_bit_reader_destroy(
	struct sippymodem_v44_bit_reader *reader);

/*
 * Input is consumed as needed.  NEED_INPUT retains a partial field and leaves
 * value unchanged; resume it with the same bit_count and the next input chunk.
 */
enum sippymodem_v44_stream_result sippymodem_v44_bit_reader_get(
	struct sippymodem_v44_bit_reader *reader,
	const uint8_t **input,
	size_t *input_length,
	unsigned int bit_count,
	uint32_t *value);

/* Validate zero padding and discard it through the next byte boundary. */
enum sippymodem_v44_stream_result sippymodem_v44_bit_reader_align(
	struct sippymodem_v44_bit_reader *reader);

/* True only when no buffered or partially collected field bits remain. */
bool sippymodem_v44_bit_reader_at_boundary(
	const struct sippymodem_v44_bit_reader *reader);

const char *sippymodem_v44_stream_result_name(
	enum sippymodem_v44_stream_result result);

#endif
