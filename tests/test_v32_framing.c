// SPDX-License-Identifier: BSD-3-Clause

#include <framing.h>

#include "test.h"

#include <string.h>

enum {
	TEST_STREAM_SYMBOLS = 2048,
	TEST_RESYNC_DIBITS = 96,
	TEST_TRN_DIBITS = 257,
	TEST_RATE_SEQUENCE_DIBITS = 64,
};

static uint32_t fixture_random(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static bool test_descrambler_polynomials(void)
{
	static const struct {
		enum v32_transmitter_role role;
		unsigned int role_tap;
	} fixtures[] = {
		{ V32_CALLING_TRANSMITTER, 18 },
		{ V32_ANSWERING_TRANSMITTER, 5 },
	};

	for (size_t fixture = 0; fixture < ARRAY_LENGTH(fixtures); ++fixture) {
		struct v32_scrambler descrambler;

		CHECK(v32_scrambler_init(
			      &descrambler, fixtures[fixture].role) ==
			      V32_FRAMING_OK,
		      "V.32 descrambler fixture did not initialize");
		for (unsigned int bit = 0; bit < 24; bit += 2) {
			uint8_t decoded;
			uint8_t received = bit == 0 ? 2u : 0u;
			uint8_t expected = 0;

			for (unsigned int offset = 0; offset < 2; ++offset) {
				unsigned int position = bit + offset;

				expected = (uint8_t)(expected << 1);
				if (position == 0 ||
				    position == fixtures[fixture].role_tap ||
				    position == V32_SCRAMBLER_BITS)
					expected |= 1u;
			}
			CHECK(v32_descramble_dibit(
				      &descrambler, received, &decoded) ==
				      V32_FRAMING_OK &&
				      decoded == expected,
			      "V.32 descrambler polynomial changed at bit %u",
			      bit);
		}
	}
	return true;
}

static bool test_normative_scrambler_starts(void)
{
	static const uint8_t trn_quadrant_by_dibit[] = { 0, 1, 3, 2 };
	static const uint8_t expected[][15] = {
		{ 3, 3, 3, 3, 3, 3, 3, 3, 3, 0, 0, 1, 3, 3, 3 },
		{ 3, 3, 2, 0, 0, 3, 3, 2, 0, 0, 3, 2, 1, 3, 3 },
	};
	static const uint8_t expected_9600[][7] = {
		{ 15, 11, 7, 3, 12, 1, 15 },
		{ 15, 4, 11, 6, 8, 6, 7 },
	};

	for (size_t role = 0; role < ARRAY_LENGTH(expected); ++role) {
		struct v32_symbol_encoder encoder;
		struct v32_symbol_encoder trn_encoder;
		struct v32_symbol_encoder encoder_9600;
		uint8_t previous = 0;

		CHECK(v32_symbol_encoder_init(
			      &encoder,
			      (enum v32_transmitter_role)role, 0) ==
			      V32_FRAMING_OK &&
		      v32_symbol_encoder_init(
			      &trn_encoder,
			      (enum v32_transmitter_role)role,
			      (uint8_t)(role + 1)) ==
			      V32_FRAMING_OK &&
		      v32_symbol_encoder_init(
			      &encoder_9600,
			      (enum v32_transmitter_role)role, 0) ==
			      V32_FRAMING_OK,
		      "V.32 normative encoder did not initialize");
		for (size_t index = 0; index < ARRAY_LENGTH(expected[role]);
		     ++index) {
			uint8_t quadrant;
			uint8_t scrambled;
			uint8_t trn_quadrant;
			uint8_t expected_trn_quadrant =
				trn_quadrant_by_dibit[expected[role][index]];

			CHECK(v32_4800_symbol_encode(
				      &encoder, 3, &quadrant) ==
				      V32_FRAMING_OK &&
			      v32_trn_symbol_encode(
				      &trn_encoder, 3, &trn_quadrant) ==
				      V32_FRAMING_OK &&
				      differential_qpsk_decode(
					      previous, quadrant, &scrambled) ==
					      DIFFERENTIAL_QPSK_OK &&
				      scrambled == expected[role][index] &&
				      trn_quadrant == expected_trn_quadrant,
			      "V.32 normative start changed at %zu/%zu", role,
			      index);
			previous = quadrant;
		}
		for (size_t index = 0;
		     index < ARRAY_LENGTH(expected_9600[role]); ++index) {
			uint8_t symbol;

			CHECK(v32_9600_symbol_encode(
				      &encoder_9600, 15, &symbol) ==
				      V32_FRAMING_OK &&
				      symbol == expected_9600[role][index],
			      "V.32 9600 normative start changed at %zu/%zu",
			      role, index);
		}
	}
	return true;
}

static bool test_symbol_round_trip(void)
{
	uint32_t random = UINT32_C(0x32c411a7);

	for (unsigned int role = 0; role < 2; ++role) {
		for (uint8_t initial = 0;
		     initial < QPSK_QUADRANT_COUNT; ++initial) {
			struct v32_symbol_encoder encoder;
			struct v32_symbol_decoder decoder;

			CHECK(v32_symbol_encoder_init(
				      &encoder,
				      (enum v32_transmitter_role)role,
				      initial) == V32_FRAMING_OK &&
				      v32_symbol_decoder_init(
					      &decoder,
					      (enum v32_transmitter_role)role,
					      initial) == V32_FRAMING_OK,
			      "V.32 symbol endpoints did not initialize");
			for (size_t index = 0; index < TEST_STREAM_SYMBOLS;
			     ++index) {
				uint8_t input =
					(uint8_t)(fixture_random(&random) & 3u);
				uint8_t output;
				uint8_t quadrant;

				CHECK(v32_4800_symbol_encode(
					      &encoder, input, &quadrant) ==
					      V32_FRAMING_OK &&
				      v32_4800_symbol_decode(
					      &decoder, quadrant, &output) ==
					      V32_FRAMING_OK &&
				      output == input,
				      "V.32 symbol round trip changed at %u/%u/%zu",
				      role, initial, index);
			}
			CHECK(encoder.phase_quadrant == decoder.phase_quadrant &&
				      encoder.scrambler.history ==
					      decoder.descrambler.history,
			      "V.32 symbol endpoints diverged");
		}
	}
	return true;
}

static bool test_trn_round_trip_and_transition(void)
{
	uint32_t random = UINT32_C(0x6d8eb713);

	for (unsigned int role = 0; role < 2; ++role) {
		for (uint8_t initial = 0;
		     initial < QPSK_QUADRANT_COUNT; ++initial) {
			struct v32_symbol_encoder encoder;
			struct v32_symbol_decoder decoder;

			CHECK(v32_symbol_encoder_init(
				      &encoder,
				      (enum v32_transmitter_role)role,
				      initial) == V32_FRAMING_OK &&
			      v32_symbol_decoder_init(
				      &decoder,
				      (enum v32_transmitter_role)role,
				      initial) == V32_FRAMING_OK,
			      "V.32 TRN endpoints did not initialize");
			for (size_t index = 0; index < TEST_TRN_DIBITS; ++index) {
				uint8_t input =
					(uint8_t)(fixture_random(&random) & 3u);
				uint8_t output;
				uint8_t quadrant;

				CHECK(v32_trn_symbol_encode(
					      &encoder, input, &quadrant) ==
					      V32_FRAMING_OK &&
				      v32_trn_symbol_decode(
					      &decoder, quadrant, &output) ==
					      V32_FRAMING_OK &&
				      output == input,
				      "V.32 TRN round trip changed at %u/%u/%zu",
				      role, initial, index);
			}
			for (size_t index = 0; index < TEST_STREAM_SYMBOLS;
			     ++index) {
				uint8_t input =
					(uint8_t)(fixture_random(&random) & 3u);
				uint8_t output;
				uint8_t quadrant;

				CHECK(v32_4800_symbol_encode(
					      &encoder, input, &quadrant) ==
					      V32_FRAMING_OK &&
				      v32_4800_symbol_decode(
					      &decoder, quadrant, &output) ==
					      V32_FRAMING_OK &&
				      output == input,
				      "V.32 TRN transition changed at %u/%u/%zu",
				      role, initial, index);
			}
			CHECK(encoder.phase_quadrant == decoder.phase_quadrant &&
				      encoder.scrambler.history ==
					      decoder.descrambler.history,
			      "V.32 TRN endpoints diverged");
		}
	}
	return true;
}

static bool test_9600_round_trip_and_transition(void)
{
	uint32_t random = UINT32_C(0xc9600321);

	for (unsigned int role = 0; role < 2; ++role) {
		for (uint8_t initial = 0;
		     initial < QPSK_QUADRANT_COUNT; ++initial) {
			struct v32_symbol_encoder encoder;
			struct v32_symbol_decoder decoder;

			CHECK(v32_symbol_encoder_init(
				      &encoder,
				      (enum v32_transmitter_role)role,
				      initial) == V32_FRAMING_OK &&
			      v32_symbol_decoder_init(
				      &decoder,
				      (enum v32_transmitter_role)role,
				      initial) == V32_FRAMING_OK,
			      "V.32 9600 endpoints did not initialize");
			for (size_t index = 0; index < TEST_RATE_SEQUENCE_DIBITS;
			     ++index) {
				uint8_t input =
					(uint8_t)(fixture_random(&random) & 3u);
				uint8_t output;
				uint8_t quadrant;

				CHECK(v32_4800_symbol_encode(
					      &encoder, input, &quadrant) ==
					      V32_FRAMING_OK &&
				      v32_4800_symbol_decode(
					      &decoder, quadrant, &output) ==
					      V32_FRAMING_OK &&
				      output == input,
				      "V.32 9600 rate transition changed at %u/%u/%zu",
				      role, initial, index);
			}
			for (size_t index = 0; index < TEST_STREAM_SYMBOLS; ++index) {
				uint8_t input =
					(uint8_t)(fixture_random(&random) & 15u);
				uint8_t output;
				uint8_t symbol;

				CHECK(v32_9600_symbol_encode(
					      &encoder, input, &symbol) ==
					      V32_FRAMING_OK &&
				      symbol < V32_9600_SYMBOL_COUNT &&
				      v32_9600_symbol_decode(
					      &decoder, symbol, &output) ==
					      V32_FRAMING_OK &&
				      output == input,
				      "V.32 9600 round trip changed at %u/%u/%zu",
				      role, initial, index);
			}
			CHECK(encoder.phase_quadrant == decoder.phase_quadrant &&
				      encoder.scrambler.history ==
					      decoder.descrambler.history,
			      "V.32 9600 endpoints diverged");
		}
	}
	return true;
}

static bool test_descrambler_resynchronization(void)
{
	struct v32_scrambler encoder;
	struct v32_scrambler decoder;
	uint32_t random = UINT32_C(0xb394e185);

	CHECK(v32_scrambler_init(
		      &encoder, V32_CALLING_TRANSMITTER) ==
		      V32_FRAMING_OK &&
		      v32_scrambler_init(
			      &decoder, V32_CALLING_TRANSMITTER) ==
			      V32_FRAMING_OK,
	      "V.32 resynchronization endpoints did not initialize");
	decoder.history = UINT32_C(0x5a4c31);
	for (size_t index = 0; index < TEST_RESYNC_DIBITS; ++index) {
		uint8_t input = (uint8_t)(fixture_random(&random) & 3u);
		uint8_t encoded;
		uint8_t output;

		CHECK(v32_scramble_dibit(
			      &encoder, input, &encoded) ==
			      V32_FRAMING_OK &&
			      v32_descramble_dibit(
				      &decoder, encoded, &output) ==
				      V32_FRAMING_OK &&
			      (index < 12 || output == input),
		      "V.32 descrambler did not synchronize at %zu", index);
	}
	CHECK(encoder.history == decoder.history,
	      "V.32 descrambler history did not converge");
	return true;
}

static bool test_three_bit_group_round_trip(void)
{
	uint32_t random = UINT32_C(0x72003211);

	for (unsigned int role = 0; role < 2; ++role) {
		struct v32_scrambler encoder;
		struct v32_scrambler decoder;

		CHECK(v32_scrambler_init(
			      &encoder,
			      (enum v32_transmitter_role)role) ==
			      V32_FRAMING_OK &&
		      v32_scrambler_init(
			      &decoder,
			      (enum v32_transmitter_role)role) ==
			      V32_FRAMING_OK,
		      "V.32 three-bit framing endpoints did not initialize");
		for (size_t index = 0; index < TEST_STREAM_SYMBOLS; ++index) {
			uint8_t input =
				(uint8_t)(fixture_random(&random) & 7u);
			uint8_t encoded;
			uint8_t output;

			CHECK(v32_scramble_group(
				      &encoder, input, 3, &encoded) ==
				      V32_FRAMING_OK &&
			      v32_descramble_group(
				      &decoder, encoded, 3, &output) ==
				      V32_FRAMING_OK &&
			      output == input,
			      "V.32 three-bit framing changed at %u/%zu", role,
			      index);
		}
		CHECK(encoder.history == decoder.history,
		      "V.32 three-bit framing histories diverged");
	}
	return true;
}

static bool test_validation(void)
{
	struct v32_scrambler scrambler;
	struct v32_scrambler saved_scrambler;
	struct v32_symbol_encoder encoder;
	struct v32_symbol_encoder saved_encoder;
	struct v32_symbol_decoder decoder;
	struct v32_symbol_decoder saved_decoder;
	uint8_t output = UINT8_MAX;

	CHECK(v32_scrambler_init(
		      &scrambler, V32_CALLING_TRANSMITTER) ==
		      V32_FRAMING_OK &&
		      v32_symbol_encoder_init(
			      &encoder, V32_CALLING_TRANSMITTER, 2) ==
			      V32_FRAMING_OK &&
		      v32_symbol_decoder_init(
			      &decoder, V32_CALLING_TRANSMITTER, 2) ==
			      V32_FRAMING_OK,
	      "V.32 validation fixtures did not initialize");
	saved_scrambler = scrambler;
	saved_encoder = encoder;
	saved_decoder = decoder;
	CHECK(v32_scrambler_init(
		      NULL, V32_CALLING_TRANSMITTER) ==
		      V32_FRAMING_INVALID_ARGUMENT &&
		      v32_scrambler_init(
			      &scrambler,
			      (enum v32_transmitter_role)2) ==
			      V32_FRAMING_INVALID_ARGUMENT &&
		      memcmp(&scrambler, &saved_scrambler,
			     sizeof(scrambler)) == 0 &&
		      v32_scramble_dibit(&scrambler, 4, &output) ==
			      V32_FRAMING_INVALID_ARGUMENT &&
		      memcmp(&scrambler, &saved_scrambler,
			     sizeof(scrambler)) == 0 &&
		      v32_scramble_group(
			      &scrambler, 0, 0, &output) ==
			      V32_FRAMING_INVALID_ARGUMENT &&
		      v32_scramble_group(
			      &scrambler, 0, 9, &output) ==
			      V32_FRAMING_INVALID_ARGUMENT &&
		      v32_scramble_group(
			      &scrambler, 8, 3, &output) ==
			      V32_FRAMING_INVALID_ARGUMENT &&
		      v32_descramble_group(
			      &scrambler, 0, 3, NULL) ==
			      V32_FRAMING_INVALID_ARGUMENT &&
		      memcmp(&scrambler, &saved_scrambler,
			     sizeof(scrambler)) == 0 &&
		      v32_trn_symbol_encode(&encoder, 4, &output) ==
			      V32_FRAMING_INVALID_ARGUMENT &&
		      memcmp(&encoder, &saved_encoder, sizeof(encoder)) == 0 &&
		      v32_trn_symbol_encode(&encoder, 0, NULL) ==
			      V32_FRAMING_INVALID_ARGUMENT &&
		      memcmp(&encoder, &saved_encoder, sizeof(encoder)) == 0 &&
		      v32_4800_symbol_encode(&encoder, 4, &output) ==
			      V32_FRAMING_INVALID_ARGUMENT &&
		      memcmp(&encoder, &saved_encoder, sizeof(encoder)) == 0 &&
		      v32_9600_symbol_encode(&encoder, 16, &output) ==
			      V32_FRAMING_INVALID_ARGUMENT &&
		      memcmp(&encoder, &saved_encoder, sizeof(encoder)) == 0 &&
		      v32_9600_symbol_encode(&encoder, 0, NULL) ==
			      V32_FRAMING_INVALID_ARGUMENT &&
		      memcmp(&encoder, &saved_encoder, sizeof(encoder)) == 0 &&
		      v32_trn_symbol_decode(&decoder, 4, &output) ==
			      V32_FRAMING_INVALID_ARGUMENT &&
		      memcmp(&decoder, &saved_decoder, sizeof(decoder)) == 0 &&
		      v32_trn_symbol_decode(&decoder, 0, NULL) ==
			      V32_FRAMING_INVALID_ARGUMENT &&
		      memcmp(&decoder, &saved_decoder, sizeof(decoder)) == 0 &&
		      v32_9600_symbol_decode(&decoder, 16, &output) ==
			      V32_FRAMING_INVALID_ARGUMENT &&
		      memcmp(&decoder, &saved_decoder, sizeof(decoder)) == 0 &&
		      v32_9600_symbol_decode(&decoder, 0, NULL) ==
			      V32_FRAMING_INVALID_ARGUMENT &&
		      memcmp(&decoder, &saved_decoder, sizeof(decoder)) == 0 &&
		      output == UINT8_MAX &&
		      v32_4800_symbol_decode(NULL, 0, &output) ==
			      V32_FRAMING_INVALID_ARGUMENT &&
		      strcmp(v32_framing_result_name(
			     V32_FRAMING_INVALID_ARGUMENT),
			     "invalid argument") == 0,
	      "V.32 framing accepted invalid input or mutated state");
	scrambler.history = UINT32_C(1) << V32_SCRAMBLER_BITS;
	CHECK(!v32_scrambler_is_valid(&scrambler),
	      "V.32 framing accepted corrupt history");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "descrambler polynomials", test_descrambler_polynomials },
		{ "normative scrambler starts", test_normative_scrambler_starts },
		{ "symbol round trip", test_symbol_round_trip },
		{ "TRN round trip and transition",
		  test_trn_round_trip_and_transition },
		{ "9600 round trip and transition",
		  test_9600_round_trip_and_transition },
		{ "descrambler resynchronization",
		  test_descrambler_resynchronization },
		{ "three-bit group round trip",
		  test_three_bit_group_round_trip },
		{ "validation", test_validation },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		bool passed;

		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		passed = tests[index].run();
		if (!passed) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0 ? 0 : 1;
}
