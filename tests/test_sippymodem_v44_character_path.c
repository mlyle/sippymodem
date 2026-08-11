// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v44_character_path.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

enum {
	PAYLOAD_LENGTH = 768,
	WIRE_CAPACITY = 4096,
};

static const struct sippymodem_v44_parameters parameters = {
	.codeword_limit = SIPPYMODEM_V44_MIN_CODEWORDS,
	.max_string_length = SIPPYMODEM_V44_MIN_STRING_LENGTH,
	.history_size = SIPPYMODEM_V44_MIN_HISTORY_SIZE,
};

struct character_io {
	const uint8_t *source;
	size_t source_length;
	size_t source_offset;
	size_t data_reads;
	size_t empty_reads;
	uint8_t *sink;
	size_t sink_capacity;
	size_t sink_length;
	unsigned int write_calls;
	unsigned int block_period;
	int read_fault;
	int write_fault;
};

static int read_character(
	void *context, uint8_t *output, size_t output_capacity)
{
	struct character_io *io = context;

	if (io->read_fault != 0)
		return io->read_fault;
	if (output_capacity != 1)
		return -1;
	if (io->source_offset == io->source_length) {
		io->empty_reads++;
		return 0;
	}
	output[0] = io->source[io->source_offset++];
	io->data_reads++;
	return 1;
}

static int write_character(
	void *context, const uint8_t *input, size_t input_length)
{
	struct character_io *io = context;
	size_t length = input_length < 3 ? input_length : 3;

	io->write_calls++;
	if (io->write_fault != 0)
		return io->write_fault > 0 ? (int)input_length + 1 : -1;
	if (io->block_period != 0 &&
	    io->write_calls % io->block_period == 1)
		return 0;
	if (length > io->sink_capacity - io->sink_length)
		return -1;
	memcpy(io->sink + io->sink_length, input, length);
	io->sink_length += length;
	return (int)length;
}

static struct sippymodem_v44_character_path *create_path(
	struct character_io *io, bool transmit_enabled, bool receive_enabled,
	size_t receive_capacity)
{
	struct sippymodem_v44_character_path_config config = {
		.codec = {
			.transmit_enabled = transmit_enabled,
			.transmit_parameters = parameters,
			.receive_enabled = receive_enabled,
			.receive_parameters = parameters,
		},
		.receive_buffer_capacity = receive_capacity,
		.context = io,
		.read = read_character,
		.write = write_character,
	};

	return sippymodem_v44_character_path_create(&config);
}

static void generate_payload(uint8_t *payload, size_t length)
{
	static const uint8_t phrase[] = "bounded character path ";
	uint32_t state = UINT32_C(0x8da6b343);

	for (size_t index = 0; index < length; ++index) {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		payload[index] = index % 97 < 73 ?
				 phrase[index % (sizeof(phrase) - 1)] :
				 (uint8_t)state;
	}
}

static bool transmit_until_idle(
	struct sippymodem_v44_character_path *path,
	uint8_t *wire,
	size_t wire_capacity,
	size_t *wire_length)
{
	for (size_t iteration = 0; iteration < 100000; ++iteration) {
		size_t output_length = 99;
		enum sippymodem_v44_character_path_result result;

		if (*wire_length == wire_capacity)
			return false;
		result = sippymodem_v44_character_path_transmit(
			path, wire + *wire_length, 1, &output_length);
		if (output_length > 1)
			return false;
		*wire_length += output_length;
		if (result == SIPPYMODEM_V44_CHARACTER_PATH_IDLE)
			return true;
		if (result != SIPPYMODEM_V44_CHARACTER_PATH_OK)
			return false;
	}
	return false;
}

