// SPDX-License-Identifier: BSD-3-Clause

#include <v44_decoder.h>
#include <v44_encoder.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

static const struct v44_parameters parameters = {
	.codeword_limit = V44_MIN_CODEWORDS,
	.max_string_length = V44_MIN_STRING_LENGTH,
	.history_size = V44_MIN_HISTORY_SIZE,
};

static bool drain_one(
	struct v44_encoder *encoder,
	uint8_t *wire,
	size_t wire_capacity,
	size_t *wire_length)
{
	if (*wire_length >= wire_capacity ||
	    v44_encoder_read(
		    encoder, wire + *wire_length, 1) != 1)
		return false;
	(*wire_length)++;
	return true;
}

static bool progress_is(
	const struct v44_encoder *encoder,
	enum v44_encoder_mode mode,
	uint64_t represented_octets,
	uint64_t wire_bits)
{
	struct v44_encoder_progress progress;

	return v44_encoder_get_progress(encoder, &progress) &&
	       progress.mode == mode &&
	       progress.represented_octets == represented_octets &&
	       progress.wire_bits == wire_bits;
}

static bool pump_input(
	struct v44_encoder *encoder,
	const uint8_t *plain,
	size_t plain_length,
	uint8_t *wire,
	size_t wire_capacity,
	size_t *wire_length)
{
	const uint8_t *input = plain;
	size_t input_length = plain_length;

	for (size_t attempts = 0; input_length != 0; ++attempts) {
		enum v44_encode_result result;

		if (attempts > 100000)
			return false;
		result = v44_encoder_process(
			encoder, &input, &input_length);
		if (result == V44_ENCODE_NEED_OUTPUT) {
			if (!drain_one(
				    encoder, wire, wire_capacity, wire_length))
				return false;
		} else if (result != V44_ENCODE_OK) {
			return false;
		}
	}
	return true;
}

static bool pump_mode(
	struct v44_encoder *encoder,
	enum v44_encoder_mode target,
	uint8_t *wire,
	size_t wire_capacity,
	size_t *wire_length)
{
	for (size_t attempts = 0; attempts < 100000; ++attempts) {
		enum v44_encode_result result =
			v44_encoder_request_mode(encoder, target);

		if (result == V44_ENCODE_OK)
			return true;
		if (result != V44_ENCODE_NEED_OUTPUT ||
		    !drain_one(encoder, wire, wire_capacity, wire_length))
			return false;
	}
	return false;
}

static bool pump_flush(
	struct v44_encoder *encoder,
	uint8_t *wire,
	size_t wire_capacity,
	size_t *wire_length)
{
	for (size_t attempts = 0; attempts < 100000; ++attempts) {
		enum v44_encode_result result =
			v44_encoder_flush(encoder);

		if (result == V44_ENCODE_FLUSHED)
			return true;
		if (result != V44_ENCODE_NEED_OUTPUT ||
		    !drain_one(encoder, wire, wire_capacity, wire_length))
			return false;
	}
	return false;
}

static bool decode_one_byte_chunks(
	const uint8_t *wire,
	size_t wire_length,
	uint8_t *plain,
	size_t plain_capacity,
	size_t *plain_length)
{
	struct v44_decoder *decoder =
		v44_decoder_create(
			&parameters, parameters.max_string_length);
	size_t offset = 0;
	bool flushed = false;

	if (decoder == NULL)
		return false;
	*plain_length = 0;
	for (size_t attempts = 0; !flushed && attempts < 100000; ++attempts) {
		const size_t offered = offset < wire_length ? 1 : 0;
		const uint8_t *input = offered != 0 ? wire + offset : NULL;
		size_t input_length = offered;
		size_t output_length = 0;
		size_t output_capacity = *plain_length < plain_capacity ? 1 : 0;
		enum v44_decode_result result =
			v44_decoder_process(
				decoder, &input, &input_length,
				output_capacity != 0 ? plain + *plain_length : NULL,
				output_capacity, &output_length);

		if (output_length > output_capacity)
			goto fail;
		offset += offered - input_length;
		*plain_length += output_length;
		if (result == V44_DECODE_FLUSHED)
			flushed = true;
		else if (result != V44_DECODE_OK &&
			 result != V44_DECODE_NEED_INPUT &&
			 result != V44_DECODE_NEED_OUTPUT)
			goto fail;
		if (result == V44_DECODE_NEED_INPUT &&
		    offset == wire_length)
			goto fail;
	}
	if (!flushed || offset != wire_length ||
	    v44_decoder_finish(decoder) !=
		    V44_DECODE_OK)
		goto fail;
	v44_decoder_destroy(decoder);
	return true;

fail:
	v44_decoder_destroy(decoder);
	return false;
}

