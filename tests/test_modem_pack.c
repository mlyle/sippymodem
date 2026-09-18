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

static bool detection_character_matches(
	const uint8_t *bits, uint8_t character)
{
	for (unsigned int bit = 0; bit < 9; ++bit) {
		if (bits[bit] != 1)
			return false;
	}
	for (unsigned int bit = 0; bit < 9; ++bit) {
		if (bits[9 + bit] != ((character >> (8 - bit)) & 1u))
			return false;
	}
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
	enum {
		EXPECTED_ADP_PATTERNS = 16,
		EXPECTED_PATTERN_BITS = 36,
		EXPECTED_ADP_BITS =
			EXPECTED_ADP_PATTERNS * EXPECTED_PATTERN_BITS,
		EARLY_ADP_BITS = 3 * EXPECTED_PATTERN_BITS,
	};
	uint8_t bits[EXPECTED_ADP_BITS];
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

	pattern_source.packer.detector.tx_count = 1;
	pattern_source.packer.detector.tx_pattern[0] = 0x88;
	pattern_source.packer.detector.tx_pattern[1] = 0x89;
	CHECK(modem_detector_get_bits(
		      &pattern_source, 1, bits, 37) == 37 &&
		      detection_character_matches(bits, 0x88) &&
		      detection_character_matches(bits + 18, 0x89) &&
		      modem_detector_put_bits(
			      &answerer, 1, bits, 37) == 37,
	      "raw answerer did not consume the first ODP");
	CHECK(answerer.packer.detector.rx_count == 2 &&
		      answerer.packer.detector.tx_count == 0,
	      "one ODP prematurely started the raw answer");
	CHECK(modem_detector_get_bits(
		      &pattern_source, 1, bits, 36) == 36 &&
		      modem_detector_put_bits(
			      &answerer, 1, bits, 36) == 36,
	      "raw answerer did not consume the second ODP");
	CHECK(answerer.packer.detector.rx_count == 4 &&
		      answerer.packer.detector.tx_count == 1 &&
		      answerer.bit_timer == 0 &&
		      answerer.packer.detector.ec_enable == 0,
	      "two ODPs did not start the ADP-V.14 response");

	modem_detector_start(&originator);
	CHECK(modem_detector_get_bits(&answerer, 1, bits, 72) == 72 &&
		      modem_detector_put_bits(
			      &originator, 1, bits, 72) == 72,
	      "originator did not consume the first ADP-V.14");
	CHECK(modem_detector_get_bits(&answerer, 1, bits + 72, 36) == 36 &&
		      modem_detector_put_bits(
			      &originator, 1, bits + 72, 36) == 36,
	      "originator did not consume the second ADP-V.14");
	CHECK(originator.packer.detector.rx_count == 4 &&
		      originator.packer.detector.ec_enable == 0,
	      "originator did not accept the V.14 selection");
	CHECK(modem_detector_get_bits(
		      &answerer, 1, bits + EARLY_ADP_BITS,
		      EXPECTED_ADP_BITS - EARLY_ADP_BITS - 1) ==
		      EXPECTED_ADP_BITS - EARLY_ADP_BITS - 1 &&
		      answerer.packer.detector.tx_count ==
			      2 * EXPECTED_ADP_PATTERNS + 1 &&
		      answerer.pack.bit == 1 && answerer.bit_timer == 0,
	      "V.14 answerer did not retain the final ADP bit");
	CHECK(modem_detector_get_bits(
		      &answerer, 1, bits + EXPECTED_ADP_BITS - 1, 1) == 1,
	      "V.14 answerer did not emit the final ADP bit");
	CHECK(answerer.packer.detector.tx_count == 0 &&
		      answerer.bit_timer_func == NULL,
	      "V.14 answerer did not finish after exactly 16 ADPs");
	for (size_t pattern = 0; pattern < EXPECTED_ADP_PATTERNS; ++pattern) {
		CHECK(detection_character_matches(
			      bits + pattern * EXPECTED_PATTERN_BITS, 0xa2),
		      "V.14 ADP pattern %zu did not begin with ADP0", pattern);
		CHECK(detection_character_matches(
			      bits + pattern * EXPECTED_PATTERN_BITS + 18, 0x00),
		      "V.14 ADP pattern %zu did not end with ADP1", pattern);
	}
	return true;
}

