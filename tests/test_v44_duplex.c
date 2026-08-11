// SPDX-License-Identifier: BSD-3-Clause

#include <v44_duplex.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

static const struct sippymodem_v44_parameters minimum_parameters = {
	.codeword_limit = SIPPYMODEM_V44_MIN_CODEWORDS,
	.max_string_length = SIPPYMODEM_V44_MIN_STRING_LENGTH,
	.history_size = SIPPYMODEM_V44_MIN_HISTORY_SIZE,
};

static void generate_payload(uint8_t *payload, size_t length, uint32_t seed)
{
	static const uint8_t phrase[] = "instance-owned V.44 stream ";

	for (size_t index = 0; index < length; ++index) {
		seed = seed * UINT32_C(1664525) + UINT32_C(1013904223);
		payload[index] = index % 83 < 64 ?
				 phrase[index % (sizeof(phrase) - 1)] :
				 (uint8_t)(seed >> 24);
	}
}

static void generate_noise(uint8_t *payload, size_t length, uint32_t state)
{
	for (size_t index = 0; index < length; ++index) {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		payload[index] = (uint8_t)state;
	}
}

static bool transmit_segment(
	struct sippymodem_v44_duplex *duplex,
	const uint8_t *plain,
	size_t plain_length,
	uint8_t *wire,
	size_t wire_capacity,
	size_t *wire_length)
{
	const uint8_t *input = plain;
	size_t input_length = plain_length;
	size_t iterations = 0;

	while (input_length != 0) {
		enum sippymodem_v44_duplex_result result;
		size_t output_length = 0;

		if (*wire_length >= wire_capacity || ++iterations > 100000)
			return false;
		result = sippymodem_v44_duplex_transmit(
			duplex, &input, &input_length, wire + *wire_length, 1,
			&output_length);
		if (output_length > 1)
			return false;
		*wire_length += output_length;
		if (result == SIPPYMODEM_V44_DUPLEX_NEED_INPUT) {
			if (input_length != 0)
				return false;
		} else if (result != SIPPYMODEM_V44_DUPLEX_NEED_OUTPUT) {
			return false;
		}
	}
	for (;;) {
		enum sippymodem_v44_duplex_result result;
		size_t output_length = 0;

		if (*wire_length >= wire_capacity || ++iterations > 100000)
			return false;
		result = sippymodem_v44_duplex_flush_transmit(
			duplex, wire + *wire_length, 1, &output_length);
		if (output_length > 1)
			return false;
		*wire_length += output_length;
		if (result == SIPPYMODEM_V44_DUPLEX_FLUSHED)
			return true;
		if (result != SIPPYMODEM_V44_DUPLEX_NEED_OUTPUT)
			return false;
	}
}

static bool transmit_bytes(
	struct sippymodem_v44_duplex *duplex,
	const uint8_t *plain,
	size_t plain_length,
	uint8_t *wire,
	size_t wire_capacity,
	size_t *wire_length)
{
	const uint8_t *input = plain;
	size_t input_length = plain_length;

	for (size_t iterations = 0; input_length != 0; ++iterations) {
		enum sippymodem_v44_duplex_result result;
		size_t output_length = 0;

		if (*wire_length >= wire_capacity || iterations > 100000)
			return false;
		result = sippymodem_v44_duplex_transmit(
			duplex, &input, &input_length, wire + *wire_length, 1,
			&output_length);
		if (output_length > 1)
			return false;
		*wire_length += output_length;
		if (result == SIPPYMODEM_V44_DUPLEX_NEED_INPUT) {
			if (input_length != 0)
				return false;
		} else if (result != SIPPYMODEM_V44_DUPLEX_NEED_OUTPUT) {
			return false;
		}
	}
	return true;
}

static bool settle_transmit(
	struct sippymodem_v44_duplex *duplex,
	uint8_t *wire,
	size_t wire_capacity,
	size_t *wire_length)
{
	const uint8_t *input = NULL;
	size_t input_length = 0;

	for (size_t attempts = 0; attempts < 100000; ++attempts) {
		size_t output_length = 0;
		enum sippymodem_v44_duplex_result result;

		if (*wire_length >= wire_capacity)
			return false;
		result = sippymodem_v44_duplex_transmit(
			duplex, &input, &input_length, wire + *wire_length, 1,
			&output_length);
		if (output_length > 1)
			return false;
		*wire_length += output_length;
		if (result == SIPPYMODEM_V44_DUPLEX_NEED_INPUT)
			return true;
		if (result != SIPPYMODEM_V44_DUPLEX_NEED_OUTPUT)
			return false;
	}
	return false;
}

static bool transmit_until_mode(
	struct sippymodem_v44_duplex *duplex,
	const uint8_t *plain,
	size_t plain_length,
	enum sippymodem_v44_encoder_mode target,
	uint8_t *wire,
	size_t wire_capacity,
	size_t *wire_length,
	size_t *consumed)
{
	for (size_t index = 0; index < plain_length; ++index) {
		enum sippymodem_v44_encoder_mode mode;

		if (!transmit_bytes(
			    duplex, plain + index, 1, wire, wire_capacity,
			    wire_length) ||
		    !settle_transmit(
			    duplex, wire, wire_capacity, wire_length) ||
		    !sippymodem_v44_duplex_get_transmit_mode(duplex, &mode))
			return false;
		if (mode == target) {
			*consumed = index + 1;
			return true;
		}
	}
	return false;
}