static bool test_exact_mode_round_trip(void)
{
	static const uint8_t compressed_prefix[] = { 'A' };
	static const uint8_t transparent[] = { 'T', 0, 'U' };
	static const uint8_t expected_prefix[] = {
		0x82, 0x01, 'T', 0, 1, 'U', 51, 0,
	};
	struct v44_encoder *encoder =
		v44_encoder_create(&parameters, 2);
	struct v44_encoder *fresh =
		v44_encoder_create(&parameters, 2);
	uint8_t post[96];
	uint8_t wire[512];
	uint8_t fresh_wire[512];
	uint8_t expected_plain[sizeof(compressed_prefix) +
			       sizeof(transparent) + sizeof(post)];
	uint8_t decoded[sizeof(expected_plain)];
	size_t wire_length = 0;
	size_t fresh_length = 0;
	size_t decoded_length = 0;
	const uint8_t *blocked = transparent;
	size_t blocked_length = sizeof(transparent);

	CHECK(encoder != NULL && fresh != NULL,
	      "could not create V.44 mode encoders");
	for (size_t index = 0; index < sizeof(post); ++index)
		post[index] = (uint8_t)"ABRACADABRA"[
			index % (sizeof("ABRACADABRA") - 1)];
	CHECK(pump_input(
		      encoder, compressed_prefix, sizeof(compressed_prefix), wire,
		      sizeof(wire), &wire_length) &&
	      v44_encoder_request_mode(
		      encoder, V44_ENCODER_TRANSPARENT) ==
		      V44_ENCODE_NEED_OUTPUT &&
	      v44_encoder_process(
		      encoder, &blocked, &blocked_length) ==
		      V44_ENCODE_INVALID_STATE &&
	      blocked == transparent && blocked_length == sizeof(transparent) &&
	      v44_encoder_flush(encoder) ==
		      V44_ENCODE_INVALID_STATE &&
	      v44_encoder_request_mode(
		      encoder, V44_ENCODER_COMPRESSED) ==
		      V44_ENCODE_INVALID_STATE &&
	      pump_mode(
		      encoder, V44_ENCODER_TRANSPARENT, wire,
		      sizeof(wire), &wire_length) &&
	      v44_encoder_request_mode(
		      encoder, V44_ENCODER_TRANSPARENT) ==
		      V44_ENCODE_OK,
	      "V.44 ETM transition did not serialize accepted data");
	CHECK(pump_input(
		      encoder, transparent, sizeof(transparent), wire,
		      sizeof(wire), &wire_length) &&
	      pump_mode(
		      encoder, V44_ENCODER_COMPRESSED, wire,
		      sizeof(wire), &wire_length) &&
	      wire_length == sizeof(expected_prefix) &&
	      memcmp(wire, expected_prefix, sizeof(expected_prefix)) == 0,
	      "V.44 ETM/EID/ECM wire sequence changed");
	CHECK(pump_input(
		      encoder, post, sizeof(post), wire, sizeof(wire),
		      &wire_length) &&
	      pump_flush(encoder, wire, sizeof(wire), &wire_length) &&
	      pump_input(
		      fresh, post, sizeof(post), fresh_wire, sizeof(fresh_wire),
		      &fresh_length) &&
	      pump_flush(
		      fresh, fresh_wire, sizeof(fresh_wire), &fresh_length) &&
	      wire_length == sizeof(expected_prefix) + fresh_length &&
	      memcmp(wire + sizeof(expected_prefix), fresh_wire, fresh_length) ==
		      0,
	      "V.44 ECM did not restore a fresh compressed dictionary");
	memcpy(expected_plain, compressed_prefix, sizeof(compressed_prefix));
	memcpy(expected_plain + sizeof(compressed_prefix), transparent,
	       sizeof(transparent));
	memcpy(expected_plain + sizeof(compressed_prefix) + sizeof(transparent),
	       post, sizeof(post));
	CHECK(decode_one_byte_chunks(
		      wire, wire_length, decoded, sizeof(decoded),
		      &decoded_length) &&
	      decoded_length == sizeof(expected_plain) &&
	      memcmp(decoded, expected_plain, sizeof(expected_plain)) == 0,
	      "mixed V.44 modes did not round trip in one-byte chunks");
	v44_encoder_destroy(fresh);
	v44_encoder_destroy(encoder);
	return true;
}