static bool direct_transmit(
	const uint8_t *plain,
	size_t plain_length,
	uint8_t *wire,
	size_t wire_capacity,
	size_t *wire_length)
{
	struct sippymodem_v44_duplex_config config = {
		.transmit_enabled = true,
		.transmit_parameters = parameters,
	};
	struct sippymodem_v44_duplex *duplex =
		sippymodem_v44_duplex_create(&config);
	const uint8_t *input = plain;
	size_t input_length = plain_length;

	if (duplex == NULL)
		return false;
	for (size_t iteration = 0;
	     input_length != 0 && iteration < 100000;
	     ++iteration) {
		size_t output_length = 0;
		enum sippymodem_v44_duplex_result result;

		if (*wire_length == wire_capacity)
			goto fail;
		result = sippymodem_v44_duplex_transmit(
			duplex, &input, &input_length, wire + *wire_length, 1,
			&output_length);
		if (output_length > 1 ||
		    (result != SIPPYMODEM_V44_DUPLEX_NEED_INPUT &&
		     result != SIPPYMODEM_V44_DUPLEX_NEED_OUTPUT))
			goto fail;
		*wire_length += output_length;
	}
	for (size_t iteration = 0; iteration < 100000; ++iteration) {
		size_t output_length = 0;
		enum sippymodem_v44_duplex_result result;

		if (*wire_length == wire_capacity)
			goto fail;
		result = sippymodem_v44_duplex_flush_transmit(
			duplex, wire + *wire_length, 1, &output_length);
		if (output_length > 1)
			goto fail;
		*wire_length += output_length;
		if (result == SIPPYMODEM_V44_DUPLEX_FLUSHED) {
			sippymodem_v44_duplex_destroy(duplex);
			return true;
		}
		if (result != SIPPYMODEM_V44_DUPLEX_NEED_OUTPUT)
			goto fail;
	}
fail:
	sippymodem_v44_duplex_destroy(duplex);
	return false;
}

static bool receive_wire(
	struct sippymodem_v44_character_path *path,
	const uint8_t *wire,
	size_t wire_length,
	const struct character_io *io,
	size_t expected_length)
{
	size_t wire_offset = 0;

	for (size_t iteration = 0;
	     wire_offset != wire_length && iteration < 100000;
	     ++iteration) {
		size_t available = wire_length - wire_offset;
		size_t chunk = wire_offset % 7 + 1;
		size_t consumed = 99;
		enum sippymodem_v44_character_path_result result;

		if (chunk > available)
			chunk = available;
		result = sippymodem_v44_character_path_receive(
			path, wire + wire_offset, chunk, &consumed);
		if ((result != SIPPYMODEM_V44_CHARACTER_PATH_OK &&
		     result != SIPPYMODEM_V44_CHARACTER_PATH_NEED_OUTPUT) ||
		    consumed > chunk)
			return false;
		wire_offset += consumed;
	}
	for (size_t iteration = 0; iteration < 100000; ++iteration) {
		size_t consumed = 99;
		enum sippymodem_v44_character_path_result result;

		if (wire_offset != wire_length)
			return false;
		if (io->sink_length == expected_length)
			return true;
		result = sippymodem_v44_character_path_receive(
			path, NULL, 0, &consumed);
		if ((result != SIPPYMODEM_V44_CHARACTER_PATH_OK &&
		     result != SIPPYMODEM_V44_CHARACTER_PATH_NEED_OUTPUT) ||
		    consumed != 0)
			return false;
	}
	return false;
}

static bool test_generated_round_trip(void)
{
	uint8_t payload[PAYLOAD_LENGTH];
	uint8_t decoded[PAYLOAD_LENGTH];
	uint8_t wire[WIRE_CAPACITY];
	struct character_io sender_io = { 0 };
	struct character_io receiver_io = {
		.sink = decoded,
		.sink_capacity = sizeof(decoded),
		.block_period = 3,
	};
	struct sippymodem_v44_character_path *sender =
		create_path(&sender_io, true, false, 5);
	struct sippymodem_v44_character_path *receiver =
		create_path(&receiver_io, false, true, 5);
	size_t wire_length = 0;
	size_t output_length = 99;

	generate_payload(payload, sizeof(payload));
	CHECK(sender != NULL && receiver != NULL,
	      "could not create V.44 character paths");
	sender_io.source = payload;
	sender_io.source_length = sizeof(payload) / 2;
	CHECK(sippymodem_v44_character_path_transmit(
		      sender, NULL, 0, &output_length) ==
		      SIPPYMODEM_V44_CHARACTER_PATH_OK &&
	      output_length == 0 && sender_io.source_offset != 0,
	      "zero-capacity transmit did not retain bounded codec work");
	CHECK(transmit_until_idle(
		      sender, wire, sizeof(wire), &wire_length),
	      "first V.44 character burst did not flush");
	sender_io.source = payload + sizeof(payload) / 2;
	sender_io.source_length = sizeof(payload) - sizeof(payload) / 2;
	sender_io.source_offset = 0;
	CHECK(transmit_until_idle(
		      sender, wire, sizeof(wire), &wire_length),
	      "second V.44 character burst did not flush");
	CHECK(sender_io.data_reads == sizeof(payload) &&
	      sender_io.empty_reads == 4,
	      "V.44 character transmit replayed source bytes");
	CHECK(receive_wire(
		      receiver, wire, wire_length, &receiver_io,
		      sizeof(payload)) &&
	      receiver_io.sink_length == sizeof(payload) &&
	      memcmp(decoded, payload, sizeof(payload)) == 0,
	      "bounded V.44 character backpressure did not round trip");
	CHECK(sippymodem_v44_character_path_finish_receive(receiver) ==
		      SIPPYMODEM_V44_CHARACTER_PATH_OK &&
	      sippymodem_v44_character_path_finish_receive(receiver) ==
		      SIPPYMODEM_V44_CHARACTER_PATH_OK,
	      "complete V.44 character stream did not finalize");
	sippymodem_v44_character_path_destroy(receiver);
	sippymodem_v44_character_path_destroy(sender);
	return true;
}

