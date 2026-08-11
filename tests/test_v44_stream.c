// SPDX-License-Identifier: BSD-3-Clause

#include <v44_stream.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

static bool test_parameter_defaults_and_bounds(void)
{
	const struct sippymodem_v44_parameters defaults =
		sippymodem_v44_default_parameters();
	const struct sippymodem_v44_parameters minimum = {
		SIPPYMODEM_V44_MIN_CODEWORDS,
		SIPPYMODEM_V44_MIN_STRING_LENGTH,
		SIPPYMODEM_V44_MIN_HISTORY_SIZE,
	};
	const struct sippymodem_v44_parameters maximum = {
		SIPPYMODEM_V44_MAX_CODEWORDS,
		SIPPYMODEM_V44_MAX_STRING_LENGTH,
		SIPPYMODEM_V44_MAX_HISTORY_SIZE,
	};
	const struct sippymodem_v44_parameters invalid[] = {
		{ SIPPYMODEM_V44_MIN_CODEWORDS - 1,
		  SIPPYMODEM_V44_MIN_STRING_LENGTH,
		  SIPPYMODEM_V44_MIN_HISTORY_SIZE },
		{ SIPPYMODEM_V44_MAX_CODEWORDS + 1U,
		  SIPPYMODEM_V44_MIN_STRING_LENGTH,
		  SIPPYMODEM_V44_MIN_HISTORY_SIZE },
		{ SIPPYMODEM_V44_MIN_CODEWORDS,
		  SIPPYMODEM_V44_MIN_STRING_LENGTH - 1,
		  SIPPYMODEM_V44_MIN_HISTORY_SIZE },
		{ SIPPYMODEM_V44_MIN_CODEWORDS,
		  SIPPYMODEM_V44_MAX_STRING_LENGTH + 1U,
		  SIPPYMODEM_V44_MIN_HISTORY_SIZE },
		{ SIPPYMODEM_V44_MIN_CODEWORDS,
		  SIPPYMODEM_V44_MIN_STRING_LENGTH,
		  SIPPYMODEM_V44_MIN_HISTORY_SIZE - 1 },
		{ SIPPYMODEM_V44_MIN_CODEWORDS,
		  SIPPYMODEM_V44_MIN_STRING_LENGTH,
		  SIPPYMODEM_V44_MAX_HISTORY_SIZE + 1U },
	};

	CHECK(defaults.codeword_limit == 1024 &&
		      defaults.max_string_length == 255 &&
		      defaults.history_size == 3072 &&
		      sippymodem_v44_parameters_are_valid(&defaults),
	      "V.44 stream defaults are wrong");
	CHECK(sippymodem_v44_parameters_are_valid(&minimum) &&
		      sippymodem_v44_parameters_are_valid(&maximum),
	      "V.44 boundary parameters were rejected");
	CHECK(!sippymodem_v44_parameters_are_valid(NULL),
	      "null V.44 parameters were accepted");
	for (size_t index = 0; index < ARRAY_LENGTH(invalid); ++index)
		CHECK(!sippymodem_v44_parameters_are_valid(&invalid[index]),
		      "invalid V.44 parameter set %zu was accepted", index);
	CHECK(sippymodem_v44_bit_writer_create(0) == NULL &&
		      sippymodem_v44_bit_writer_create(1) == NULL,
	      "undersized V.44 output queue was accepted");
	CHECK(strcmp(sippymodem_v44_stream_result_name(
			     SIPPYMODEM_V44_STREAM_NEED_INPUT),
		     "need input") == 0 &&
		      strcmp(sippymodem_v44_stream_result_name(
			     (enum sippymodem_v44_stream_result)99),
		     "unknown") == 0,
	      "V.44 stream result names changed");
	return true;
}

