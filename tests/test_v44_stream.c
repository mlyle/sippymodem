// SPDX-License-Identifier: BSD-3-Clause

#include <v44_stream.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

static bool test_parameter_defaults_and_bounds(void)
{
	const struct v44_parameters defaults =
		v44_default_parameters();
	const struct v44_parameters minimum = {
		V44_MIN_CODEWORDS,
		V44_MIN_STRING_LENGTH,
		V44_MIN_HISTORY_SIZE,
	};
	const struct v44_parameters maximum = {
		V44_MAX_CODEWORDS,
		V44_MAX_STRING_LENGTH,
		V44_MAX_HISTORY_SIZE,
	};
	const struct v44_parameters invalid[] = {
		{ V44_MIN_CODEWORDS - 1,
		  V44_MIN_STRING_LENGTH,
		  V44_MIN_HISTORY_SIZE },
		{ V44_MAX_CODEWORDS + 1U,
		  V44_MIN_STRING_LENGTH,
		  V44_MIN_HISTORY_SIZE },
		{ V44_MIN_CODEWORDS,
		  V44_MIN_STRING_LENGTH - 1,
		  V44_MIN_HISTORY_SIZE },
		{ V44_MIN_CODEWORDS,
		  V44_MAX_STRING_LENGTH + 1U,
		  V44_MIN_HISTORY_SIZE },
		{ V44_MIN_CODEWORDS,
		  V44_MIN_STRING_LENGTH,
		  V44_MIN_HISTORY_SIZE - 1 },
		{ V44_MIN_CODEWORDS,
		  V44_MIN_STRING_LENGTH,
		  V44_MAX_HISTORY_SIZE + 1U },
	};

	CHECK(defaults.codeword_limit == 1024 &&
		      defaults.max_string_length == 255 &&
		      defaults.history_size == 3072 &&
		      v44_parameters_are_valid(&defaults),
	      "V.44 stream defaults are wrong");
	CHECK(v44_parameters_are_valid(&minimum) &&
		      v44_parameters_are_valid(&maximum),
	      "V.44 boundary parameters were rejected");
	CHECK(!v44_parameters_are_valid(NULL),
	      "null V.44 parameters were accepted");
	for (size_t index = 0; index < ARRAY_LENGTH(invalid); ++index)
		CHECK(!v44_parameters_are_valid(&invalid[index]),
		      "invalid V.44 parameter set %zu was accepted", index);
	CHECK(v44_bit_writer_create(0) == NULL &&
		      v44_bit_writer_create(1) == NULL,
	      "undersized V.44 output queue was accepted");
	CHECK(strcmp(v44_stream_result_name(
			     V44_STREAM_NEED_INPUT),
		     "need input") == 0 &&
		      strcmp(v44_stream_result_name(
			     (enum v44_stream_result)99),
		     "unknown") == 0,
	      "V.44 stream result names changed");
	return true;
}

static bool test_lsb_first_packing_and_alignment(void)
{
	static const uint8_t expected[] = { 0x15, 0xbc, 0x0a };
	struct v44_bit_writer *writer =
		v44_bit_writer_create(sizeof(expected));
	uint8_t actual[sizeof(expected)] = { 0 };
	bool passed = false;

	CHECK(writer != NULL, "could not create V.44 bit writer");
	CHECK(v44_bit_writer_at_octet_boundary(writer) &&
	      !v44_bit_writer_at_octet_boundary(NULL),
	      "fresh V.44 writer boundary state was wrong");
	if (v44_bit_writer_put(writer, 5, 3) !=
		    V44_STREAM_OK ||
	    v44_bit_writer_at_octet_boundary(writer) ||
	    v44_bit_writer_put(writer, 2, 2) !=
		    V44_STREAM_OK ||
	    v44_bit_writer_align(writer) !=
		    V44_STREAM_OK ||
	    !v44_bit_writer_at_octet_boundary(writer) ||
	    v44_bit_writer_put(writer, 0xabc, 12) !=
		    V44_STREAM_OK ||
	    v44_bit_writer_at_octet_boundary(writer) ||
	    v44_bit_writer_align(writer) !=
		    V44_STREAM_OK ||
	    !v44_bit_writer_at_octet_boundary(writer))
		goto out;
	if (v44_bit_writer_pending(writer) != sizeof(expected) ||
	    v44_bit_writer_read(writer, actual, sizeof(actual)) !=
		    sizeof(actual) ||
	    memcmp(actual, expected, sizeof(expected)) != 0)
		goto out;
	passed = true;

out:
	v44_bit_writer_destroy(writer);
	CHECK(passed, "V.44 fields were not packed least-significant bit first");
	return true;
}