static bool test_exact_flush_boundary(void)
{
	static const uint8_t payload[] = "one exact V.44 burst boundary";
	uint8_t adapter_wire[256];
	uint8_t direct_wire[256];
	struct character_io io = {
		.source = payload,
		.source_length = sizeof(payload),
	};
	struct sippymodem_v44_character_path *path =
		create_path(&io, true, false, 1);
	size_t adapter_length = 0;
	size_t direct_length = 0;
	size_t output_length = 99;

	CHECK(path != NULL &&
	      transmit_until_idle(
		      path, adapter_wire, sizeof(adapter_wire),
		      &adapter_length) &&
	      direct_transmit(
		      payload, sizeof(payload), direct_wire,
		      sizeof(direct_wire), &direct_length) &&
	      adapter_length == direct_length &&
	      memcmp(adapter_wire, direct_wire, direct_length) == 0 &&
	      io.data_reads == sizeof(payload) && io.empty_reads == 2,
	      "reader starvation did not produce exactly one C-FLUSH");
	CHECK(sippymodem_v44_character_path_transmit(
		      path, adapter_wire, sizeof(adapter_wire),
		      &output_length) == SIPPYMODEM_V44_CHARACTER_PATH_IDLE &&
	      output_length == 0 && io.empty_reads == 3,
	      "repeated empty reads emitted another C-FLUSH");
	sippymodem_v44_character_path_destroy(path);
	return true;
}

static bool test_literal_path(void)
{
	static const uint8_t payload[] = "literal character path";
	uint8_t decoded[sizeof(payload)];
	uint8_t wire[sizeof(payload) + 1];
	struct character_io sender_io = {
		.source = payload,
		.source_length = sizeof(payload),
	};
	struct character_io receiver_io = {
		.sink = decoded,
		.sink_capacity = sizeof(decoded),
		.block_period = 2,
	};
	struct sippymodem_v44_character_path *sender =
		create_path(&sender_io, false, false, 1);
	struct sippymodem_v44_character_path *receiver =
		create_path(&receiver_io, false, false, 1);
	size_t wire_length = 0;

	CHECK(sender != NULL && receiver != NULL &&
	      transmit_until_idle(sender, wire, sizeof(wire), &wire_length) &&
	      wire_length == sizeof(payload) &&
	      memcmp(wire, payload, sizeof(payload)) == 0,
	      "disabled V.44 path was not literal");
	CHECK(receive_wire(
		      receiver, wire, wire_length, &receiver_io,
		      sizeof(payload)) &&
	      receiver_io.sink_length == sizeof(payload) &&
	      memcmp(decoded, payload, sizeof(payload)) == 0,
	      "literal character receive did not preserve backpressure");
	CHECK(sippymodem_v44_character_path_finish_receive(receiver) ==
		      SIPPYMODEM_V44_CHARACTER_PATH_OK,
	      "literal character receive did not finalize");
	sippymodem_v44_character_path_destroy(receiver);
	sippymodem_v44_character_path_destroy(sender);
	return true;
}