static bool request_transmit_mode(
	struct sippymodem_v44_duplex *duplex,
	enum sippymodem_v44_encoder_mode target,
	uint8_t *wire,
	size_t wire_capacity,
	size_t *wire_length)
{
	for (size_t attempts = 0; attempts < 100000; ++attempts) {
		enum sippymodem_v44_duplex_result result;
		size_t output_length = 0;

		if (*wire_length >= wire_capacity)
			return false;
		result = sippymodem_v44_duplex_request_transmit_mode(
			duplex, target, wire + *wire_length, 1, &output_length);
		if (output_length > 1)
			return false;
		*wire_length += output_length;
		if (result == SIPPYMODEM_V44_DUPLEX_OK)
			return true;
		if (result != SIPPYMODEM_V44_DUPLEX_NEED_OUTPUT)
			return false;
	}
	return false;
}

static bool receive_segment(
	struct sippymodem_v44_duplex *duplex,
	const uint8_t *wire,
	size_t wire_length,
	uint8_t *plain,
	size_t plain_capacity,
	size_t *plain_length,
	bool finish);

static bool test_adaptive_transmit_modes(void)
{
	struct sippymodem_v44_duplex_config sender_config = {
		.transmit_enabled = true,
		.transmit_parameters = minimum_parameters,
		.transmit_adaptation_enabled = true,
		.transmit_adaptation = {
			.window_octets = SIPPYMODEM_V44_MIN_STRING_LENGTH,
			.leave_compressed_excess_bits = 0,
			.enter_compressed_savings_bits = 0,
		},
	};
	struct sippymodem_v44_duplex_config receiver_config = {
		.receive_enabled = true,
		.receive_parameters = minimum_parameters,
	};
	struct sippymodem_v44_duplex *sender =
		sippymodem_v44_duplex_create(&sender_config);
	struct sippymodem_v44_duplex *receiver =
		sippymodem_v44_duplex_create(&receiver_config);
	uint8_t noise[256];
	uint8_t repeated[256];
	uint8_t expected[sizeof(noise) + sizeof(repeated)];
	uint8_t decoded[sizeof(expected)];
	uint8_t wire[2048];
	struct sippymodem_v44_tx_monitor_sample sample = {
		.mode = SIPPYMODEM_V44_ENCODER_TRANSPARENT,
		.represented_octets = 17,
		.compressed_bits = 29,
		.decision = SIPPYMODEM_V44_TX_MONITOR_STAY,
	};
	enum sippymodem_v44_encoder_mode mode =
		SIPPYMODEM_V44_ENCODER_TRANSPARENT;
	size_t noise_length = 0;
	size_t repeated_length = 0;
	size_t expected_length;
	size_t decoded_length = 0;
	size_t wire_length = 0;

	CHECK(sender != NULL && receiver != NULL &&
	      sippymodem_v44_duplex_transmit_adaptation_active(sender) &&
	      sippymodem_v44_duplex_get_transmit_mode(sender, &mode) &&
	      mode == SIPPYMODEM_V44_ENCODER_COMPRESSED &&
	      !sippymodem_v44_duplex_get_last_transmit_sample(
		      sender, &sample) &&
	      sample.mode == SIPPYMODEM_V44_ENCODER_TRANSPARENT &&
	      sample.represented_octets == 17 && sample.compressed_bits == 29,
	      "could not create an adaptive V.44 transmit peer");
	generate_noise(noise, sizeof(noise), UINT32_C(0xc3a5c85c));
	memset(repeated, 'A', sizeof(repeated));
	CHECK(transmit_until_mode(
		      sender, noise, sizeof(noise),
		      SIPPYMODEM_V44_ENCODER_TRANSPARENT, wire, sizeof(wire),
		      &wire_length, &noise_length) &&
	      sippymodem_v44_duplex_get_last_transmit_sample(
		      sender, &sample) &&
	      sample.mode == SIPPYMODEM_V44_ENCODER_COMPRESSED &&
	      sample.decision ==
		      SIPPYMODEM_V44_TX_MONITOR_ENTER_TRANSPARENT,
	      "generated noise did not leave V.44 compressed mode");
	CHECK(transmit_until_mode(
		      sender, repeated, sizeof(repeated),
		      SIPPYMODEM_V44_ENCODER_COMPRESSED, wire, sizeof(wire),
		      &wire_length, &repeated_length) &&
	      sippymodem_v44_duplex_get_last_transmit_sample(
		      sender, &sample) &&
	      sample.mode == SIPPYMODEM_V44_ENCODER_TRANSPARENT &&
	      sample.decision ==
		      SIPPYMODEM_V44_TX_MONITOR_ENTER_COMPRESSED &&
	      sippymodem_v44_duplex_transmit_adaptation_active(sender),
	      "generated repetition did not re-enter V.44 compressed mode");
	CHECK(transmit_segment(
		      sender, NULL, 0, wire, sizeof(wire), &wire_length),
	      "could not finish the adaptive V.44 transmit stream");
	memcpy(expected, noise, noise_length);
	memcpy(expected + noise_length, repeated, repeated_length);
	expected_length = noise_length + repeated_length;
	CHECK(receive_segment(
		      receiver, wire, wire_length, decoded, sizeof(decoded),
		      &decoded_length, true) &&
	      decoded_length == expected_length &&
	      memcmp(decoded, expected, expected_length) == 0,
	      "adaptive V.44 mode changes did not round trip");
	sippymodem_v44_duplex_destroy(receiver);
	sippymodem_v44_duplex_destroy(sender);
	return true;
}

