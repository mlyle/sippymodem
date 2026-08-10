// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v32_framing.h>

#include "test.h"

#include <string.h>

enum {
	TEST_STREAM_DIBITS = 2048,
	TEST_RESYNC_DIBITS = 96,
};

static uint32_t fixture_random(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static bool test_descrambler_polynomials(void)
{
	static const struct {
		enum sippymodem_v32_transmitter_role role;
		unsigned int role_tap;
	} fixtures[] = {
		{ SIPPYMODEM_V32_CALLING_TRANSMITTER, 18 },
		{ SIPPYMODEM_V32_ANSWERING_TRANSMITTER, 5 },
	};

	for (size_t fixture = 0; fixture < ARRAY_LENGTH(fixtures); ++fixture) {
		struct sippymodem_v32_scrambler descrambler;

		CHECK(sippymodem_v32_scrambler_init(
			      &descrambler, fixtures[fixture].role) ==
			      SIPPYMODEM_V32_FRAMING_OK,
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
				    position == SIPPYMODEM_V32_SCRAMBLER_BITS)
					expected |= 1u;
			}
			CHECK(sippymodem_v32_descramble_dibit(
				      &descrambler, received, &decoded) ==
				      SIPPYMODEM_V32_FRAMING_OK &&
				      decoded == expected,
			      "V.32 descrambler polynomial changed at bit %u",
			      bit);
		}
	}
	return true;
}

static bool test_normative_scrambler_starts(void)
{
	static const uint8_t expected[][15] = {
		{ 3, 3, 3, 3, 3, 3, 3, 3, 3, 0, 0, 1, 3, 3, 3 },
		{ 3, 3, 2, 0, 0, 3, 3, 2, 0, 0, 3, 2, 1, 3, 3 },
	};

	for (size_t role = 0; role < ARRAY_LENGTH(expected); ++role) {
		struct sippymodem_v32_symbol_encoder encoder;
		uint8_t previous = 0;

		CHECK(sippymodem_v32_symbol_encoder_init(
			      &encoder,
			      (enum sippymodem_v32_transmitter_role)role, 0) ==
			      SIPPYMODEM_V32_FRAMING_OK,
		      "V.32 normative encoder did not initialize");
		for (size_t index = 0; index < ARRAY_LENGTH(expected[role]);
		     ++index) {
			uint8_t quadrant;
			uint8_t scrambled;

			CHECK(sippymodem_v32_symbol_encode(
				      &encoder, 3, &quadrant) ==
				      SIPPYMODEM_V32_FRAMING_OK &&
				      sippymodem_differential_qpsk_decode(
					      previous, quadrant, &scrambled) ==
					      SIPPYMODEM_DIFFERENTIAL_QPSK_OK &&
				      scrambled == expected[role][index],
			      "V.32 normative start changed at %zu/%zu", role,
			      index);
			previous = quadrant;
		}
	}
	return true;
}