static bool test_validation_and_callback_failures(void)
{
	uint8_t octet = 0x5a;
	static const uint8_t truncated[] = { 0x01, 0x00 };
	struct character_io io = {
		.source = &octet,
		.source_length = 1,
		.sink = &octet,
		.sink_capacity = 1,
	};
	struct sippymodem_v44_character_path_config invalid = {
		.receive_buffer_capacity = 1,
		.context = &io,
		.read = read_character,
		.write = write_character,
	};
	struct sippymodem_v44_character_path *path =
		create_path(&io, false, false, 1);
	size_t length = 99;

	CHECK(path != NULL &&
	      sippymodem_v44_character_path_create(NULL) == NULL,
	      "V.44 character path validation accepted a null config");
	invalid.read = NULL;
	CHECK(sippymodem_v44_character_path_create(&invalid) == NULL,
	      "V.44 character path accepted a null reader");
	invalid.read = read_character;
	invalid.receive_buffer_capacity = 0;
	CHECK(sippymodem_v44_character_path_create(&invalid) == NULL,
	      "V.44 character path accepted an empty receive buffer");
	CHECK(sippymodem_v44_character_path_transmit(
		      path, NULL, 1, &length) ==
		      SIPPYMODEM_V44_CHARACTER_PATH_INVALID_ARGUMENT &&
	      length == 99,
	      "invalid character transmit changed caller state");
	CHECK(sippymodem_v44_character_path_receive(
		      path, NULL, 1, &length) ==
		      SIPPYMODEM_V44_CHARACTER_PATH_INVALID_ARGUMENT &&
	      length == 99,
	      "invalid character receive changed caller state");
	io.read_fault = -1;
	CHECK(sippymodem_v44_character_path_transmit(
		      path, NULL, 0, &length) ==
		      SIPPYMODEM_V44_CHARACTER_PATH_IO_ERROR &&
	      length == 0 &&
	      sippymodem_v44_character_path_transmit(
		      path, NULL, 0, &length) ==
		      SIPPYMODEM_V44_CHARACTER_PATH_IO_ERROR,
	      "reader failure did not latch its direction");
	sippymodem_v44_character_path_destroy(path);
	io.read_fault = 0;
	io.write_fault = 1;
	path = create_path(&io, false, false, 1);
	CHECK(path != NULL &&
	      sippymodem_v44_character_path_receive(
		      path, &octet, 1, &length) ==
		      SIPPYMODEM_V44_CHARACTER_PATH_IO_ERROR &&
	      length == 1 &&
	      sippymodem_v44_character_path_receive(
		      path, NULL, 0, &length) ==
		      SIPPYMODEM_V44_CHARACTER_PATH_IO_ERROR,
	      "writer contract failure did not preserve consumed input");
	CHECK(strcmp(sippymodem_v44_character_path_result_name(
		     SIPPYMODEM_V44_CHARACTER_PATH_IDLE), "idle") == 0 &&
	      strcmp(sippymodem_v44_character_path_result_name(
		     SIPPYMODEM_V44_CHARACTER_PATH_NEED_OUTPUT),
		     "need output") == 0 &&
	      strcmp(sippymodem_v44_character_path_result_name(
		     (enum sippymodem_v44_character_path_result)99),
		     "unknown") == 0,
	      "V.44 character path result names changed");
	sippymodem_v44_character_path_destroy(path);
	io = (struct character_io){
		.sink = &octet,
		.sink_capacity = 1,
	};
	path = create_path(&io, false, true, 1);
	CHECK(path != NULL &&
	      sippymodem_v44_character_path_receive(
		      path, truncated, sizeof(truncated), &length) ==
		      SIPPYMODEM_V44_CHARACTER_PATH_OK &&
	      length == sizeof(truncated) &&
	      sippymodem_v44_character_path_finish_receive(path) ==
		      SIPPYMODEM_V44_CHARACTER_PATH_CODEC_ERROR,
	      "truncated V.44 character stream did not fail closed");
	sippymodem_v44_character_path_destroy(path);
	return true;
}

int main(void)
{
	struct test_case {
		const char *name;
		bool (*run)(void);
	};
	static const struct test_case tests[] = {
		{ "generated round trip", test_generated_round_trip },
		{ "exact flush boundary", test_exact_flush_boundary },
		{ "literal path", test_literal_path },
		{ "validation and callback failures",
		  test_validation_and_callback_failures },
	};

	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		else {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0 ? 0 : 1;
}