static bool test_lsb_first_packing_and_alignment(void)
{
	static const uint8_t expected[] = { 0x15, 0xbc, 0x0a };
	struct sippymodem_v44_bit_writer *writer =
		sippymodem_v44_bit_writer_create(sizeof(expected));
	uint8_t actual[sizeof(expected)] = { 0 };
	bool passed = false;

	CHECK(writer != NULL, "could not create V.44 bit writer");
	CHECK(sippymodem_v44_bit_writer_at_octet_boundary(writer) &&
	      !sippymodem_v44_bit_writer_at_octet_boundary(NULL),
	      "fresh V.44 writer boundary state was wrong");
	if (sippymodem_v44_bit_writer_put(writer, 5, 3) !=
		    SIPPYMODEM_V44_STREAM_OK ||
	    sippymodem_v44_bit_writer_at_octet_boundary(writer) ||
	    sippymodem_v44_bit_writer_put(writer, 2, 2) !=
		    SIPPYMODEM_V44_STREAM_OK ||
	    sippymodem_v44_bit_writer_align(writer) !=
		    SIPPYMODEM_V44_STREAM_OK ||
	    !sippymodem_v44_bit_writer_at_octet_boundary(writer) ||
	    sippymodem_v44_bit_writer_put(writer, 0xabc, 12) !=
		    SIPPYMODEM_V44_STREAM_OK ||
	    sippymodem_v44_bit_writer_at_octet_boundary(writer) ||
	    sippymodem_v44_bit_writer_align(writer) !=
		    SIPPYMODEM_V44_STREAM_OK ||
	    !sippymodem_v44_bit_writer_at_octet_boundary(writer))
		goto out;
	if (sippymodem_v44_bit_writer_pending(writer) != sizeof(expected) ||
	    sippymodem_v44_bit_writer_read(writer, actual, sizeof(actual)) !=
		    sizeof(actual) ||
	    memcmp(actual, expected, sizeof(expected)) != 0)
		goto out;
	passed = true;

out:
	sippymodem_v44_bit_writer_destroy(writer);
	CHECK(passed, "V.44 fields were not packed least-significant bit first");
	return true;
}

static bool test_bounded_queue_and_empty_read(void)
{
	static const uint8_t wrapped_tail[] = { 0x77, 0x5f };
	struct sippymodem_v44_bit_writer *writer =
		sippymodem_v44_bit_writer_create(2);
	uint8_t tail[sizeof(wrapped_tail)] = { 0 };
	uint8_t byte = 0xa5;

	CHECK(writer != NULL, "could not create bounded V.44 bit writer");
	CHECK(sippymodem_v44_bit_writer_read(writer, &byte, 1) == 0 &&
		      byte == 0xa5 &&
		      sippymodem_v44_bit_writer_read(NULL, &byte, 1) == 0 &&
		      sippymodem_v44_bit_writer_read(writer, NULL, 1) == 0 &&
		      sippymodem_v44_bit_writer_read(writer, &byte, 0) == 0,
	      "empty V.44 queue read changed its destination");
	CHECK(sippymodem_v44_bit_writer_put(writer, 0x2211, 16) ==
		      SIPPYMODEM_V44_STREAM_OK &&
		      sippymodem_v44_bit_writer_put(writer, 0x33, 8) ==
		      SIPPYMODEM_V44_STREAM_NEED_OUTPUT,
	      "bounded V.44 queue did not apply backpressure");
	CHECK(sippymodem_v44_bit_writer_read(writer, &byte, 1) == 1 &&
		      byte == 0x11 &&
		      sippymodem_v44_bit_writer_put(writer, 0x33, 8) ==
		      SIPPYMODEM_V44_STREAM_OK,
	      "V.44 writer did not resume after output was drained");
	CHECK(sippymodem_v44_bit_writer_read(writer, &byte, 1) == 1 &&
		      byte == 0x22 &&
		      sippymodem_v44_bit_writer_read(writer, &byte, 1) == 1 &&
		      byte == 0x33 &&
		      sippymodem_v44_bit_writer_pending(writer) == 0,
	      "V.44 queue wrap changed output ordering");
	CHECK(sippymodem_v44_bit_writer_put(NULL, 0, 1) ==
		      SIPPYMODEM_V44_STREAM_INVALID_ARGUMENT &&
		      sippymodem_v44_bit_writer_put(writer, 0, 17) ==
		      SIPPYMODEM_V44_STREAM_INVALID_ARGUMENT &&
		      sippymodem_v44_bit_writer_put(writer, 0x7f, 7) ==
		      SIPPYMODEM_V44_STREAM_OK &&
		      sippymodem_v44_bit_writer_put(writer, 0xbeef, 16) ==
		      SIPPYMODEM_V44_STREAM_OK &&
		      sippymodem_v44_bit_writer_align(writer) ==
		      SIPPYMODEM_V44_STREAM_NEED_OUTPUT &&
		      sippymodem_v44_bit_writer_read(writer, &byte, 1) == 1 &&
		      byte == 0xff &&
		      sippymodem_v44_bit_writer_align(writer) ==
		      SIPPYMODEM_V44_STREAM_OK &&
		      sippymodem_v44_bit_writer_read(
			      writer, tail, sizeof(tail)) == sizeof(tail) &&
		      memcmp(tail, wrapped_tail, sizeof(tail)) == 0,
	      "V.44 backpressure changed a partially packed field");
	sippymodem_v44_bit_writer_destroy(writer);
	return true;
}