static bool test_symbol_round_trip(void)
{
	uint32_t random = UINT32_C(0x32c411a7);

	for (unsigned int role = 0; role < 2; ++role) {
		for (uint8_t initial = 0;
		     initial < SIPPYMODEM_QPSK_QUADRANT_COUNT; ++initial) {
			struct sippymodem_v32_symbol_encoder encoder;
			struct sippymodem_v32_symbol_decoder decoder;

			CHECK(sippymodem_v32_symbol_encoder_init(
				      &encoder,
				      (enum sippymodem_v32_transmitter_role)role,
				      initial) == SIPPYMODEM_V32_FRAMING_OK &&
				      sippymodem_v32_symbol_decoder_init(
					      &decoder,
					      (enum sippymodem_v32_transmitter_role)role,
					      initial) == SIPPYMODEM_V32_FRAMING_OK,
			      "V.32 symbol endpoints did not initialize");
			for (size_t index = 0; index < TEST_STREAM_DIBITS;
			     ++index) {
				uint8_t input =
					(uint8_t)(fixture_random(&random) & 3u);
				uint8_t output;
				uint8_t quadrant;

				CHECK(sippymodem_v32_symbol_encode(
					      &encoder, input, &quadrant) ==
					      SIPPYMODEM_V32_FRAMING_OK &&
				      sippymodem_v32_symbol_decode(
					      &decoder, quadrant, &output) ==
					      SIPPYMODEM_V32_FRAMING_OK &&
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

static bool test_descrambler_resynchronization(void)
{
	struct sippymodem_v32_scrambler encoder;
	struct sippymodem_v32_scrambler decoder;
	uint32_t random = UINT32_C(0xb394e185);

	CHECK(sippymodem_v32_scrambler_init(
		      &encoder, SIPPYMODEM_V32_CALLING_TRANSMITTER) ==
		      SIPPYMODEM_V32_FRAMING_OK &&
		      sippymodem_v32_scrambler_init(
			      &decoder, SIPPYMODEM_V32_CALLING_TRANSMITTER) ==
			      SIPPYMODEM_V32_FRAMING_OK,
	      "V.32 resynchronization endpoints did not initialize");
	decoder.history = UINT32_C(0x5a4c31);
	for (size_t index = 0; index < TEST_RESYNC_DIBITS; ++index) {
		uint8_t input = (uint8_t)(fixture_random(&random) & 3u);
		uint8_t encoded;
		uint8_t output;

		CHECK(sippymodem_v32_scramble_dibit(
			      &encoder, input, &encoded) ==
			      SIPPYMODEM_V32_FRAMING_OK &&
			      sippymodem_v32_descramble_dibit(
				      &decoder, encoded, &output) ==
				      SIPPYMODEM_V32_FRAMING_OK &&
			      (index < 12 || output == input),
		      "V.32 descrambler did not synchronize at %zu", index);
	}
	CHECK(encoder.history == decoder.history,
	      "V.32 descrambler history did not converge");
	return true;
}

static bool test_validation(void)
{
	struct sippymodem_v32_scrambler scrambler;
	struct sippymodem_v32_scrambler saved_scrambler;
	struct sippymodem_v32_symbol_encoder encoder;
	struct sippymodem_v32_symbol_encoder saved;
	uint8_t output = UINT8_MAX;

	CHECK(sippymodem_v32_scrambler_init(
		      &scrambler, SIPPYMODEM_V32_CALLING_TRANSMITTER) ==
		      SIPPYMODEM_V32_FRAMING_OK &&
		      sippymodem_v32_symbol_encoder_init(
			      &encoder, SIPPYMODEM_V32_CALLING_TRANSMITTER, 2) ==
			      SIPPYMODEM_V32_FRAMING_OK,
	      "V.32 validation fixtures did not initialize");
	saved_scrambler = scrambler;
	saved = encoder;
	CHECK(sippymodem_v32_scrambler_init(
		      NULL, SIPPYMODEM_V32_CALLING_TRANSMITTER) ==
		      SIPPYMODEM_V32_FRAMING_INVALID_ARGUMENT &&
		      sippymodem_v32_scrambler_init(
			      &scrambler,
			      (enum sippymodem_v32_transmitter_role)2) ==
			      SIPPYMODEM_V32_FRAMING_INVALID_ARGUMENT &&
		      memcmp(&scrambler, &saved_scrambler,
			     sizeof(scrambler)) == 0 &&
		      sippymodem_v32_scramble_dibit(&scrambler, 4, &output) ==
			      SIPPYMODEM_V32_FRAMING_INVALID_ARGUMENT &&
		      memcmp(&scrambler, &saved_scrambler,
			     sizeof(scrambler)) == 0 &&
		      sippymodem_v32_symbol_encode(&encoder, 4, &output) ==
			      SIPPYMODEM_V32_FRAMING_INVALID_ARGUMENT &&
		      memcmp(&encoder, &saved, sizeof(encoder)) == 0 &&
		      output == UINT8_MAX &&
		      sippymodem_v32_symbol_decode(NULL, 0, &output) ==
			      SIPPYMODEM_V32_FRAMING_INVALID_ARGUMENT &&
		      strcmp(sippymodem_v32_framing_result_name(
			     SIPPYMODEM_V32_FRAMING_INVALID_ARGUMENT),
			     "invalid argument") == 0,
	      "V.32 framing accepted invalid input or mutated state");
	scrambler.history = UINT32_C(1) << SIPPYMODEM_V32_SCRAMBLER_BITS;
	CHECK(!sippymodem_v32_scrambler_is_valid(&scrambler),
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
		{ "descrambler resynchronization",
		  test_descrambler_resynchronization },
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