static bool test_bounded_queue_and_empty_read(void)
{
	static const uint8_t wrapped_tail[] = { 0x77, 0x5f };
	struct v44_bit_writer *writer =
		v44_bit_writer_create(2);
	uint8_t tail[sizeof(wrapped_tail)] = { 0 };
	uint8_t byte = 0xa5;

	CHECK(writer != NULL, "could not create bounded V.44 bit writer");
	CHECK(v44_bit_writer_read(writer, &byte, 1) == 0 &&
		      byte == 0xa5 &&
		      v44_bit_writer_read(NULL, &byte, 1) == 0 &&
		      v44_bit_writer_read(writer, NULL, 1) == 0 &&
		      v44_bit_writer_read(writer, &byte, 0) == 0,
	      "empty V.44 queue read changed its destination");
	CHECK(v44_bit_writer_put(writer, 0x2211, 16) ==
		      V44_STREAM_OK &&
		      v44_bit_writer_put(writer, 0x33, 8) ==
		      V44_STREAM_NEED_OUTPUT,
	      "bounded V.44 queue did not apply backpressure");
	CHECK(v44_bit_writer_read(writer, &byte, 1) == 1 &&
		      byte == 0x11 &&
		      v44_bit_writer_put(writer, 0x33, 8) ==
		      V44_STREAM_OK,
	      "V.44 writer did not resume after output was drained");
	CHECK(v44_bit_writer_read(writer, &byte, 1) == 1 &&
		      byte == 0x22 &&
		      v44_bit_writer_read(writer, &byte, 1) == 1 &&
		      byte == 0x33 &&
		      v44_bit_writer_pending(writer) == 0,
	      "V.44 queue wrap changed output ordering");
	CHECK(v44_bit_writer_put(NULL, 0, 1) ==
		      V44_STREAM_INVALID_ARGUMENT &&
		      v44_bit_writer_put(writer, 0, 17) ==
		      V44_STREAM_INVALID_ARGUMENT &&
		      v44_bit_writer_put(writer, 0x7f, 7) ==
		      V44_STREAM_OK &&
		      v44_bit_writer_put(writer, 0xbeef, 16) ==
		      V44_STREAM_OK &&
		      v44_bit_writer_align(writer) ==
		      V44_STREAM_NEED_OUTPUT &&
		      v44_bit_writer_read(writer, &byte, 1) == 1 &&
		      byte == 0xff &&
		      v44_bit_writer_align(writer) ==
		      V44_STREAM_OK &&
		      v44_bit_writer_read(
			      writer, tail, sizeof(tail)) == sizeof(tail) &&
		      memcmp(tail, wrapped_tail, sizeof(tail)) == 0,
	      "V.44 backpressure changed a partially packed field");
	v44_bit_writer_destroy(writer);
	return true;
}

static bool test_chunked_reader_preserves_partial_field(void)
{
	static const uint8_t first[] = { 0xad };
	static const uint8_t rest[] = { 0xd2, 0x01, 0x7e };
	struct v44_bit_reader *reader =
		v44_bit_reader_create();
	const uint8_t *input = first;
	size_t input_length = sizeof(first);
	uint32_t value = 0xdeadbeef;

	CHECK(reader != NULL, "could not create V.44 bit reader");
	CHECK(v44_bit_reader_at_boundary(reader) &&
	      !v44_bit_reader_at_boundary(NULL),
	      "fresh V.44 reader boundary state was wrong");
	input = NULL;
	input_length = 0;
	CHECK(v44_bit_reader_get(
		      reader, &input, &input_length, 12, &value) ==
		      V44_STREAM_NEED_INPUT &&
		      input == NULL && input_length == 0 && value == 0xdeadbeef,
	      "empty V.44 input changed the reader");
	CHECK(v44_bit_reader_at_boundary(reader),
	      "fresh empty V.44 input started a partial field");
	input = first;
	input_length = sizeof(first);
	CHECK(v44_bit_reader_get(
		      reader, &input, &input_length, 12, &value) ==
		      V44_STREAM_NEED_INPUT &&
		      input_length == 0 && value == 0xdeadbeef,
	      "partial V.44 field was not preserved");
	CHECK(!v44_bit_reader_at_boundary(reader),
	      "partial V.44 field reported a clean boundary");
	CHECK(v44_bit_reader_get(
		      reader, &input, &input_length, 7, &value) ==
		      V44_STREAM_INVALID_ARGUMENT &&
		      value == 0xdeadbeef &&
		      v44_bit_reader_align(reader) ==
		      V44_STREAM_INVALID_ARGUMENT,
	      "partial V.44 field could be reinterpreted");

	input = rest;
	input_length = sizeof(rest);
	CHECK(v44_bit_reader_get(
		      reader, &input, &input_length, 12, &value) ==
		      V44_STREAM_OK &&
		      value == 0x2ad && input_length == 2,
	      "chunked V.44 field resumed with the wrong value");
	CHECK(v44_bit_reader_get(
		      reader, &input, &input_length, 7, &value) ==
		      V44_STREAM_OK &&
		      value == 0x1d && input_length == 1 &&
		      v44_bit_reader_align(reader) ==
		      V44_STREAM_OK,
	      "V.44 reader alignment lost buffered input");
	CHECK(v44_bit_reader_get(
		      reader, &input, &input_length, 8, &value) ==
		      V44_STREAM_OK &&
		      value == 0x7e && input_length == 0,
	      "V.44 reader did not continue at the byte boundary");
	CHECK(v44_bit_reader_at_boundary(reader),
	      "drained V.44 reader did not report a clean boundary");
	v44_bit_reader_destroy(reader);
	return true;
}