static bool test_chunked_reader_preserves_partial_field(void)
{
	static const uint8_t first[] = { 0xad };
	static const uint8_t rest[] = { 0xd2, 0x01, 0x7e };
	struct sippymodem_v44_bit_reader *reader =
		sippymodem_v44_bit_reader_create();
	const uint8_t *input = first;
	size_t input_length = sizeof(first);
	uint32_t value = 0xdeadbeef;

	CHECK(reader != NULL, "could not create V.44 bit reader");
	CHECK(sippymodem_v44_bit_reader_at_boundary(reader) &&
	      !sippymodem_v44_bit_reader_at_boundary(NULL),
	      "fresh V.44 reader boundary state was wrong");
	input = NULL;
	input_length = 0;
	CHECK(sippymodem_v44_bit_reader_get(
		      reader, &input, &input_length, 12, &value) ==
		      SIPPYMODEM_V44_STREAM_NEED_INPUT &&
		      input == NULL && input_length == 0 && value == 0xdeadbeef,
	      "empty V.44 input changed the reader");
	CHECK(sippymodem_v44_bit_reader_at_boundary(reader),
	      "fresh empty V.44 input started a partial field");
	input = first;
	input_length = sizeof(first);
	CHECK(sippymodem_v44_bit_reader_get(
		      reader, &input, &input_length, 12, &value) ==
		      SIPPYMODEM_V44_STREAM_NEED_INPUT &&
		      input_length == 0 && value == 0xdeadbeef,
	      "partial V.44 field was not preserved");
	CHECK(!sippymodem_v44_bit_reader_at_boundary(reader),
	      "partial V.44 field reported a clean boundary");
	CHECK(sippymodem_v44_bit_reader_get(
		      reader, &input, &input_length, 7, &value) ==
		      SIPPYMODEM_V44_STREAM_INVALID_ARGUMENT &&
		      value == 0xdeadbeef &&
		      sippymodem_v44_bit_reader_align(reader) ==
		      SIPPYMODEM_V44_STREAM_INVALID_ARGUMENT,
	      "partial V.44 field could be reinterpreted");

	input = rest;
	input_length = sizeof(rest);
	CHECK(sippymodem_v44_bit_reader_get(
		      reader, &input, &input_length, 12, &value) ==
		      SIPPYMODEM_V44_STREAM_OK &&
		      value == 0x2ad && input_length == 2,
	      "chunked V.44 field resumed with the wrong value");
	CHECK(sippymodem_v44_bit_reader_get(
		      reader, &input, &input_length, 7, &value) ==
		      SIPPYMODEM_V44_STREAM_OK &&
		      value == 0x1d && input_length == 1 &&
		      sippymodem_v44_bit_reader_align(reader) ==
		      SIPPYMODEM_V44_STREAM_OK,
	      "V.44 reader alignment lost buffered input");
	CHECK(sippymodem_v44_bit_reader_get(
		      reader, &input, &input_length, 8, &value) ==
		      SIPPYMODEM_V44_STREAM_OK &&
		      value == 0x7e && input_length == 0,
	      "V.44 reader did not continue at the byte boundary");
	CHECK(sippymodem_v44_bit_reader_at_boundary(reader),
	      "drained V.44 reader did not report a clean boundary");
	sippymodem_v44_bit_reader_destroy(reader);
	return true;
}

static bool test_reader_rejects_nonzero_padding(void)
{
	static const uint8_t encoded[] = { 0xe1 };
	struct sippymodem_v44_bit_reader *reader =
		sippymodem_v44_bit_reader_create();
	const uint8_t *input = encoded;
	size_t input_length = sizeof(encoded);
	uint32_t value = 0;

	CHECK(reader != NULL, "could not create V.44 padding reader");
	CHECK(sippymodem_v44_bit_reader_get(
		      reader, &input, &input_length, 5, &value) ==
		      SIPPYMODEM_V44_STREAM_OK &&
		      value == 1 && input_length == 0 &&
		      sippymodem_v44_bit_reader_align(reader) ==
		      SIPPYMODEM_V44_STREAM_INVALID_ARGUMENT &&
		      !sippymodem_v44_bit_reader_at_boundary(reader),
	      "V.44 reader accepted nonzero alignment padding");
	sippymodem_v44_bit_reader_destroy(reader);
	return true;
}

