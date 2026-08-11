// SPDX-License-Identifier: BSD-3-Clause

#include <v22_framing.h>
#include <v22bis_symbols.h>

#include "test.h"

#include <string.h>

enum {
	TEST_STREAM_BITS = 4096,
	TEST_RESYNC_BITS = 160,
};

static uint32_t fixture_random(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static bool test_scrambler_polynomial(void)
{
	struct sippymodem_v22_scrambler scrambler;
	struct sippymodem_v22_scrambler descrambler;

	sippymodem_v22_scrambler_reset(&scrambler);
	sippymodem_v22_scrambler_reset(&descrambler);
	for (unsigned int index = 0; index <= SIPPYMODEM_V22_SCRAMBLER_BITS;
	     ++index) {
		uint8_t decoded;
		uint8_t encoded;
		uint8_t impulse = index == 0 ? 1u : 0u;
		uint8_t expected =
			(index == 0 || index == 14 || index == 17) ? 1u : 0u;

		CHECK(sippymodem_v22_scramble_bit(
			      &scrambler, impulse, &encoded) ==
			      SIPPYMODEM_V22_FRAMING_OK &&
			      encoded == expected,
		      "V.22 scrambler polynomial changed at bit %u", index);
		CHECK(sippymodem_v22_descramble_bit(
			      &descrambler, impulse, &decoded) ==
			      SIPPYMODEM_V22_FRAMING_OK &&
			      decoded == expected,
		      "V.22 descrambler polynomial changed at bit %u", index);
	}
	return true;
}

static bool test_scrambler_lockup_escape(void)
{
	struct sippymodem_v22_scrambler encoder = {
		.history = (UINT32_C(1) << SIPPYMODEM_V22_SCRAMBLER_BITS) - 1,
	};
	struct sippymodem_v22_scrambler decoder = encoder;

	for (unsigned int index = 0;
	     index <= SIPPYMODEM_V22_SCRAMBLER_ONES_LIMIT; ++index) {
		uint8_t decoded = 0;
		uint8_t encoded = 0;
		uint8_t expected_encoded =
			index < SIPPYMODEM_V22_SCRAMBLER_ONES_LIMIT ? 1u : 0u;

		CHECK(sippymodem_v22_scramble_bit(&encoder, 1, &encoded) ==
			      SIPPYMODEM_V22_FRAMING_OK &&
			      encoded == expected_encoded &&
			      sippymodem_v22_descramble_bit(
				      &decoder, encoded, &decoded) ==
				      SIPPYMODEM_V22_FRAMING_OK &&
			      decoded == 1,
		      "V.22 lockup escape changed bit %u", index);
	}
	CHECK(encoder.consecutive_ones == 0 &&
		      decoder.consecutive_ones == 0,
	      "V.22 lockup escape did not clear its detector");
	return true;
}

static bool test_symbol_round_trip(void)
{
	struct sippymodem_v22_symbol_encoder encoder;
	struct sippymodem_v22_symbol_decoder decoder;
	uint32_t random = UINT32_C(0xd17f4a29);

	CHECK(sippymodem_v22_symbol_encoder_init(&encoder, 3) ==
		      SIPPYMODEM_V22_FRAMING_OK &&
		      sippymodem_v22_symbol_decoder_init(&decoder, 3) ==
			      SIPPYMODEM_V22_FRAMING_OK,
	      "V.22 symbol endpoints did not initialize");
	for (size_t index = 0; index < TEST_STREAM_BITS; index += 2) {
		uint8_t first = (uint8_t)(fixture_random(&random) >> 31);
		uint8_t second = (uint8_t)(fixture_random(&random) >> 31);
		uint8_t decoded_first = UINT8_MAX;
		uint8_t decoded_second = UINT8_MAX;
		uint8_t quadrant = UINT8_MAX;

		CHECK(sippymodem_v22_symbol_encode(
			      &encoder, first, second, &quadrant) ==
			      SIPPYMODEM_V22_FRAMING_OK &&
			      sippymodem_v22_symbol_decode(
				      &decoder, quadrant, &decoded_first,
				      &decoded_second) ==
				      SIPPYMODEM_V22_FRAMING_OK &&
			      decoded_first == first &&
			      decoded_second == second,
		      "V.22 symbol round trip changed bits at %zu", index);
	}
	return true;
}

static void bits_from_nibble(uint8_t nibble, uint8_t bits[4])
{
	for (size_t index = 0; index < 4; ++index)
		bits[index] = (uint8_t)((nibble >> (3u - index)) & 1u);
}

static bool test_v22bis_symbol_mapping(void)
{
	for (uint8_t previous = 0; previous < 4; ++previous) {
		for (uint8_t nibble = 0; nibble < 16; ++nibble) {
			struct sippymodem_v22bis_symbol_encoder encoder;
			struct sippymodem_v22bis_symbol_decoder decoder;
			uint8_t decoded[4] = { UINT8_MAX, UINT8_MAX,
					       UINT8_MAX, UINT8_MAX };
			uint8_t bits[4];
			uint8_t expected_quadrant;
			uint8_t symbol = UINT8_MAX;

			bits_from_nibble(nibble, bits);
			CHECK(sippymodem_v22bis_symbol_encoder_init(
				      &encoder, previous) ==
				      SIPPYMODEM_V22_FRAMING_OK &&
				      sippymodem_v22bis_symbol_decoder_init(
					      &decoder, previous) ==
					      SIPPYMODEM_V22_FRAMING_OK &&
				      differential_qpsk_encode(
					      previous, (uint8_t)(nibble >> 2),
					      &expected_quadrant) ==
					      DIFFERENTIAL_QPSK_OK,
			      "V.22bis mapping fixture failed at %u/%u",
			      previous, nibble);
			CHECK(sippymodem_v22bis_symbol_encode(
				      &encoder, bits, &symbol) ==
				      SIPPYMODEM_V22_FRAMING_OK &&
				      symbol == (uint8_t)((expected_quadrant << 2) |
							 (nibble & 3u)) &&
				      sippymodem_v22bis_symbol_decode(
					      &decoder, symbol, decoded) ==
					      SIPPYMODEM_V22_FRAMING_OK &&
				      memcmp(bits, decoded, sizeof(bits)) == 0,
			      "V.22bis symbol mapping changed at %u/%u",
			      previous, nibble);
		}
	}
	return true;
}

static bool test_v22bis_constellation(void)
{
	static const double scale = 0.23570226039551584146694812070162;

	for (uint8_t low_bits = 0; low_bits < 4; ++low_bits) {
		double base_in_phase = (low_bits & 1u) != 0 ? 3.0 : 1.0;
		double base_quadrature = (low_bits & 2u) != 0 ? 3.0 : 1.0;
		struct qam_point points[4];

		for (uint8_t quadrant = 0; quadrant < 4; ++quadrant)
			CHECK(sippymodem_v22bis_constellation_point(
				      (uint8_t)((quadrant << 2) | low_bits),
				      &points[quadrant]) ==
				      SIPPYMODEM_V22_FRAMING_OK,
			      "V.22bis constellation rejected symbol %u",
			      (unsigned int)((quadrant << 2) | low_bits));
		CHECK(points[0].in_phase == base_in_phase * scale &&
			      points[0].quadrature == base_quadrature * scale &&
			      points[1].in_phase == -points[0].quadrature &&
			      points[1].quadrature == points[0].in_phase &&
			      points[2].in_phase == -points[0].in_phase &&
			      points[2].quadrature == -points[0].quadrature &&
			      points[3].in_phase == points[0].quadrature &&
			      points[3].quadrature == -points[0].in_phase,
		      "V.22bis generated constellation changed at low bits %u",
		      low_bits);
		for (uint8_t quadrant = 0; quadrant < 4; ++quadrant) {
			double power =
				points[quadrant].in_phase *
					points[quadrant].in_phase +
				points[quadrant].quadrature *
					points[quadrant].quadrature;

			CHECK(power <= 1.0 + 1e-12,
			      "V.22bis constellation exceeded unit magnitude");
		}
	}
	return true;
}

static bool test_v22bis_symbol_round_trip(void)
{
	struct sippymodem_v22bis_symbol_encoder encoder;
	struct sippymodem_v22bis_symbol_decoder decoder;
	uint32_t random = UINT32_C(0x1222400b);

	CHECK(sippymodem_v22bis_symbol_encoder_init(&encoder, 2) ==
		      SIPPYMODEM_V22_FRAMING_OK &&
		      sippymodem_v22bis_symbol_decoder_init(&decoder, 2) ==
			      SIPPYMODEM_V22_FRAMING_OK,
	      "V.22bis symbol endpoints did not initialize");
	for (size_t index = 0; index < TEST_STREAM_BITS; index += 4) {
		uint8_t bits[4];
		uint8_t decoded[4];
		uint8_t symbol;

		for (size_t bit = 0; bit < ARRAY_LENGTH(bits); ++bit)
			bits[bit] = (uint8_t)(fixture_random(&random) >> 31);
		CHECK(sippymodem_v22bis_symbol_encode(
			      &encoder, bits, &symbol) ==
			      SIPPYMODEM_V22_FRAMING_OK &&
			      sippymodem_v22bis_symbol_decode(
				      &decoder, symbol, decoded) ==
				      SIPPYMODEM_V22_FRAMING_OK &&
			      memcmp(bits, decoded, sizeof(bits)) == 0,
		      "V.22bis symbol round trip changed bits at %zu", index);
	}
	CHECK(encoder.phase_quadrant == decoder.phase_quadrant &&
		      memcmp(&encoder.scrambler, &decoder.descrambler,
			     sizeof(encoder.scrambler)) == 0,
	      "V.22bis framing endpoints diverged");
	return true;
}

static bool test_v22bis_validation(void)
{
	struct sippymodem_v22bis_symbol_encoder encoder;
	struct sippymodem_v22bis_symbol_decoder decoder;
	struct sippymodem_v22bis_symbol_encoder saved_encoder;
	struct qam_point point = { 7.0, 9.0 };
	uint8_t invalid_bits[4] = { 0, 1, 2, 0 };
	uint8_t decoded[4];
	uint8_t symbol = UINT8_MAX;

	CHECK(sippymodem_v22bis_symbol_encoder_init(&encoder, 1) ==
		      SIPPYMODEM_V22_FRAMING_OK &&
		      sippymodem_v22bis_symbol_decoder_init(&decoder, 1) ==
			      SIPPYMODEM_V22_FRAMING_OK,
	      "V.22bis validation endpoints did not initialize");
	saved_encoder = encoder;
	CHECK(sippymodem_v22bis_symbol_encoder_init(NULL, 0) ==
		      SIPPYMODEM_V22_FRAMING_INVALID_ARGUMENT &&
		      sippymodem_v22bis_symbol_decoder_init(&decoder, 4) ==
			      SIPPYMODEM_V22_FRAMING_INVALID_ARGUMENT &&
		      sippymodem_v22bis_symbol_encode(
			      &encoder, invalid_bits, &symbol) ==
			      SIPPYMODEM_V22_FRAMING_INVALID_ARGUMENT &&
		      memcmp(&encoder, &saved_encoder, sizeof(encoder)) == 0 &&
		      symbol == UINT8_MAX &&
		      sippymodem_v22bis_symbol_decode(
			      &decoder, 16, decoded) ==
			      SIPPYMODEM_V22_FRAMING_INVALID_ARGUMENT,
	      "V.22bis framing accepted invalid input or mutated state");
	CHECK(sippymodem_v22bis_constellation_point(16, &point) ==
		      SIPPYMODEM_V22_FRAMING_INVALID_ARGUMENT &&
		      point.in_phase == 7.0 && point.quadrature == 9.0 &&
		      sippymodem_v22bis_constellation_point(0, NULL) ==
			      SIPPYMODEM_V22_FRAMING_INVALID_ARGUMENT,
	      "V.22bis constellation accepted invalid input");
	return true;
}

static bool test_descrambler_resynchronization(void)
{
	struct sippymodem_v22_scrambler encoder;
	struct sippymodem_v22_scrambler decoder;
	uint8_t input[TEST_RESYNC_BITS];
	uint8_t output[TEST_RESYNC_BITS];
	uint32_t random = UINT32_C(0x22a74c91);

	sippymodem_v22_scrambler_reset(&encoder);
	decoder = (struct sippymodem_v22_scrambler){
		.history = UINT32_C(0x15555),
	};
	for (size_t index = 0; index < ARRAY_LENGTH(input); ++index) {
		uint8_t encoded;

		input[index] = (uint8_t)(fixture_random(&random) >> 31);
		CHECK(sippymodem_v22_scramble_bit(
			      &encoder, input[index], &encoded) ==
			      SIPPYMODEM_V22_FRAMING_OK &&
			      sippymodem_v22_descramble_bit(
				      &decoder, encoded, &output[index]) ==
				      SIPPYMODEM_V22_FRAMING_OK,
		      "V.22 resynchronization fixture failed at %zu", index);
	}
	CHECK(memcmp(input + SIPPYMODEM_V22_SCRAMBLER_BITS,
		     output + SIPPYMODEM_V22_SCRAMBLER_BITS,
		     sizeof(input) - SIPPYMODEM_V22_SCRAMBLER_BITS) == 0 &&
		      encoder.history == decoder.history,
	      "V.22 descrambler did not self-synchronize after 17 bits");
	return true;
}

static bool test_validation(void)
{
	struct sippymodem_v22_scrambler scrambler = { 0 };
	struct sippymodem_v22_symbol_encoder encoder;
	struct sippymodem_v22_symbol_decoder decoder;
	uint8_t value = 0;

	CHECK(sippymodem_v22_scramble_bit(NULL, 0, &value) ==
		      SIPPYMODEM_V22_FRAMING_INVALID_ARGUMENT &&
		      sippymodem_v22_scramble_bit(&scrambler, 2, &value) ==
			      SIPPYMODEM_V22_FRAMING_INVALID_ARGUMENT &&
		      sippymodem_v22_descramble_bit(&scrambler, 0, NULL) ==
			      SIPPYMODEM_V22_FRAMING_INVALID_ARGUMENT,
	      "V.22 framing accepted an invalid scalar argument");
	CHECK(sippymodem_v22_symbol_encoder_init(&encoder, 0) ==
		      SIPPYMODEM_V22_FRAMING_OK &&
		      sippymodem_v22_symbol_decoder_init(&decoder, 0) ==
			      SIPPYMODEM_V22_FRAMING_OK,
	      "V.22 validation endpoints did not initialize");
	encoder.scrambler.history = UINT32_C(1) <<
				     SIPPYMODEM_V22_SCRAMBLER_BITS;
	decoder.descrambler.consecutive_ones =
		SIPPYMODEM_V22_SCRAMBLER_ONES_LIMIT + 1;
	CHECK(sippymodem_v22_symbol_encode(&encoder, 0, 0, &value) ==
		      SIPPYMODEM_V22_FRAMING_INVALID_ARGUMENT &&
		      sippymodem_v22_symbol_decode(
			      &decoder, 0, &value, &value) ==
			      SIPPYMODEM_V22_FRAMING_INVALID_ARGUMENT &&
		      strcmp(sippymodem_v22_framing_result_name(
			     SIPPYMODEM_V22_FRAMING_INVALID_ARGUMENT),
			     "invalid argument") == 0,
	      "V.22 framing accepted corrupt state");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "scrambler polynomial", test_scrambler_polynomial },
		{ "scrambler lockup escape", test_scrambler_lockup_escape },
		{ "symbol round trip", test_symbol_round_trip },
		{ "V.22bis symbol mapping", test_v22bis_symbol_mapping },
		{ "V.22bis generated constellation", test_v22bis_constellation },
		{ "V.22bis symbol round trip", test_v22bis_symbol_round_trip },
		{ "V.22bis validation", test_v22bis_validation },
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
