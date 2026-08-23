// SPDX-License-Identifier: BSD-3-Clause

#include <modem.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

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

static bool append_async_character(
	uint8_t *bits, size_t capacity, size_t *length,
	uint8_t character, bool valid_stop)
{
	if (*length + 10 > capacity)
		return false;
	bits[(*length)++] = 0;
	for (unsigned int bit = 0; bit < 8; ++bit)
		bits[(*length)++] = (uint8_t)((character >> bit) & 1u);
	bits[(*length)++] = valid_stop ? 1u : 0u;
	return true;
}

static bool test_async_resynchronizes_after_bad_stop(void)
{
	uint8_t bits[64] = { 0 };
	uint8_t decoded[3] = { 0 };
	size_t bit_count = 0;
	struct byte_stream sink = {
		.output = decoded,
		.output_capacity = ARRAY_LENGTH(decoded),
	};
	struct modem receiver = {
		.name = "async-resynchronization-receiver",
		.dev_data = &sink,
		.put_chars = write_chars,
	};

	CHECK(append_async_character(
		      bits, ARRAY_LENGTH(bits), &bit_count, 'A', true) &&
		      append_async_character(
			      bits, ARRAY_LENGTH(bits), &bit_count, 0x55, false) &&
		      append_async_character(
			      bits, ARRAY_LENGTH(bits), &bit_count, 0xff, true) &&
		      append_async_character(
			      bits, ARRAY_LENGTH(bits), &bit_count, 'B', true),
	      "async resynchronization fixture overflowed");

	modem_async_start(&receiver);
	CHECK(modem_async_put_bits(
		      &receiver, 1, bits, (int)bit_count) == (int)bit_count,
	      "async decoder did not consume the malformed stream");
	CHECK(!sink.output_overflow,
	      "async resynchronization overflowed its output");
	CHECK(sink.output_length == 2 && decoded[0] == 'A' && decoded[1] == 'B',
	      "async decoder retained malformed framing or missed recovery");
	return true;
}

static bool test_async_reframe_discards_partial_characters(void)
{
	static const uint8_t partial[] = { 0, 1, 0, 1, 0 };
	uint8_t complete[10] = { 0 };
	uint8_t decoded = 0;
	size_t bit_count = 0;
	struct byte_stream sink = {
		.output = &decoded,
		.output_capacity = 1,
	};
	struct modem receiver = {
		.name = "async-reset-receiver",
		.dev_data = &sink,
		.put_chars = write_chars,
	};

	modem_async_start(&receiver);
	CHECK(modem_async_put_bits(
		      &receiver, 1, (uint8_t *)partial,
		      (int)ARRAY_LENGTH(partial)) == (int)ARRAY_LENGTH(partial),
	      "async decoder did not consume the partial character");
	receiver.pack.bit = 7;
	receiver.pack.data = 0x1234;
	modem_async_reframe(&receiver);
	CHECK(receiver.pack.bit == 0 && receiver.pack.data == 0,
	      "async reframe retained queued transmit framing");
	CHECK(append_async_character(
		      complete, ARRAY_LENGTH(complete), &bit_count, 'R', true),
	      "async reset fixture overflowed");
	CHECK(modem_async_put_bits(
		      &receiver, 1, complete, (int)bit_count) == (int)bit_count,
	      "async decoder did not consume the post-reset character");
	CHECK(!sink.output_overflow && sink.output_length == 1 && decoded == 'R',
	      "async reframe retained a partial receive character");
	return true;
}