static bool test_v42_waits_for_a_delayed_peer(void)
{
	uint8_t bits[512];
	struct modem originator = {
		.name = "delayed-v42-originator",
		.caller = 1,
		.tx_rate = 9600,
		.cfg = { .ec = 1, .ec_detector = 1 },
	};
	struct modem answerer = {
		.name = "delayed-v42-answerer",
		.tx_rate = 9600,
		.cfg = { .ec = 1, .ec_detector = 1 },
	};

	modem_detector_start(&originator);
	CHECK(originator.bit_timer == 512 &&
		      originator.packer.detector.tx_count == 0,
	      "V.42 originator did not begin with the 512-bit mark interval");
	CHECK(modem_detector_get_bits(
		      &originator, 1, bits, 512) == 512,
	      "V.42 originator did not emit its initial mark interval");
	originator.bit_timer = 0;
	originator.bit_timer_func(&originator);
	CHECK(originator.bit_timer == 0 &&
		      originator.packer.detector.tx_count == 1,
	      "V.42 originator did not begin its continuous ODP run");

	/* Discard more than the former T400-limited ODP run before the answerer
	 * reaches its own packet-protocol boundary. */
	for (unsigned int iteration = 0; iteration < 101; ++iteration)
		CHECK(modem_detector_get_bits(
			      &originator, 1, bits, (int)ARRAY_LENGTH(bits)) ==
			      (int)ARRAY_LENGTH(bits),
		      "V.42 originator stopped during the delayed ODP run");
	CHECK(originator.bit_timer == 0 &&
		      originator.packer.detector.tx_count > 1 &&
		      originator.packer.detector.ec_enable == 0,
	      "V.42 originator abandoned detection before its peer arrived");

	modem_detector_start(&answerer);
	CHECK(answerer.bit_timer == 2073600,
	      "V.42 answerer armed %d initial detection bits",
	      answerer.bit_timer);
	for (unsigned int iteration = 0;
	     iteration < 4 && answerer.packer.detector.rx_count < 4;
	     ++iteration) {
		CHECK(modem_detector_get_bits(
			      &originator, 1, bits, (int)ARRAY_LENGTH(bits)) ==
			      (int)ARRAY_LENGTH(bits) &&
			      modem_detector_put_bits(
				      &answerer, 1, bits,
				      (int)ARRAY_LENGTH(bits)) ==
				      (int)ARRAY_LENGTH(bits),
		      "V.42 answerer did not consume delayed ODP");
	}
	CHECK(answerer.packer.detector.rx_count == 4 &&
		      answerer.packer.detector.tx_count == 1 &&
		      answerer.packer.detector.ec_enable == 1,
	      "V.42 answerer did not accept delayed ODP");

	for (unsigned int iteration = 0;
	     iteration < 4 && originator.packer.detector.rx_count < 4;
	     ++iteration) {
		CHECK(modem_detector_get_bits(
			      &answerer, 1, bits, (int)ARRAY_LENGTH(bits)) ==
			      (int)ARRAY_LENGTH(bits) &&
			      modem_detector_put_bits(
				      &originator, 1, bits,
				      (int)ARRAY_LENGTH(bits)) ==
				      (int)ARRAY_LENGTH(bits),
		      "V.42 originator did not consume delayed ADP");
	}
	CHECK(originator.packer.detector.rx_count == 4 &&
		      originator.packer.detector.ec_enable == 1,
	      "V.42 originator did not accept the delayed response");
	return true;
}

static bool test_v42_flags_do_not_extend_detection(void)
{
	uint8_t bits[128];
	struct modem flag_source = { .name = "V.42 flag source" };
	struct modem detector = {
		.name = "V.42 flag detector",
		.tx_rate = 4800,
		.cfg = { .ec = 1, .ec_detector = 1 },
	};

	modem_hdlc_start(&flag_source);
	modem_detector_start(&detector);
	CHECK(modem_hdlc_get_bits(
		      &flag_source, 1, bits, (int)ARRAY_LENGTH(bits)) ==
		      (int)ARRAY_LENGTH(bits) &&
		      modem_detector_put_bits(
			      &detector, 1, bits, (int)ARRAY_LENGTH(bits)) ==
			      (int)ARRAY_LENGTH(bits) &&
		      detector.packer.detector.ec_enable == 1 &&
		      detector.packer.detector.tx_count == 1 &&
		      detector.bit_timer == 0,
	      "V.42 detector did not accept the HDLC flag transition");
	CHECK(modem_hdlc_get_bits(
		      &flag_source, 1, bits, (int)ARRAY_LENGTH(bits)) ==
		      (int)ARRAY_LENGTH(bits) &&
		      modem_detector_put_bits(
			      &detector, 1, bits, (int)ARRAY_LENGTH(bits)) ==
			      (int)ARRAY_LENGTH(bits) &&
		      detector.packer.detector.tx_count == 1 &&
		      detector.bit_timer == 0,
	      "continued HDLC flags postponed V.42 completion");
	return true;
}