static bool test_reader_rejects_nonzero_padding(void)
{
	static const uint8_t encoded[] = { 0xe1 };
	struct v44_bit_reader *reader =
		v44_bit_reader_create();
	const uint8_t *input = encoded;
	size_t input_length = sizeof(encoded);
	uint32_t value = 0;

	CHECK(reader != NULL, "could not create V.44 padding reader");
	CHECK(v44_bit_reader_get(
		      reader, &input, &input_length, 5, &value) ==
		      V44_STREAM_OK &&
		      value == 1 && input_length == 0 &&
		      v44_bit_reader_align(reader) ==
		      V44_STREAM_INVALID_ARGUMENT &&
		      !v44_bit_reader_at_boundary(reader),
	      "V.44 reader accepted nonzero alignment padding");
	v44_bit_reader_destroy(reader);
	return true;
}

static bool drain_writer(
	struct v44_bit_writer *writer,
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
	count = v44_bit_writer_read(
		writer, output + *output_length, chunk);
	*output_length += count;
	return count != 0;
}

static bool test_generated_chunked_round_trip(void)
{
	enum { FIELD_COUNT = 257, WIRE_CAPACITY = 1024 };
	struct v44_bit_writer *writer =
		v44_bit_writer_create(5);
	struct v44_bit_reader *reader = NULL;
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
		enum v44_stream_result result;
		uint32_t mask;

		random = random * UINT32_C(1664525) + UINT32_C(1013904223);
		widths[index] = (uint8_t)((index * 5U) % 16U + 1U);
		mask = (UINT32_C(1) << widths[index]) - UINT32_C(1);
		values[index] = (uint16_t)(random & mask);
		do {
			result = v44_bit_writer_put(
				writer, values[index], widths[index]);
			if (result == V44_STREAM_NEED_OUTPUT &&
			    !drain_writer(writer, wire, sizeof(wire),
				    &wire_length, index % 3U + 1U))
				goto out;
		} while (result == V44_STREAM_NEED_OUTPUT);
		if (result != V44_STREAM_OK)
			goto out;
	}
	while (v44_bit_writer_align(writer) ==
	       V44_STREAM_NEED_OUTPUT) {
		if (!drain_writer(
			    writer, wire, sizeof(wire), &wire_length, 2))
			goto out;
	}
	while (v44_bit_writer_pending(writer) != 0)
		if (!drain_writer(
			    writer, wire, sizeof(wire), &wire_length, 3))
			goto out;

	reader = v44_bit_reader_create();
	if (reader == NULL)
		goto out;
	for (size_t index = 0; index < FIELD_COUNT; ++index) {
		enum v44_stream_result result;
		uint32_t value = UINT32_MAX;

		do {
			if (input_length == 0 && source_offset < wire_length) {
				input_length = (index % 3U) + 1U;
				if (input_length > wire_length - source_offset)
					input_length = wire_length - source_offset;
				input = wire + source_offset;
				source_offset += input_length;
			}
			result = v44_bit_reader_get(
				reader, &input, &input_length, widths[index],
				&value);
		} while (result == V44_STREAM_NEED_INPUT &&
			 source_offset < wire_length);
		if (result != V44_STREAM_OK ||
		    value != values[index])
			goto out;
	}
	if (v44_bit_reader_align(reader) !=
		    V44_STREAM_OK ||
	    input_length != 0 || source_offset != wire_length)
		goto out;
	passed = true;

out:
	v44_bit_reader_destroy(reader);
	v44_bit_writer_destroy(writer);
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