static bool test_adaptive_flush_decision(void)
{
	struct sippymodem_v44_duplex_config sender_config = {
		.transmit_enabled = true,
		.transmit_parameters = minimum_parameters,
		.transmit_adaptation_enabled = true,
		.transmit_adaptation = {
			.window_octets = SIPPYMODEM_V44_MIN_STRING_LENGTH,
			.leave_compressed_excess_bits = 0,
			.enter_compressed_savings_bits = 0,
		},
	};
	struct sippymodem_v44_duplex_config receiver_config = {
		.receive_enabled = true,
		.receive_parameters = minimum_parameters,
	};
	struct sippymodem_v44_duplex *sender =
		sippymodem_v44_duplex_create(&sender_config);
	struct sippymodem_v44_duplex *receiver =
		sippymodem_v44_duplex_create(&receiver_config);
	uint8_t noise[SIPPYMODEM_V44_MIN_STRING_LENGTH];
	static const uint8_t following = 'Z';
	uint8_t decoded[sizeof(noise)];
	uint8_t wire[512];
	struct sippymodem_v44_tx_monitor_sample sample;
	enum sippymodem_v44_encoder_mode mode;
	const uint8_t *blocked = &following;
	size_t blocked_length = 1;
	size_t decoded_length = 0;
	size_t first_wire_length;
	size_t output_length = 99;
	size_t wire_length = 0;

	generate_noise(noise, sizeof(noise), UINT32_C(0x9216ac43));
	/* Leave a known four-octet match pending until C-FLUSH. */
	memcpy(noise + sizeof(noise) - 4, noise, 4);
	CHECK(sender != NULL && receiver != NULL,
	      "could not create adaptive V.44 flush peers");
	CHECK(transmit_bytes(
		      sender, noise, sizeof(noise), wire, sizeof(wire),
		      &wire_length),
	      "could not feed the adaptive V.44 flush window");
	CHECK(!sippymodem_v44_duplex_get_last_transmit_sample(sender, &sample),
	      "adaptive V.44 sample completed before C-FLUSH");
	CHECK(transmit_segment(
		      sender, NULL, 0, wire, sizeof(wire), &wire_length) &&
	      sippymodem_v44_duplex_get_last_transmit_sample(
		      sender, &sample) &&
	      sample.mode == SIPPYMODEM_V44_ENCODER_COMPRESSED &&
	      sample.represented_octets == sizeof(noise) &&
	      sample.decision ==
		      SIPPYMODEM_V44_TX_MONITOR_ENTER_TRANSPARENT &&
	      sippymodem_v44_duplex_get_transmit_mode(sender, &mode) &&
	      mode == SIPPYMODEM_V44_ENCODER_COMPRESSED,
	      "C-FLUSH did not expose an adaptive V.44 decision");
	first_wire_length = wire_length;
	CHECK(sippymodem_v44_duplex_transmit(
		      sender, &blocked, &blocked_length, NULL, 0,
		      &output_length) == SIPPYMODEM_V44_DUPLEX_NEED_OUTPUT &&
	      output_length == 0 && blocked == &following &&
	      blocked_length == 1 &&
	      settle_transmit(sender, wire, sizeof(wire), &wire_length) &&
	      sippymodem_v44_duplex_get_transmit_mode(sender, &mode) &&
	      mode == SIPPYMODEM_V44_ENCODER_TRANSPARENT &&
	      transmit_bytes(
		      sender, &following, 1, wire, sizeof(wire), &wire_length) &&
	      request_transmit_mode(
		      sender, SIPPYMODEM_V44_ENCODER_COMPRESSED, wire,
		      sizeof(wire), &wire_length) &&
	      transmit_segment(
		      sender, NULL, 0, wire, sizeof(wire), &wire_length),
	      "adaptive V.44 flush did not serialize its next transition");
	CHECK(receive_segment(
		      receiver, wire, first_wire_length, decoded,
		      sizeof(decoded), &decoded_length, false) &&
	      decoded_length == sizeof(noise) &&
	      memcmp(decoded, noise, sizeof(noise)) == 0,
	      "pre-transition V.44 flush segment did not round trip");
	decoded_length = 0;
	CHECK(receive_segment(
		      receiver, wire + first_wire_length,
		      wire_length - first_wire_length, decoded, sizeof(decoded),
		      &decoded_length, true) &&
	      decoded_length == 1 && decoded[0] == following &&
	      sippymodem_v44_duplex_transmit_adaptation_active(sender),
	      "flush-triggered V.44 mode transition did not round trip");
	sippymodem_v44_duplex_destroy(receiver);
	sippymodem_v44_duplex_destroy(sender);
	return true;
}