static bool test_async_retrain_preserves_transmit_framing(void)
{
	static const uint8_t partial[] = { 0, 1, 0, 1, 0 };
	uint8_t complete[10] = { 0 };
	uint8_t decoded = 0;
	size_t bit_count = 0;
	struct byte_stream sink = {
		.output = &decoded,
		.output_capacity = 1,
	};
	struct modem modem = {
		.name = "async-retrain-modem",
		.dev_data = &sink,
		.put_chars = write_chars,
	};

	modem_async_start(&modem);
	CHECK(modem_async_put_bits(
		      &modem, 1, (uint8_t *)partial,
		      (int)ARRAY_LENGTH(partial)) == (int)ARRAY_LENGTH(partial),
	      "async decoder did not consume the pre-retrain partial character");
	modem.pack.bit = 7;
	modem.pack.data = 0x1234;
	modem_async_retrain(&modem);
	CHECK(modem.pack.bit == 7 && modem.pack.data == 0x1234,
	      "async retrain discarded pending transmit framing");
	CHECK(append_async_character(
		      complete, ARRAY_LENGTH(complete), &bit_count, 'T', true),
	      "async retrain fixture overflowed");
	CHECK(modem_async_put_bits(
		      &modem, 1, complete, (int)bit_count) == (int)bit_count,
	      "async decoder did not consume the post-retrain character");
	CHECK(!sink.output_overflow && sink.output_length == 1 && decoded == 'T',
	      "async retrain retained a partial receive character");
	return true;
}

static bool test_async_preserves_raw_data_through_t400(void)
{
	static const uint8_t input[] = { 'T' };
	uint8_t guarded[900];
	uint8_t encoded[2] = { 0 };
	struct byte_stream source = {
		.input = input,
		.input_length = ARRAY_LENGTH(input),
	};
	struct dp datapump = { .id = DP_V34 };
	struct modem modem = {
		.name = "raw-guard-transmitter",
		.dev_data = &source,
		.get_chars = read_chars,
		.dp = &datapump,
		.rx_rate = 9600,
		.tx_rate = 9600,
	};

	datapump.modem = &modem;
	modem_async_start(&modem);
	CHECK(modem.packer.async.transmit_mark_bits == 7200,
	      "raw transmitter armed %d T400 mark bits instead of 7200",
	      modem.packer.async.transmit_mark_bits);
	CHECK(modem_async_get_bits(
		      &modem, 8, guarded, (int)ARRAY_LENGTH(guarded)) ==
		      (int)ARRAY_LENGTH(guarded),
	      "raw transmitter did not fill the complete T400 interval");
	for (size_t index = 0; index < ARRAY_LENGTH(guarded); ++index)
		CHECK(guarded[index] == 0xff,
		      "raw T400 guard emitted data at octet %zu", index);
	CHECK(source.input_offset == 0,
	      "raw T400 guard consumed queued DTE data");
	CHECK(modem_async_get_bits(
		      &modem, 8, encoded, (int)ARRAY_LENGTH(encoded)) ==
		      (int)ARRAY_LENGTH(encoded) &&
		      source.input_offset == ARRAY_LENGTH(input),
	      "raw transmitter did not release queued data after T400");

	datapump.id = DP_V21;
	source.input_offset = 0;
	modem_async_start(&modem);
	CHECK(modem.packer.async.transmit_mark_bits == 0,
	      "raw-only data pump armed a V.42 guard");
	CHECK(modem_async_get_bits(&modem, 8, encoded, 1) == 1 &&
		      source.input_offset == ARRAY_LENGTH(input),
	      "raw-only data pump delayed DTE data");
	return true;
}