static bool test_escape_cycle_and_transition_backpressure(void)
{
	struct v44_encoder *encoder =
		v44_encoder_create(&parameters, 2);
	uint8_t transition[8];
	size_t transition_length = 0;
	uint8_t escape = 0;
	bool saw_wrap = false;

	CHECK(encoder != NULL &&
	      v44_encoder_request_mode(
		      encoder, V44_ENCODER_COMPRESSED) ==
		      V44_ENCODE_OK &&
	      v44_encoder_request_mode(
		      encoder, (enum v44_encoder_mode)99) ==
		      V44_ENCODE_INVALID_ARGUMENT &&
	      v44_encoder_request_mode(
		      NULL, V44_ENCODER_TRANSPARENT) ==
		      V44_ENCODE_INVALID_ARGUMENT &&
	      pump_mode(
		      encoder, V44_ENCODER_TRANSPARENT, transition,
		      sizeof(transition), &transition_length) &&
	      transition_length == 1 && transition[0] == 0x01,
	      "empty V.44 ETM transition changed its wire image");
	for (unsigned int index = 0; index < 256; ++index) {
		uint8_t value = escape;
		uint8_t next_value = (uint8_t)(escape + 51);
		const uint8_t *input = &value;
		const uint8_t *blocked = &next_value;
		size_t input_length = 1;
		size_t blocked_length = 1;
		uint8_t output = 0xff;

		CHECK(v44_encoder_process(
			      encoder, &input, &input_length) ==
			      V44_ENCODE_OK &&
		      input_length == 0 &&
		      v44_encoder_pending(encoder) == 2 &&
		      v44_encoder_process(
			      encoder, &blocked, &blocked_length) ==
			      V44_ENCODE_NEED_OUTPUT &&
		      blocked == &next_value && blocked_length == 1 &&
		      v44_encoder_read(encoder, &output, 1) == 1 &&
		      output == escape &&
		      v44_encoder_process(
			      encoder, &blocked, &blocked_length) ==
			      V44_ENCODE_NEED_OUTPUT &&
		      blocked == &next_value && blocked_length == 1 &&
		      v44_encoder_read(encoder, &output, 1) == 1 &&
		      output == 1,
		      "V.44 EID rotation %u was not atomic", index);
		if (escape == 255 && next_value == 50)
			saw_wrap = true;
		escape = next_value;
	}
	CHECK(saw_wrap && escape == 0,
	      "V.44 ESCAPE did not cover its modulo-256 cycle");
	{
		static const uint8_t pending[] = { 'Q' };
		static const uint8_t blocked_byte[] = { 'R' };
		const uint8_t *input = pending;
		const uint8_t *blocked = blocked_byte;
		size_t input_length = sizeof(pending);
		size_t blocked_length = sizeof(blocked_byte);
		uint8_t output = 0xff;

		CHECK(v44_encoder_process(
			      encoder, &input, &input_length) ==
			      V44_ENCODE_OK &&
		      v44_encoder_request_mode(
			      encoder, V44_ENCODER_COMPRESSED) ==
			      V44_ENCODE_NEED_OUTPUT &&
		      v44_encoder_process(
			      encoder, &blocked, &blocked_length) ==
			      V44_ENCODE_INVALID_STATE &&
		      blocked == blocked_byte &&
		      blocked_length == sizeof(blocked_byte) &&
		      v44_encoder_request_mode(
			      encoder, V44_ENCODER_TRANSPARENT) ==
			      V44_ENCODE_INVALID_STATE &&
		      v44_encoder_read(encoder, &output, 1) == 1 &&
		      output == 'Q' &&
		      v44_encoder_request_mode(
			      encoder, V44_ENCODER_COMPRESSED) ==
			      V44_ENCODE_NEED_OUTPUT &&
		      v44_encoder_read(encoder, &output, 1) == 1 &&
		      output == 0 &&
		      v44_encoder_request_mode(
			      encoder, V44_ENCODER_COMPRESSED) ==
			      V44_ENCODE_NEED_OUTPUT &&
		      v44_encoder_read(encoder, &output, 1) == 1 &&
		      output == 0 &&
		      v44_encoder_request_mode(
			      encoder, V44_ENCODER_COMPRESSED) ==
			      V44_ENCODE_OK,
		      "V.44 ECM transition duplicated or reordered output");
	}
	v44_encoder_destroy(encoder);
	return true;
}