static bool test_transmit_mode_handoff(void)
{
	static const uint8_t compressed_prefix[] = { 'A' };
	static const uint8_t transparent[] = { 0, 'T' };
	static const uint8_t compressed_suffix[] = "ABRACADABRA";
	struct sippymodem_v44_duplex_config sender_config = {
		.transmit_enabled = true,
		.transmit_parameters = minimum_parameters,
	};
	struct sippymodem_v44_duplex_config receiver_config = {
		.receive_enabled = true,
		.receive_parameters = minimum_parameters,
	};
	struct sippymodem_v44_duplex *sender =
		sippymodem_v44_duplex_create(&sender_config);
	struct sippymodem_v44_duplex *receiver =
		sippymodem_v44_duplex_create(&receiver_config);
	uint8_t wire[512];
	uint8_t expected[sizeof(compressed_prefix) + sizeof(transparent) +
			sizeof(compressed_suffix)];
	uint8_t decoded[sizeof(expected) + 1];
	static const uint8_t blocked_byte = 'B';
	const uint8_t *blocked = &blocked_byte;
	size_t blocked_length = 1;
	size_t wire_length = 0;
	size_t decoded_length = 0;
	size_t output_length = 99;

	CHECK(sender != NULL && receiver != NULL,
	      "could not create V.44 mode-handoff peers");
	CHECK(transmit_bytes(
		      sender, compressed_prefix, sizeof(compressed_prefix), wire,
		      sizeof(wire), &wire_length) &&
	      sippymodem_v44_duplex_request_transmit_mode(
		      sender, SIPPYMODEM_V44_ENCODER_TRANSPARENT, NULL, 0,
		      &output_length) == SIPPYMODEM_V44_DUPLEX_NEED_OUTPUT &&
	      output_length == 0 &&
	      sippymodem_v44_duplex_transmit(
		      sender, &blocked, &blocked_length, NULL, 0,
		      &output_length) == SIPPYMODEM_V44_DUPLEX_INVALID_STATE &&
	      blocked == &blocked_byte && blocked_length == 1 &&
	      sippymodem_v44_duplex_request_transmit_mode(
		      sender, SIPPYMODEM_V44_ENCODER_COMPRESSED, NULL, 0,
		      &output_length) == SIPPYMODEM_V44_DUPLEX_INVALID_STATE &&
	      sippymodem_v44_duplex_flush_transmit(
		      sender, NULL, 0, &output_length) ==
		      SIPPYMODEM_V44_DUPLEX_INVALID_STATE &&
	      request_transmit_mode(
		      sender, SIPPYMODEM_V44_ENCODER_TRANSPARENT, wire,
		      sizeof(wire), &wire_length),
	      "V.44 duplex did not serialize its ETM transition");
	CHECK(transmit_bytes(
		      sender, transparent, sizeof(transparent), wire,
		      sizeof(wire), &wire_length) &&
	      request_transmit_mode(
		      sender, SIPPYMODEM_V44_ENCODER_COMPRESSED, wire,
		      sizeof(wire), &wire_length) &&
	      transmit_segment(
		      sender, compressed_suffix, sizeof(compressed_suffix), wire,
		      sizeof(wire), &wire_length),
	      "V.44 duplex did not complete its ECM transition");
	memcpy(expected, compressed_prefix, sizeof(compressed_prefix));
	memcpy(expected + sizeof(compressed_prefix), transparent,
	       sizeof(transparent));
	memcpy(expected + sizeof(compressed_prefix) + sizeof(transparent),
	       compressed_suffix, sizeof(compressed_suffix));
	CHECK(receive_segment(
		      receiver, wire, wire_length, decoded, sizeof(decoded),
		      &decoded_length, true) &&
	      decoded_length == sizeof(expected) &&
	      memcmp(decoded, expected, sizeof(expected)) == 0,
	      "V.44 duplex mixed-mode stream did not round trip");
	output_length = 99;
	CHECK(sippymodem_v44_duplex_request_transmit_mode(
		      sender, SIPPYMODEM_V44_ENCODER_COMPRESSED, NULL, 0,
		      &output_length) == SIPPYMODEM_V44_DUPLEX_OK &&
	      output_length == 0 &&
	      sippymodem_v44_duplex_request_transmit_mode(
		      sender, (enum sippymodem_v44_encoder_mode)99, NULL, 0,
		      &output_length) ==
		      SIPPYMODEM_V44_DUPLEX_INVALID_ARGUMENT &&
	      sippymodem_v44_duplex_request_transmit_mode(
		      NULL, SIPPYMODEM_V44_ENCODER_COMPRESSED, NULL, 0,
		      &output_length) == SIPPYMODEM_V44_DUPLEX_INVALID_ARGUMENT,
	      "V.44 duplex mode API validation changed state");
	output_length = 99;
	CHECK(sippymodem_v44_duplex_request_transmit_mode(
		      NULL, SIPPYMODEM_V44_ENCODER_COMPRESSED, NULL, 0,
		      &output_length) == SIPPYMODEM_V44_DUPLEX_INVALID_ARGUMENT &&
	      output_length == 99,
	      "invalid V.44 mode request changed caller output state");
	sippymodem_v44_duplex_destroy(receiver);
	sippymodem_v44_duplex_destroy(sender);
	return true;
}