static bool receive_detection_bits(
	struct modem *modem, const uint8_t *bits, size_t count, unsigned int width)
{
	for (size_t offset = 0; offset < count;) {
		unsigned int chunk = count - offset < width ?
			(unsigned int)(count - offset) : width;
		uint8_t value = 0;

		for (unsigned int bit = 0; bit < chunk; ++bit)
			value = (uint8_t)((value << 1) | bits[offset++]);
		CHECK(modem_detector_put_bits(modem, (int)chunk, &value, 1) == 1,
		      "detector did not consume a %u-bit group", chunk);
	}
	return true;
}

static bool test_v42_requires_adjacent_synchronous_flags(void)
{
	static const struct {
		uint32_t bits;
		unsigned int count;
		bool flags;
	} patterns[] = {
		{ 0x7e7e, 16, true },
		{ 0x0fd, 10, false }, /* Start, async 0x7e, stop. */
		{ 0x7e, 8, false },
		{ (0x7e << 9) | 0x100 | 0x7e, 17, false }, /* Mark gap. */
		{ 0x7e7e, 15, false }, /* Missing the first flag's opening zero. */
	};

	for (int caller = 0; caller <= 1; ++caller) {
		for (unsigned int width = 1; width <= 8; ++width) {
			struct modem modem = {
				.name = "V.42 flag boundary", .caller = caller,
				.tx_rate = 9600, .cfg = { .ec = 1, .ec_detector = 1 },
			};
			uint8_t bits[64];

			for (unsigned int prefix = 0; prefix < 8; ++prefix) {
				for (size_t pattern = 0; pattern < ARRAY_LENGTH(patterns);
				     ++pattern) {
					memset(bits, 1, sizeof(bits));
					for (unsigned int bit = 0; bit < patterns[pattern].count;
					     ++bit)
						bits[prefix + bit] = (uint8_t)((patterns[pattern].bits >>
							(patterns[pattern].count - bit - 1)) & 1u);
					modem_detector_start(&modem);
					CHECK(receive_detection_bits(&modem, bits, sizeof(bits), width),
					      "flag fixture stopped");
					CHECK((modem.packer.detector.ec_enable != 0) ==
					      patterns[pattern].flags,
					      "flag decision changed for pattern %zu, caller %d, width %u, prefix %u",
					      pattern, caller, width, prefix);
				}
			}
			for (unsigned int split = 1; split < 16; ++split) {
				memset(bits, 1, sizeof(bits));
				for (unsigned int bit = 0; bit < 16; ++bit)
					bits[bit] = (uint8_t)((0x7e7eu >> (15 - bit)) & 1u);
				modem_detector_start(&modem);
				CHECK(receive_detection_bits(&modem, bits, split, width),
				      "pre-retrain flags stopped");
				modem_detector_reframe(&modem);
				CHECK(receive_detection_bits(
					      &modem, bits + split, sizeof(bits) - split, width) &&
				      modem.packer.detector.ec_enable == 0,
				      "flag adjacency survived retrain at bit %u", split);
				CHECK(receive_detection_bits(&modem, bits, sizeof(bits), width) &&
				      modem.packer.detector.ec_enable == 1,
				      "post-retrain flags were lost at bit %u", split);
			}
		}
	}
	return true;
}