static bool test_raw_answerer_selects_v14(void)
{
	uint8_t bits[40];
	struct dp answer_datapump = { .id = DP_V34 };
	struct modem answerer = {
		.name = "raw-v42-answerer",
		.dp = &answer_datapump,
		.rx_rate = 9600,
		.tx_rate = 9600,
	};
	struct modem originator = {
		.name = "v42-originator",
		.caller = 1,
		.rx_rate = 9600,
		.tx_rate = 9600,
		.cfg = { .ec = 1, .ec_detector = 1 },
	};
	struct modem pattern_source = {
		.name = "odp-source",
	};

	answer_datapump.modem = &answerer;
	modem_detector_start(&answerer);
	CHECK(answerer.bit_timer == 7200 &&
		      answerer.packer.detector.tx_pattern[0] == 0xa2 &&
		      answerer.packer.detector.tx_pattern[1] == 0x00,
	      "raw answerer did not arm T400 with ADP-V.14");

	pattern_source.packer.detector.tx_count = 2;
	pattern_source.packer.detector.tx_pattern[0] = 0x88;
	pattern_source.packer.detector.tx_pattern[1] = 0x89;
	CHECK(modem_detector_get_bits(
		      &pattern_source, 1, bits, (int)ARRAY_LENGTH(bits)) ==
		      (int)ARRAY_LENGTH(bits) &&
		      modem_detector_put_bits(
			      &answerer, 1, bits, (int)ARRAY_LENGTH(bits)) ==
		      (int)ARRAY_LENGTH(bits),
	      "raw answerer did not consume the first ODP");
	CHECK(answerer.packer.detector.rx_count == 2 &&
		      answerer.packer.detector.tx_count == 0,
	      "one ODP prematurely started the raw answer");
	CHECK(modem_detector_get_bits(
		      &pattern_source, 1, bits, (int)ARRAY_LENGTH(bits)) ==
		      (int)ARRAY_LENGTH(bits) &&
		      modem_detector_put_bits(
			      &answerer, 1, bits, (int)ARRAY_LENGTH(bits)) ==
		      (int)ARRAY_LENGTH(bits),
	      "raw answerer did not consume the second ODP");
	CHECK(answerer.packer.detector.rx_count == 4 &&
		      answerer.packer.detector.tx_count == 2 &&
		      answerer.packer.detector.ec_enable == 0,
	      "two ODPs did not start the ADP-V.14 response");

	modem_detector_start(&originator);
	CHECK(modem_detector_get_bits(
		      &answerer, 1, bits, (int)ARRAY_LENGTH(bits)) ==
		      (int)ARRAY_LENGTH(bits) &&
		      modem_detector_put_bits(
			      &originator, 1, bits, (int)ARRAY_LENGTH(bits)) ==
		      (int)ARRAY_LENGTH(bits),
	      "originator did not consume the first ADP-V.14");
	CHECK(modem_detector_get_bits(
		      &answerer, 1, bits, (int)ARRAY_LENGTH(bits)) ==
		      (int)ARRAY_LENGTH(bits) &&
		      modem_detector_put_bits(
			      &originator, 1, bits, (int)ARRAY_LENGTH(bits)) ==
		      (int)ARRAY_LENGTH(bits),
	      "originator did not consume the second ADP-V.14");
	CHECK(originator.packer.detector.rx_count == 4 &&
		      originator.packer.detector.ec_enable == 0,
	      "originator did not accept the V.14 selection");
	return true;
}

static bool test_v42_originator_t400_is_bounded(void)
{
	struct modem fallback = {
		.name = "v42-fallback-originator",
		.caller = 1,
		.tx_rate = 9600,
		.cfg = { .ec = 1, .ec_detector = 1 },
	};
	struct modem required = fallback;

	modem_detector_start(&fallback);
	CHECK(fallback.bit_timer == 192 &&
		      fallback.packer.detector.tx_count == 0,
	      "V.42 originator did not begin with a 20 ms mark interval");
	fallback.bit_timer_func(&fallback);
	CHECK(fallback.bit_timer == 7200 &&
		      fallback.packer.detector.tx_count == 2,
	      "V.42 originator did not arm ODP for T400");
	fallback.bit_timer_func(&fallback);
	CHECK(fallback.cfg.ec == 0,
	      "auto-reliable originator did not fall back after T400");

	required.sregs[SREG_EC] = 4;
	modem_detector_start(&required);
	required.bit_timer_func(&required);
	required.bit_timer_func(&required);
	CHECK(required.cfg.ec == 1,
	      "LAPM-only originator silently fell back after T400");
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
		{ "async bad-stop resynchronization",
		  test_async_resynchronizes_after_bad_stop },
		{ "async reframe",
		  test_async_reframe_discards_partial_characters },
		{ "async retrain framing",
		  test_async_retrain_preserves_transmit_framing },
		{ "async raw T400 guard",
		  test_async_preserves_raw_data_through_t400 },
		{ "raw answerer V.14 selection",
		  test_raw_answerer_selects_v14 },
		{ "V.42 originator T400",
		  test_v42_originator_t400_is_bounded },
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