static bool receive_segment(
	struct sippymodem_v44_duplex *duplex,
	const uint8_t *wire,
	size_t wire_length,
	uint8_t *plain,
	size_t plain_capacity,
	size_t *plain_length,
	bool finish)
{
	const uint8_t *input = wire;
	size_t input_length = wire_length;
	size_t iterations = 0;

	for (;;) {
		enum sippymodem_v44_duplex_result result;
		size_t output_length = 0;

		if (*plain_length >= plain_capacity || ++iterations > 100000)
			return false;
		result = sippymodem_v44_duplex_receive(
			duplex, &input, &input_length, plain + *plain_length, 1,
			&output_length);
		if (output_length > 1)
			return false;
		*plain_length += output_length;
		if (result == SIPPYMODEM_V44_DUPLEX_FLUSHED)
			break;
		if (result != SIPPYMODEM_V44_DUPLEX_NEED_OUTPUT)
			return false;
	}
	return input_length == 0 &&
	       (!finish ||
		sippymodem_v44_duplex_finish_receive(duplex) ==
			SIPPYMODEM_V44_DUPLEX_OK);
}

static bool test_asymmetric_duplex_round_trip(void)
{
	struct sippymodem_v44_duplex_config a_config = {
		.transmit_enabled = true,
		.transmit_parameters = sippymodem_v44_default_parameters(),
		.receive_enabled = true,
		.receive_parameters = minimum_parameters,
	};
	struct sippymodem_v44_duplex_config b_config = {
		.transmit_enabled = true,
		.transmit_parameters = minimum_parameters,
		.receive_enabled = true,
		.receive_parameters = sippymodem_v44_default_parameters(),
	};
	struct sippymodem_v44_duplex *a =
		sippymodem_v44_duplex_create(&a_config);
	struct sippymodem_v44_duplex *b =
		sippymodem_v44_duplex_create(&b_config);
	uint8_t a_plain[521];
	uint8_t b_plain[367];
	uint8_t a_wire[2048];
	uint8_t b_wire[2048];
	uint8_t decoded[sizeof(a_plain)];
	size_t a_wire_length = 0;
	size_t b_wire_length = 0;
	size_t decoded_length = 0;

	CHECK(a != NULL && b != NULL, "could not create V.44 peers");
	generate_payload(a_plain, sizeof(a_plain), UINT32_C(0x12345678));
	generate_payload(b_plain, sizeof(b_plain), UINT32_C(0x87654321));
	CHECK(transmit_segment(
		      a, a_plain, 241, a_wire, sizeof(a_wire),
		      &a_wire_length) &&
	      receive_segment(
		      b, a_wire, a_wire_length, decoded, sizeof(decoded),
		      &decoded_length, false) &&
	      decoded_length == 241 && memcmp(decoded, a_plain, 241) == 0,
	      "first V.44 stream segment did not round trip");
	a_wire_length = 0;
	decoded_length = 0;
	CHECK(transmit_segment(
		      a, a_plain + 241, sizeof(a_plain) - 241, a_wire,
		      sizeof(a_wire), &a_wire_length) &&
	      receive_segment(
		      b, a_wire, a_wire_length, decoded, sizeof(decoded),
		      &decoded_length, true) &&
	      decoded_length == sizeof(a_plain) - 241 &&
	      memcmp(decoded, a_plain + 241, decoded_length) == 0,
	      "successive V.44 stream segment did not round trip");
	decoded_length = 0;
	CHECK(transmit_segment(
		      b, b_plain, sizeof(b_plain), b_wire, sizeof(b_wire),
		      &b_wire_length) &&
	      receive_segment(
		      a, b_wire, b_wire_length, decoded, sizeof(decoded),
		      &decoded_length, true) &&
	      decoded_length == sizeof(b_plain) &&
	      memcmp(decoded, b_plain, sizeof(b_plain)) == 0,
	      "minimum transmit parameters did not round trip");
	/* Finishing receive in each peer must not disturb its transmitter. */
	{
		const uint8_t *empty = NULL;
		size_t empty_length = 0;
		size_t output_length = 0;

		CHECK(sippymodem_v44_duplex_receive(
			      a, &empty, &empty_length, NULL, 0,
			      &output_length) ==
			      SIPPYMODEM_V44_DUPLEX_INVALID_STATE &&
		      sippymodem_v44_duplex_receive(
			      b, &empty, &empty_length, NULL, 0,
			      &output_length) ==
			      SIPPYMODEM_V44_DUPLEX_INVALID_STATE,
		      "finished receive direction was not isolated");
	}
	sippymodem_v44_duplex_destroy(b);
	sippymodem_v44_duplex_destroy(a);
	return true;
}