static bool test_v42_rejects_bad_detection_stops(void)
{
	static const struct {
		bool caller;
		uint8_t characters[2];
		bool reliable;
	} patterns[] = {
		{ false, { 0x11, 0x91 }, true },
		{ true, { 'E', 'C' }, true },
		{ true, { 'E', 0 }, false },
	};

	for (size_t pattern = 0; pattern < ARRAY_LENGTH(patterns); ++pattern) {
		for (unsigned width = 1; width <= 8; ++width) {
			for (unsigned broken = 0; broken < 4; ++broken) {
				struct modem modem = {
					.name = "V.42 detection stop", .caller = patterns[pattern].caller,
					.tx_rate = 9600, .cfg = { .ec = 1, .ec_detector = 1 },
				};
				uint8_t bits[80];
				size_t count = 3;

				memset(bits, 1, sizeof(bits));
				for (unsigned character = 0; character < 4; ++character) {
					CHECK(append_async_character(bits, sizeof(bits), &count,
						patterns[pattern].characters[character % 2], character != broken),
					      "detection fixture overflow");
					count += 8;
				}
				modem_detector_start(&modem);
				CHECK(receive_detection_bits(&modem, bits, count, width) &&
				      modem.packer.detector.rx_count < 4 &&
				      modem.packer.detector.tx_count == 0,
				      "accepted bad stop: pattern %zu width %u character %u",
				      pattern, width, broken);
				bits[3 + broken * 18 + 9] = 1;
				CHECK(receive_detection_bits(&modem, bits, count, width) &&
				      modem.packer.detector.rx_count == 4 &&
				      (modem.packer.detector.ec_enable != 0) == patterns[pattern].reliable,
				      "valid detection did not recover: pattern %zu width %u character %u",
				      pattern, width, broken);
				if (!modem.caller) {
					/* Once selected, damaged input must not restart the
					 * answerer's partly transmitted ADP reply. */
					modem.packer.detector.tx_count = 7;
					bits[3 + broken * 18 + 9] = 0;
					CHECK(receive_detection_bits(&modem, bits, count, width) &&
					      modem.packer.detector.rx_count == 4 &&
					      modem.packer.detector.tx_count == 7,
					      "bad stop restarted an accepted ADP reply");
				}
			}
		}
	}
	return true;
}

struct hdlc_fixture {
	struct hdlc_frame transmit_frame;
	const uint8_t *payload;
	size_t payload_length;
	bool frame_served;
	unsigned int preamble_completions;
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

static void preamble_complete(struct modem *modem)
{
	struct hdlc_fixture *fixture = modem->packer.hdlc.framer;

	fixture->preamble_completions++;
	modem->packer.hdlc.get_tx_frame = next_transmit_frame;
}

static bool test_hdlc_transmit_preamble(void)
{
	enum { PREAMBLE_FLAGS = 16 };
	static const uint8_t payload[] = { 0x03, 0xaf, 0x82 };
	struct hdlc_fixture fixture = {
		.payload = payload,
		.payload_length = ARRAY_LENGTH(payload),
	};
	struct modem transmitter = { .name = "HDLC preamble transmitter" };
	struct hdlc_state *hdlc;

	modem_hdlc_start(&transmitter);
	hdlc = &transmitter.packer.hdlc;
	hdlc->framer = &fixture;
	hdlc->tx_preamble_flags = PREAMBLE_FLAGS;
	hdlc->tx_preamble_complete = preamble_complete;
	for (unsigned int flag = 0; flag < PREAMBLE_FLAGS; ++flag) {
		uint8_t wire = 0;

		CHECK(modem_hdlc_get_bits(&transmitter, 8, &wire, 1) == 1 &&
			      wire == 0x7e,
		      "HDLC preamble flag %u changed", flag);
		CHECK(fixture.preamble_completions ==
			      (flag + 1u == PREAMBLE_FLAGS) &&
			      hdlc->tx_preamble_flags ==
				      PREAMBLE_FLAGS - flag - 1u,
		      "HDLC preamble completed at flag %u", flag);
	}
	CHECK(hdlc->tx_preamble_complete == NULL && fixture.frame_served &&
		      hdlc->tx_frame == &fixture.transmit_frame,
	      "HDLC preamble did not arm exactly one source frame");
	return true;
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
		{ "V.42 delayed startup",
		  test_v42_waits_for_a_delayed_peer },
		{ "V.42 bounded HDLC flag transition",
		  test_v42_flags_do_not_extend_detection },
		{ "V.42 synchronous flag boundaries",
		  test_v42_requires_adjacent_synchronous_flags },
		{ "V.42 detection stop validation",
		  test_v42_rejects_bad_detection_stops },
		{ "HDLC transmit preamble", test_hdlc_transmit_preamble },
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