static bool drain_writer(
	struct sippymodem_v44_bit_writer *writer,
	uint8_t *output,
	size_t output_capacity,
	size_t *output_length,
	size_t maximum_chunk)
{
	size_t available = output_capacity - *output_length;
	size_t chunk = available < maximum_chunk ? available : maximum_chunk;
	size_t count;

	if (chunk == 0)
		return false;
	count = sippymodem_v44_bit_writer_read(
		writer, output + *output_length, chunk);
	*output_length += count;
	return count != 0;
}

static bool test_generated_chunked_round_trip(void)
{
	enum { FIELD_COUNT = 257, WIRE_CAPACITY = 1024 };
	struct sippymodem_v44_bit_writer *writer =
		sippymodem_v44_bit_writer_create(5);
	struct sippymodem_v44_bit_reader *reader = NULL;
	uint8_t wire[WIRE_CAPACITY];
	uint16_t values[FIELD_COUNT];
	uint8_t widths[FIELD_COUNT];
	uint32_t random = 0x13579bdf;
	size_t wire_length = 0;
	size_t source_offset = 0;
	const uint8_t *input = NULL;
	size_t input_length = 0;
	bool passed = false;

	CHECK(writer != NULL, "could not create generated-test V.44 writer");
	for (size_t index = 0; index < FIELD_COUNT; ++index) {
		enum sippymodem_v44_stream_result result;
		uint32_t mask;

		random = random * UINT32_C(1664525) + UINT32_C(1013904223);
		widths[index] = (uint8_t)((index * 5U) % 16U + 1U);
		mask = (UINT32_C(1) << widths[index]) - UINT32_C(1);
		values[index] = (uint16_t)(random & mask);
		do {
			result = sippymodem_v44_bit_writer_put(
				writer, values[index], widths[index]);
			if (result == SIPPYMODEM_V44_STREAM_NEED_OUTPUT &&
			    !drain_writer(writer, wire, sizeof(wire),
				    &wire_length, index % 3U + 1U))
				goto out;
		} while (result == SIPPYMODEM_V44_STREAM_NEED_OUTPUT);
		if (result != SIPPYMODEM_V44_STREAM_OK)
			goto out;
	}
	while (sippymodem_v44_bit_writer_align(writer) ==
	       SIPPYMODEM_V44_STREAM_NEED_OUTPUT) {
		if (!drain_writer(
			    writer, wire, sizeof(wire), &wire_length, 2))
			goto out;
	}
	while (sippymodem_v44_bit_writer_pending(writer) != 0)
		if (!drain_writer(
			    writer, wire, sizeof(wire), &wire_length, 3))
			goto out;

	reader = sippymodem_v44_bit_reader_create();
	if (reader == NULL)
		goto out;
	for (size_t index = 0; index < FIELD_COUNT; ++index) {
		enum sippymodem_v44_stream_result result;
		uint32_t value = UINT32_MAX;

		do {
			if (input_length == 0 && source_offset < wire_length) {
				input_length = (index % 3U) + 1U;
				if (input_length > wire_length - source_offset)
					input_length = wire_length - source_offset;
				input = wire + source_offset;
				source_offset += input_length;
			}
			result = sippymodem_v44_bit_reader_get(
				reader, &input, &input_length, widths[index],
				&value);
		} while (result == SIPPYMODEM_V44_STREAM_NEED_INPUT &&
			 source_offset < wire_length);
		if (result != SIPPYMODEM_V44_STREAM_OK ||
		    value != values[index])
			goto out;
	}
	if (sippymodem_v44_bit_reader_align(reader) !=
		    SIPPYMODEM_V44_STREAM_OK ||
	    input_length != 0 || source_offset != wire_length)
		goto out;
	passed = true;

out:
	sippymodem_v44_bit_reader_destroy(reader);
	sippymodem_v44_bit_writer_destroy(writer);
	CHECK(passed, "generated chunked V.44 bitstream did not round-trip");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "parameter defaults and bounds",
		  test_parameter_defaults_and_bounds },
		{ "LSB-first packing and alignment",
		  test_lsb_first_packing_and_alignment },
		{ "bounded queue and empty read",
		  test_bounded_queue_and_empty_read },
		{ "chunked partial field",
		  test_chunked_reader_preserves_partial_field },
		{ "zero padding validation",
		  test_reader_rejects_nonzero_padding },
		{ "generated chunked round trip",
		  test_generated_chunked_round_trip },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		printf("[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			printf("[       OK ] %s\n", tests[index].name);
		else {
			printf("[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0 ? 0 : 1;
}