static bool test_transparent_flush(void)
{
	static const uint8_t plain[] = { 'X' };
	struct v44_encoder *encoder =
		v44_encoder_create(&parameters, 2);
	uint8_t wire[8];
	size_t wire_length = 0;
	const uint8_t *input = plain;
	size_t input_length = sizeof(plain);
	const uint8_t *blocked = plain;
	size_t blocked_length = sizeof(plain);

	CHECK(encoder != NULL &&
	      pump_mode(
		      encoder, V44_ENCODER_TRANSPARENT, wire,
		      sizeof(wire), &wire_length),
	      "could not enter V.44 transparent flush mode");
	wire_length = 0;
	CHECK(v44_encoder_process(
		      encoder, &input, &input_length) ==
		      V44_ENCODE_OK &&
	      v44_encoder_flush(encoder) ==
		      V44_ENCODE_NEED_OUTPUT &&
	      drain_one(encoder, wire, sizeof(wire), &wire_length) &&
	      v44_encoder_process(
		      encoder, &blocked, &blocked_length) ==
		      V44_ENCODE_INVALID_STATE &&
	      blocked == plain && blocked_length == sizeof(plain) &&
	      v44_encoder_request_mode(
		      encoder, V44_ENCODER_COMPRESSED) ==
		      V44_ENCODE_INVALID_STATE &&
	      v44_encoder_flush(encoder) ==
		      V44_ENCODE_FLUSHED &&
	      v44_encoder_flush(encoder) ==
		      V44_ENCODE_FLUSHED &&
	      wire_length == 1 && wire[0] == 'X',
	      "transparent V.44 flush emitted a compressed control");
	v44_encoder_destroy(encoder);
	return true;
}