static bool test_transmit_backpressure_and_flush_state(void)
{
	struct sippymodem_v44_duplex_config config = {
		.transmit_enabled = true,
		.transmit_parameters = minimum_parameters,
	};
	struct sippymodem_v44_duplex *duplex =
		sippymodem_v44_duplex_create(&config);
	uint8_t plain[64];
	uint8_t wire[512];
	const uint8_t *input = plain;
	size_t input_length = sizeof(plain);
	size_t output_length = 99;
	const uint8_t *blocked_input;
	size_t blocked_length;

	CHECK(duplex != NULL, "could not create V.44 backpressure peer");
	generate_payload(plain, sizeof(plain), UINT32_C(0xc001d00d));
	CHECK(sippymodem_v44_duplex_transmit(
		      duplex, &input, &input_length, NULL, 0,
		      &output_length) == SIPPYMODEM_V44_DUPLEX_NEED_OUTPUT &&
	      output_length == 0 && input > plain && input_length != 0,
	      "zero-capacity transmit did not reach bounded backpressure");
	blocked_input = input;
	blocked_length = input_length;
	CHECK(sippymodem_v44_duplex_transmit(
		      duplex, &input, &input_length, NULL, 0,
		      &output_length) == SIPPYMODEM_V44_DUPLEX_NEED_OUTPUT &&
	      input == blocked_input && input_length == blocked_length,
	      "transmit consumed input while encoded output was blocked");
	CHECK(sippymodem_v44_duplex_flush_transmit(
		      duplex, NULL, 0, &output_length) ==
		      SIPPYMODEM_V44_DUPLEX_NEED_OUTPUT &&
	      sippymodem_v44_duplex_transmit(
		      duplex, &input, &input_length, wire, sizeof(wire),
		      &output_length) == SIPPYMODEM_V44_DUPLEX_INVALID_STATE &&
	      input == blocked_input && input_length == blocked_length,
	      "transmit was not excluded during an incomplete flush");
	for (size_t attempts = 0;; ++attempts) {
		enum sippymodem_v44_duplex_result result;

		CHECK(attempts < sizeof(wire), "V.44 flush did not terminate");
		result = sippymodem_v44_duplex_flush_transmit(
			duplex, wire, 1, &output_length);
		CHECK(output_length <= 1,
		      "V.44 flush exceeded caller output capacity");
		if (result == SIPPYMODEM_V44_DUPLEX_FLUSHED)
			break;
		CHECK(result == SIPPYMODEM_V44_DUPLEX_NEED_OUTPUT,
		      "V.44 flush returned an unexpected result");
	}
	{
		enum sippymodem_v44_duplex_result result =
			sippymodem_v44_duplex_transmit(
				duplex, &input, &input_length, wire, sizeof(wire),
				&output_length);

		CHECK(result == SIPPYMODEM_V44_DUPLEX_NEED_INPUT ||
		      result == SIPPYMODEM_V44_DUPLEX_NEED_OUTPUT,
		      "completed flush did not restore transmit readiness");
	}
	sippymodem_v44_duplex_destroy(duplex);
	return true;
}

