// SPDX-License-Identifier: BSD-3-Clause

#include <modem.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ARRAY_LENGTH(array) (sizeof(array) / sizeof((array)[0]))

#define CHECK(condition, ...)                                                \
	do {                                                                   \
		if (!(condition)) {                                              \
			fprintf(stderr, "%s:%d: ", __FILE__, __LINE__);          \
			fprintf(stderr, __VA_ARGS__);                              \
			fputc('\n', stderr);                                      \
			return false;                                              \
		}                                                              \
	} while (0)

struct byte_stream {
	const uint8_t *input;
	size_t input_length;
	size_t input_offset;
	uint8_t *output;
	size_t output_capacity;
	size_t output_length;
	bool output_overflow;
};

static int read_chars(struct modem *modem, char *output, int count)
{
	struct byte_stream *stream = modem->dev_data;
	size_t available = stream->input_length - stream->input_offset;
	size_t requested = (size_t)count;

	if (requested > available)
		requested = available;
	memcpy(output, stream->input + stream->input_offset, requested);
	stream->input_offset += requested;
	return (int)requested;
}

static int write_chars(struct modem *modem, const char *input, int count)
{
	struct byte_stream *stream = modem->dev_data;
	size_t requested = (size_t)count;
	size_t available = stream->output_capacity - stream->output_length;

	if (requested > available) {
		stream->output_overflow = true;
		return 0;
	}
	memcpy(stream->output + stream->output_length, input, requested);
	stream->output_length += requested;
	return count;
}

static bool test_async_round_trip(void)
{
	static const uint8_t input[] = { 0x00, 0xff, 0x55, 0xaa };
	static const uint8_t expected[] = { 0x00, 0x5f, 0xf5, 0x54, 0xab };
	uint8_t encoded[ARRAY_LENGTH(expected)] = { 0 };
	uint8_t decoded[ARRAY_LENGTH(input)] = { 0 };
	struct byte_stream source = {
		.input = input,
		.input_length = ARRAY_LENGTH(input),
	};
	struct byte_stream sink = {
		.output = decoded,
		.output_capacity = ARRAY_LENGTH(decoded),
	};
	struct modem transmitter = {
		.name = "async-transmitter",
		.dev_data = &source,
		.get_chars = read_chars,
	};
	struct modem receiver = {
		.name = "async-receiver",
		.dev_data = &sink,
		.put_chars = write_chars,
	};

	modem_async_start(&transmitter);
	CHECK(modem_async_get_bits(&transmitter, 8, encoded,
				   (int)ARRAY_LENGTH(encoded)) ==
		      (int)ARRAY_LENGTH(encoded),
	      "async encoder returned an unexpected chunk count");
	CHECK(source.input_offset == ARRAY_LENGTH(input),
	      "async encoder did not consume its input");
	CHECK(memcmp(encoded, expected, sizeof(expected)) == 0,
	      "async encoder produced the wrong bitstream");

	modem_async_start(&receiver);
	CHECK(modem_async_put_bits(&receiver, 8, encoded,
				   (int)ARRAY_LENGTH(encoded)) ==
		      (int)ARRAY_LENGTH(encoded),
	      "async decoder returned an unexpected chunk count");
	CHECK(!sink.output_overflow, "async decoder overflowed its output");
	CHECK(sink.output_length == ARRAY_LENGTH(input),
	      "async decoder produced %zu bytes instead of %zu",
	      sink.output_length, ARRAY_LENGTH(input));
	CHECK(memcmp(decoded, input, sizeof(input)) == 0,
	      "async round trip changed the payload");
	return true;
}

struct hdlc_fixture {
	struct hdlc_frame transmit_frame;
	const uint8_t *payload;
	size_t payload_length;
	bool frame_served;
	unsigned int transmit_completions;
	uint8_t received[64];
	size_t received_length;
	unsigned int receive_completions;
	bool receive_overflow;
};

static struct hdlc_frame *next_transmit_frame(void *context)
{
	struct hdlc_fixture *fixture = context;

	if (fixture->frame_served)
		return NULL;
	fixture->frame_served = true;
	fixture->transmit_frame.buf = (uint8_t *)fixture->payload;
	fixture->transmit_frame.size = (int)fixture->payload_length;
	fixture->transmit_frame.count = 0;
	return &fixture->transmit_frame;
}

static void transmit_complete(void *context, frame_t *frame)
{
	struct hdlc_fixture *fixture = context;

	(void)frame;
	fixture->transmit_completions++;
}

static void receive_complete(void *context, frame_t *frame)
{
	struct hdlc_fixture *fixture = context;
	size_t received = (size_t)frame->count;

	if (received > sizeof(fixture->received)) {
		fixture->receive_overflow = true;
		return;
	}
	memcpy(fixture->received, frame->buf, received);
	fixture->received_length = received;
	fixture->receive_completions++;
}

static bool test_hdlc_round_trip(void)
{
	static const uint8_t payload[] = {
		0x01, 0x7e, 0x7d, 0x00, 0xff, 0xf8, 0x55, 0xaa,
	};
	struct hdlc_fixture fixture = {
		.payload = payload,
		.payload_length = ARRAY_LENGTH(payload),
	};
	struct modem transmitter = { .name = "hdlc-transmitter" };
	struct modem receiver = { .name = "hdlc-receiver" };

	modem_hdlc_start(&transmitter);
	modem_hdlc_start(&receiver);
	transmitter.packer.hdlc.framer = &fixture;
	transmitter.packer.hdlc.get_tx_frame = next_transmit_frame;
	transmitter.packer.hdlc.tx_complete = transmit_complete;
	receiver.packer.hdlc.framer = &fixture;
	receiver.packer.hdlc.rx_complete = receive_complete;

	for (unsigned int iteration = 0;
	     iteration < 32 && fixture.receive_completions == 0;
	     ++iteration) {
		uint8_t wire_data[16];
		int produced = modem_hdlc_get_bits(
			&transmitter, 8, wire_data, (int)ARRAY_LENGTH(wire_data));

		CHECK(produced > 0, "HDLC encoder stopped before completing a frame");
		CHECK(modem_hdlc_put_bits(&receiver, 8, wire_data, produced) ==
			      produced,
		      "HDLC decoder did not consume the encoded data");
	}

	CHECK(fixture.transmit_completions == 1,
	      "HDLC transmitter completed %u frames instead of one",
	      fixture.transmit_completions);
	CHECK(fixture.receive_completions == 1,
	      "HDLC receiver completed %u frames instead of one",
	      fixture.receive_completions);
	CHECK(!fixture.receive_overflow, "HDLC receiver overflowed its fixture");
	CHECK(fixture.received_length == ARRAY_LENGTH(payload),
	      "HDLC receiver produced %zu bytes instead of %zu",
	      fixture.received_length, ARRAY_LENGTH(payload));
	CHECK(memcmp(fixture.received, payload, sizeof(payload)) == 0,
	      "HDLC round trip changed the payload");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "async round trip", test_async_round_trip },
		{ "HDLC round trip", test_hdlc_round_trip },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		printf("[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run()) {
			printf("[       OK ] %s\n", tests[index].name);
		} else {
			printf("[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}

	if (failures != 0)
		fprintf(stderr, "%u test(s) failed\n", failures);
	return failures == 0 ? 0 : 1;
}