static bool test_progress_accounting(void)
{
	static const uint8_t compressed_a[] = { 'A' };
	static const uint8_t transparent_escape[] = { 0 };
	static const uint8_t transparent_t[] = { 'T' };
	static const uint8_t compressed_b[] = { 'B' };
	struct v44_encoder *encoder =
		v44_encoder_create(&parameters, 2);
	struct v44_encoder_progress unchanged = {
		.mode = V44_ENCODER_TRANSPARENT,
		.represented_octets = 91,
		.wire_bits = 73,
	};
	const uint8_t *input = compressed_a;
	size_t input_length = sizeof(compressed_a);
	uint8_t output = 0;

	CHECK(encoder != NULL &&
	      progress_is(
		      encoder, V44_ENCODER_COMPRESSED, 0, 0) &&
	      !v44_encoder_get_progress(NULL, &unchanged) &&
	      unchanged.mode == V44_ENCODER_TRANSPARENT &&
	      unchanged.represented_octets == 91 && unchanged.wire_bits == 73 &&
	      !v44_encoder_get_progress(encoder, NULL),
	      "fresh V.44 encoder progress was invalid");
	CHECK(v44_encoder_process(
		      encoder, &input, &input_length) ==
		      V44_ENCODE_OK &&
	      input_length == 0 &&
	      progress_is(
		      encoder, V44_ENCODER_COMPRESSED, 1, 8),
	      "queued V.44 ordinal was not reported as represented");
	CHECK(v44_encoder_request_mode(
		      encoder, V44_ENCODER_TRANSPARENT) ==
		      V44_ENCODE_NEED_OUTPUT &&
	      progress_is(
		      encoder, V44_ENCODER_COMPRESSED, 1, 8) &&
	      v44_encoder_read(encoder, &output, 1) == 1 &&
	      v44_encoder_request_mode(
		      encoder, V44_ENCODER_TRANSPARENT) ==
		      V44_ENCODE_NEED_OUTPUT &&
	      progress_is(
		      encoder, V44_ENCODER_COMPRESSED, 1, 16) &&
	      v44_encoder_read(encoder, &output, 1) == 1 &&
	      v44_encoder_request_mode(
		      encoder, V44_ENCODER_TRANSPARENT) ==
		      V44_ENCODE_OK &&
	      progress_is(
		      encoder, V44_ENCODER_TRANSPARENT, 1, 16),
	      "V.44 ETM progress duplicated data or missed padding");
	input = transparent_escape;
	input_length = sizeof(transparent_escape);
	CHECK(v44_encoder_process(
		      encoder, &input, &input_length) ==
		      V44_ENCODE_OK &&
	      progress_is(
		      encoder, V44_ENCODER_TRANSPARENT, 2, 32) &&
	      v44_encoder_read(encoder, &output, 1) == 1 &&
	      progress_is(
		      encoder, V44_ENCODER_TRANSPARENT, 2, 32) &&
	      v44_encoder_read(encoder, &output, 1) == 1,
	      "V.44 EID progress depended on output draining");
	input = transparent_t;
	input_length = sizeof(transparent_t);
	CHECK(v44_encoder_process(
		      encoder, &input, &input_length) ==
		      V44_ENCODE_OK &&
	      progress_is(
		      encoder, V44_ENCODER_TRANSPARENT, 3, 40) &&
	      v44_encoder_flush(encoder) ==
		      V44_ENCODE_NEED_OUTPUT &&
	      v44_encoder_read(encoder, &output, 1) == 1 &&
	      v44_encoder_flush(encoder) ==
		      V44_ENCODE_FLUSHED &&
	      progress_is(
		      encoder, V44_ENCODER_TRANSPARENT, 3, 40),
	      "transparent V.44 flush changed progress");
	CHECK(v44_encoder_request_mode(
		      encoder, V44_ENCODER_COMPRESSED) ==
		      V44_ENCODE_NEED_OUTPUT &&
	      progress_is(
		      encoder, V44_ENCODER_TRANSPARENT, 3, 56) &&
	      v44_encoder_read(encoder, &output, 1) == 1 &&
	      v44_encoder_request_mode(
		      encoder, V44_ENCODER_COMPRESSED) ==
		      V44_ENCODE_NEED_OUTPUT &&
	      progress_is(
		      encoder, V44_ENCODER_TRANSPARENT, 3, 56) &&
	      v44_encoder_read(encoder, &output, 1) == 1 &&
	      v44_encoder_request_mode(
		      encoder, V44_ENCODER_COMPRESSED) ==
		      V44_ENCODE_OK &&
	      progress_is(
		      encoder, V44_ENCODER_COMPRESSED, 3, 56),
	      "V.44 ECM progress changed across backpressure retries");
	input = compressed_b;
	input_length = sizeof(compressed_b);
	CHECK(v44_encoder_process(
		      encoder, &input, &input_length) ==
		      V44_ENCODE_OK &&
	      progress_is(
		      encoder, V44_ENCODER_COMPRESSED, 4, 64) &&
	      v44_encoder_flush(encoder) ==
		      V44_ENCODE_NEED_OUTPUT &&
	      progress_is(
		      encoder, V44_ENCODER_COMPRESSED, 4, 64) &&
	      v44_encoder_read(encoder, &output, 1) == 1 &&
	      v44_encoder_flush(encoder) ==
		      V44_ENCODE_NEED_OUTPUT &&
	      progress_is(
		      encoder, V44_ENCODER_COMPRESSED, 4, 72) &&
	      v44_encoder_read(encoder, &output, 1) == 1 &&
	      v44_encoder_flush(encoder) ==
		      V44_ENCODE_FLUSHED &&
	      progress_is(
		      encoder, V44_ENCODER_COMPRESSED, 4, 72),
	      "V.44 compressed FLUSH progress was not exact");
	v44_encoder_destroy(encoder);
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "exact mode round trip", test_exact_mode_round_trip },
		{ "escape cycle and transition backpressure",
		  test_escape_cycle_and_transition_backpressure },
		{ "transparent flush", test_transparent_flush },
		{ "progress accounting", test_progress_accounting },
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