static bool test_literal_bypass_and_api(void)
{
	static const uint8_t plain[] = { 1, 2, 3, 4, 5 };
	static const struct {
		enum sippymodem_v44_duplex_result result;
		const char *name;
	} names[] = {
		{ SIPPYMODEM_V44_DUPLEX_OK, "ok" },
		{ SIPPYMODEM_V44_DUPLEX_NEED_INPUT, "need input" },
		{ SIPPYMODEM_V44_DUPLEX_NEED_OUTPUT, "need output" },
		{ SIPPYMODEM_V44_DUPLEX_FLUSHED, "flushed" },
		{ SIPPYMODEM_V44_DUPLEX_ENTER_PARAMETER, "enter parameter" },
		{ SIPPYMODEM_V44_DUPLEX_INVALID_ARGUMENT, "invalid argument" },
		{ SIPPYMODEM_V44_DUPLEX_INVALID_STATE, "invalid state" },
		{ SIPPYMODEM_V44_DUPLEX_PROCEDURAL_ERROR,
		  "procedural error" },
	};
	struct sippymodem_v44_duplex_config config = { 0 };
	struct sippymodem_v44_duplex_config adaptive_without_transmit = {
		.transmit_adaptation_enabled = true,
		.transmit_adaptation = {
			.window_octets =
				SIPPYMODEM_V44_TX_MONITOR_DEFAULT_WINDOW_OCTETS,
			.leave_compressed_excess_bits =
				SIPPYMODEM_V44_TX_MONITOR_DEFAULT_LEAVE_EXCESS_BITS,
			.enter_compressed_savings_bits =
				SIPPYMODEM_V44_TX_MONITOR_DEFAULT_ENTER_SAVINGS_BITS,
		},
	};
	struct sippymodem_v44_duplex_config invalid_adaptation = {
		.transmit_enabled = true,
		.transmit_parameters = minimum_parameters,
		.transmit_adaptation_enabled = true,
		.transmit_adaptation = {
			.window_octets =
				SIPPYMODEM_V44_MIN_STRING_LENGTH - 1,
		},
	};
	struct sippymodem_v44_duplex *duplex =
		sippymodem_v44_duplex_create(&config);
	const uint8_t *input = plain;
	size_t input_length = sizeof(plain);
	size_t output_length = 99;
	uint8_t output[sizeof(plain)] = { 0 };
	enum sippymodem_v44_encoder_mode mode =
		SIPPYMODEM_V44_ENCODER_TRANSPARENT;

	CHECK(duplex != NULL && sippymodem_v44_duplex_create(NULL) == NULL,
	      "disabled or NULL V.44 configuration was mishandled");
	CHECK(sippymodem_v44_duplex_create(&adaptive_without_transmit) == NULL &&
	      sippymodem_v44_duplex_create(&invalid_adaptation) == NULL,
	      "invalid V.44 transmit adaptation was accepted");
	CHECK(!sippymodem_v44_duplex_get_transmit_mode(duplex, &mode) &&
	      mode == SIPPYMODEM_V44_ENCODER_TRANSPARENT &&
	      !sippymodem_v44_duplex_get_transmit_mode(NULL, &mode) &&
	      !sippymodem_v44_duplex_transmit_adaptation_active(duplex) &&
	      !sippymodem_v44_duplex_get_last_transmit_sample(
		      duplex, NULL),
	      "disabled V.44 transmit exposed adaptive diagnostics");
	config.transmit_enabled = true;
	CHECK(sippymodem_v44_duplex_create(&config) == NULL,
	      "invalid enabled V.44 parameters were accepted");
	CHECK(sippymodem_v44_duplex_transmit(
		      duplex, &input, &input_length, output, 2,
		      &output_length) == SIPPYMODEM_V44_DUPLEX_NEED_OUTPUT &&
	      output_length == 2 && input_length == 3 &&
	      sippymodem_v44_duplex_transmit(
		      duplex, &input, &input_length, output + 2, 3,
		      &output_length) == SIPPYMODEM_V44_DUPLEX_NEED_INPUT &&
	      output_length == 3 && input_length == 0 &&
	      memcmp(output, plain, sizeof(plain)) == 0,
	      "disabled transmit did not copy literal bytes");
	CHECK(sippymodem_v44_duplex_flush_transmit(
		      duplex, NULL, 0, &output_length) ==
		      SIPPYMODEM_V44_DUPLEX_FLUSHED && output_length == 0,
	      "disabled transmit emitted a flush command");
	CHECK(sippymodem_v44_duplex_request_transmit_mode(
		      duplex, SIPPYMODEM_V44_ENCODER_TRANSPARENT, NULL, 0,
		      &output_length) == SIPPYMODEM_V44_DUPLEX_INVALID_STATE &&
	      output_length == 0,
	      "disabled transmit acquired a V.44 stream mode");
	input = plain;
	input_length = sizeof(plain);
	memset(output, 0, sizeof(output));
	CHECK(sippymodem_v44_duplex_receive(
		      duplex, &input, &input_length, output, sizeof(output),
		      &output_length) == SIPPYMODEM_V44_DUPLEX_NEED_INPUT &&
	      output_length == sizeof(plain) && input_length == 0 &&
	      memcmp(output, plain, sizeof(plain)) == 0 &&
	      sippymodem_v44_duplex_finish_receive(duplex) ==
		      SIPPYMODEM_V44_DUPLEX_OK &&
	      sippymodem_v44_duplex_receive(
		      duplex, &input, &input_length, NULL, 0,
		      &output_length) == SIPPYMODEM_V44_DUPLEX_INVALID_STATE,
	      "disabled receive lifecycle changed literal data");
	output_length = 99;
	CHECK(sippymodem_v44_duplex_transmit(
		      duplex, &input, &input_length, NULL, 1,
		      &output_length) == SIPPYMODEM_V44_DUPLEX_INVALID_ARGUMENT &&
	      output_length == 99 &&
	      sippymodem_v44_duplex_flush_transmit(NULL, NULL, 0,
						    &output_length) ==
		      SIPPYMODEM_V44_DUPLEX_INVALID_ARGUMENT &&
	      sippymodem_v44_duplex_finish_receive(NULL) ==
		      SIPPYMODEM_V44_DUPLEX_INVALID_ARGUMENT &&
	      sippymodem_v44_duplex_resume_receive_transparent(NULL) ==
		      SIPPYMODEM_V44_DUPLEX_INVALID_ARGUMENT,
	      "V.44 duplex accepted invalid API arguments");
	for (size_t index = 0; index < ARRAY_LENGTH(names); ++index)
		CHECK(strcmp(
			      sippymodem_v44_duplex_result_name(
				      names[index].result),
			      names[index].name) == 0,
		      "V.44 duplex result name changed");
	CHECK(strcmp(
		      sippymodem_v44_duplex_result_name(
			      (enum sippymodem_v44_duplex_result)99),
		      "unknown") == 0,
	      "unknown V.44 duplex result name changed");
	sippymodem_v44_duplex_destroy(duplex);
	sippymodem_v44_duplex_destroy(NULL);
	return true;
}

static bool test_parameter_handoff_and_failure_isolation(void)
{
	static const uint8_t queued_literals[] = {
		0x82, 0x84, /* Compressed A, B. */
	};
	static const uint8_t wire[] = {
		0x01, 0x00, 0x02, 0xaa, 0xbb, 'P',
	};
	static const uint8_t dangling_escape[] = { 0x00 };
	static const uint8_t transmit_byte[] = { 'T' };
	struct sippymodem_v44_duplex_config config = {
		.receive_enabled = true,
		.receive_parameters = minimum_parameters,
	};
	struct sippymodem_v44_duplex *duplex =
		sippymodem_v44_duplex_create(&config);
	const uint8_t *input = wire;
	size_t input_length = sizeof(wire);
	size_t output_length = 0;
	uint8_t output = 0;

	CHECK(duplex != NULL, "could not create V.44 receive peer");
	input = queued_literals;
	input_length = sizeof(queued_literals);
	CHECK(sippymodem_v44_duplex_receive(
		      duplex, &input, &input_length, NULL, 0,
		      &output_length) == SIPPYMODEM_V44_DUPLEX_NEED_OUTPUT &&
	      input == queued_literals + 1 && input_length == 1 &&
	      output_length == 0 &&
	      sippymodem_v44_duplex_finish_receive(duplex) ==
		      SIPPYMODEM_V44_DUPLEX_NEED_OUTPUT &&
	      sippymodem_v44_duplex_receive(
		      duplex, &input, &input_length, NULL, 0,
		      &output_length) == SIPPYMODEM_V44_DUPLEX_NEED_OUTPUT &&
	      input == queued_literals + 1 && input_length == 1 &&
	      sippymodem_v44_duplex_receive(
		      duplex, &input, &input_length, &output, 1,
		      &output_length) == SIPPYMODEM_V44_DUPLEX_NEED_OUTPUT &&
	      output_length == 1 && output == 'A' && input_length == 0 &&
	      sippymodem_v44_duplex_finish_receive(duplex) ==
		      SIPPYMODEM_V44_DUPLEX_NEED_OUTPUT &&
	      sippymodem_v44_duplex_receive(
		      duplex, &input, &input_length, &output, 1,
		      &output_length) == SIPPYMODEM_V44_DUPLEX_NEED_INPUT &&
	      output_length == 1 && output == 'B' &&
	      sippymodem_v44_duplex_finish_receive(duplex) ==
		      SIPPYMODEM_V44_DUPLEX_OK,
	      "receive finish did not preserve queued output");
	sippymodem_v44_duplex_destroy(duplex);
	duplex = sippymodem_v44_duplex_create(&config);
	CHECK(duplex != NULL, "could not recreate V.44 receive peer");
	input = wire;
	input_length = sizeof(wire);
	CHECK(sippymodem_v44_duplex_receive(
		      duplex, &input, &input_length, &output, 1,
		      &output_length) == SIPPYMODEM_V44_DUPLEX_ENTER_PARAMETER &&
	      output_length == 0 && input == wire + 3 && input_length == 3,
	      "V.44 parameter event did not preserve following input");
	{
		const uint8_t *held_input = input;

		CHECK(sippymodem_v44_duplex_receive(
			      duplex, &input, &input_length, &output, 1,
			      &output_length) ==
			      SIPPYMODEM_V44_DUPLEX_ENTER_PARAMETER &&
		      input == held_input && input_length == 3 &&
		      sippymodem_v44_duplex_finish_receive(duplex) ==
			      SIPPYMODEM_V44_DUPLEX_ENTER_PARAMETER,
		      "V.44 parameter handoff was not latched");
	}
	input += 2;
	input_length -= 2;
	CHECK(sippymodem_v44_duplex_resume_receive_transparent(duplex) ==
		      SIPPYMODEM_V44_DUPLEX_OK &&
	      sippymodem_v44_duplex_receive(
		      duplex, &input, &input_length, &output, 1,
		      &output_length) == SIPPYMODEM_V44_DUPLEX_NEED_INPUT &&
	      output_length == 1 && output == 'P' && input_length == 0 &&
	      sippymodem_v44_duplex_resume_receive_transparent(duplex) ==
		      SIPPYMODEM_V44_DUPLEX_INVALID_STATE,
	      "V.44 receive did not resume after parameter End");
	input = dangling_escape;
	input_length = sizeof(dangling_escape);
	CHECK(sippymodem_v44_duplex_receive(
		      duplex, &input, &input_length, &output, 1,
		      &output_length) == SIPPYMODEM_V44_DUPLEX_NEED_INPUT &&
	      input_length == 0 &&
	      sippymodem_v44_duplex_finish_receive(duplex) ==
		      SIPPYMODEM_V44_DUPLEX_PROCEDURAL_ERROR &&
	      sippymodem_v44_duplex_receive(
		      duplex, &input, &input_length, &output, 1,
		      &output_length) == SIPPYMODEM_V44_DUPLEX_PROCEDURAL_ERROR,
	      "truncated V.44 receive did not fail closed");
	input = transmit_byte;
	input_length = sizeof(transmit_byte);
	CHECK(sippymodem_v44_duplex_transmit(
		      duplex, &input, &input_length, &output, 1,
		      &output_length) == SIPPYMODEM_V44_DUPLEX_NEED_INPUT &&
	      output_length == 1 && output == 'T',
	      "receive failure contaminated the transmit direction");
	sippymodem_v44_duplex_destroy(duplex);
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "asymmetric duplex round trip",
		  test_asymmetric_duplex_round_trip },
		{ "adaptive transmit modes", test_adaptive_transmit_modes },
		{ "adaptive flush decision", test_adaptive_flush_decision },
		{ "transmit mode handoff", test_transmit_mode_handoff },
		{ "transmit backpressure and flush state",
		  test_transmit_backpressure_and_flush_state },
		{ "literal bypass and API", test_literal_bypass_and_api },
		{ "parameter handoff and failure isolation",
		  test_parameter_handoff_and_failure_isolation },
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
