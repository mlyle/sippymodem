// SPDX-License-Identifier: BSD-3-Clause

#include <v22_framing.h>
#include <v22bis_symbols.h>

#include "test.h"

#include <math.h>
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
	struct v22_scrambler scrambler;
	struct v22_scrambler descrambler;

	v22_scrambler_reset(&scrambler);
	v22_scrambler_reset(&descrambler);
	for (unsigned int index = 0; index <= V22_SCRAMBLER_BITS;
	     ++index) {
		uint8_t decoded;
		uint8_t encoded;
		uint8_t impulse = index == 0 ? 1u : 0u;
		uint8_t expected =
			(index == 0 || index == 14 || index == 17) ? 1u : 0u;

		CHECK(v22_scramble_bit(
			      &scrambler, impulse, &encoded) ==
			      V22_FRAMING_OK &&
			      encoded == expected,
		      "V.22 scrambler polynomial changed at bit %u", index);
		CHECK(v22_descramble_bit(
			      &descrambler, impulse, &decoded) ==
			      V22_FRAMING_OK &&
			      decoded == expected,
		      "V.22 descrambler polynomial changed at bit %u", index);
	}
	return true;
}

static bool test_scrambler_lockup_escape(void)
{
	struct v22_scrambler encoder = {
		.history = (UINT32_C(1) << V22_SCRAMBLER_BITS) - 1,
	};
	struct v22_scrambler decoder = encoder;

	for (unsigned int index = 0;
	     index <= V22_SCRAMBLER_ONES_LIMIT; ++index) {
		uint8_t decoded = 0;
		uint8_t encoded = 0;
		uint8_t expected_encoded =
			index < V22_SCRAMBLER_ONES_LIMIT ? 1u : 0u;

		CHECK(v22_scramble_bit(&encoder, 1, &encoded) ==
			      V22_FRAMING_OK &&
			      encoded == expected_encoded &&
			      v22_descramble_bit(
				      &decoder, encoded, &decoded) ==
				      V22_FRAMING_OK &&
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
	struct v22_symbol_encoder encoder;
	struct v22_symbol_decoder decoder;
	uint32_t random = UINT32_C(0xd17f4a29);

	CHECK(v22_symbol_encoder_init(&encoder, 3) ==
		      V22_FRAMING_OK &&
		      v22_symbol_decoder_init(&decoder, 3) ==
			      V22_FRAMING_OK,
	      "V.22 symbol endpoints did not initialize");
	for (size_t index = 0; index < TEST_STREAM_BITS; index += 2) {
		uint8_t first = (uint8_t)(fixture_random(&random) >> 31);
		uint8_t second = (uint8_t)(fixture_random(&random) >> 31);
		uint8_t decoded_first = UINT8_MAX;
		uint8_t decoded_second = UINT8_MAX;
		uint8_t quadrant = UINT8_MAX;

		CHECK(v22_symbol_encode(
			      &encoder, first, second, &quadrant) ==
			      V22_FRAMING_OK &&
			      v22_symbol_decode(
				      &decoder, quadrant, &decoded_first,
				      &decoded_second) ==
				      V22_FRAMING_OK &&
			      decoded_first == first &&
			      decoded_second == second,
		      "V.22 symbol round trip changed bits at %zu", index);
	}
	return true;
}

static bool test_600_symbol_mapping(void)
{
	for (uint8_t previous = 0; previous < QPSK_QUADRANT_COUNT;
	     ++previous) {
		for (uint8_t bit = 0; bit < 2; ++bit) {
			struct v22_symbol_encoder encoder;
			struct v22_symbol_decoder decoder;
			uint8_t decoded = UINT8_MAX;
			uint8_t quadrant = UINT8_MAX;
			uint8_t expected = (uint8_t)(
				(previous + (bit == 0 ? 1u : 3u)) & 3u);

			CHECK(v22_symbol_encoder_init(&encoder, previous) ==
				      V22_FRAMING_OK &&
			      v22_symbol_decoder_init(&decoder, previous) ==
				      V22_FRAMING_OK &&
			      v22_600_symbol_encode(
				      &encoder, bit, &quadrant) ==
				      V22_FRAMING_OK &&
			      quadrant == expected &&
			      v22_600_symbol_decode(
				      &decoder, quadrant, &decoded) ==
				      V22_FRAMING_OK &&
			      decoded == bit,
			      "V.22 600 mapping changed at %u/%u",
			      previous, bit);
		}
	}
	return true;
}

static bool test_600_symbol_round_trip(void)
{
	struct v22_symbol_encoder encoder;
	struct v22_symbol_decoder decoder;
	uint32_t random = UINT32_C(0x60022b17);

	CHECK(v22_symbol_encoder_init(&encoder, 2) == V22_FRAMING_OK &&
		      v22_symbol_decoder_init(&decoder, 2) ==
			      V22_FRAMING_OK,
	      "V.22 600 framing endpoints did not initialize");
	for (size_t index = 0; index < TEST_STREAM_BITS; ++index) {
		uint8_t bit = (uint8_t)(fixture_random(&random) >> 31);
		uint8_t decoded = UINT8_MAX;
		uint8_t quadrant = UINT8_MAX;

		CHECK(v22_600_symbol_encode(
			      &encoder, bit, &quadrant) == V22_FRAMING_OK &&
			      v22_600_symbol_decode(
				      &decoder, quadrant, &decoded) ==
				      V22_FRAMING_OK &&
			      decoded == bit,
		      "V.22 600 round trip changed bit %zu", index);
	}
	CHECK(encoder.phase_quadrant == decoder.phase_quadrant &&
		      memcmp(&encoder.scrambler, &decoder.descrambler,
			     sizeof(encoder.scrambler)) == 0,
	      "V.22 600 framing endpoints diverged");
	return true;
}

static bool test_600_validation(void)
{
	struct v22_symbol_encoder encoder;
	struct v22_symbol_decoder decoder;
	struct v22_symbol_encoder saved_encoder;
	struct v22_symbol_decoder saved_decoder;
	uint8_t value = UINT8_MAX;

	CHECK(v22_symbol_encoder_init(&encoder, 0) == V22_FRAMING_OK &&
		      v22_symbol_decoder_init(&decoder, 0) ==
			      V22_FRAMING_OK,
	      "V.22 600 validation endpoints did not initialize");
	saved_encoder = encoder;
	saved_decoder = decoder;
	CHECK(v22_600_symbol_encode(&encoder, 2, &value) ==
		      V22_FRAMING_INVALID_ARGUMENT &&
		      memcmp(&encoder, &saved_encoder, sizeof(encoder)) == 0 &&
		      value == UINT8_MAX &&
		      v22_600_symbol_decode(&decoder, 0, &value) ==
			      V22_FRAMING_INVALID_ARGUMENT &&
		      memcmp(&decoder, &saved_decoder, sizeof(decoder)) == 0 &&
		      value == UINT8_MAX &&
		      v22_600_symbol_decode(&decoder, 2, &value) ==
			      V22_FRAMING_INVALID_ARGUMENT &&
		      memcmp(&decoder, &saved_decoder, sizeof(decoder)) == 0 &&
		      value == UINT8_MAX,
	      "V.22 600 framing accepted invalid input or mutated state");
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
			struct v22bis_symbol_encoder encoder;
			struct v22bis_symbol_decoder decoder;
			uint8_t decoded[4] = { UINT8_MAX, UINT8_MAX,
					       UINT8_MAX, UINT8_MAX };
			uint8_t bits[4];
			uint8_t expected_quadrant;
			uint8_t symbol = UINT8_MAX;

			bits_from_nibble(nibble, bits);
			CHECK(v22bis_symbol_encoder_init(
				      &encoder, previous) ==
				      V22_FRAMING_OK &&
				      v22bis_symbol_decoder_init(
					      &decoder, previous) ==
					      V22_FRAMING_OK &&
				      differential_qpsk_encode(
					      previous, (uint8_t)(nibble >> 2),
					      &expected_quadrant) ==
					      DIFFERENTIAL_QPSK_OK,
			      "V.22bis mapping fixture failed at %u/%u",
			      previous, nibble);
			CHECK(v22bis_symbol_encode(
				      &encoder, bits, &symbol) ==
				      V22_FRAMING_OK &&
				      symbol == (uint8_t)((expected_quadrant << 2) |
							 (nibble & 3u)) &&
				      v22bis_symbol_decode(
					      &decoder, symbol, decoded) ==
					      V22_FRAMING_OK &&
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
			CHECK(v22bis_constellation_point(
				      (uint8_t)((quadrant << 2) | low_bits),
				      &points[quadrant]) ==
				      V22_FRAMING_OK,
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

static bool test_v22bis_1200_constellation(void)
{
	struct qam_point points[QPSK_QUADRANT_COUNT];
	struct qam_point invalid = { 7.0, 9.0 };
	double online_power = 0.0;
	double signalling_power = 0.0;

	for (uint8_t symbol = 0; symbol < V22BIS_SYMBOL_COUNT; ++symbol) {
		struct qam_point online;

		CHECK(v22bis_constellation_point(symbol, &online) ==
			      V22_FRAMING_OK,
		      "V.22bis online point %u was unavailable", symbol);
		online_power += online.in_phase * online.in_phase +
				online.quadrature * online.quadrature;
	}
	online_power /= V22BIS_SYMBOL_COUNT;

	for (uint8_t quadrant = 0; quadrant < QPSK_QUADRANT_COUNT;
	     ++quadrant) {
		struct qam_point expected;
		double power;
		double cross;
		double dot;

		CHECK(v22bis_1200_constellation_point(
			      quadrant,
			      &points[quadrant]) == V22_FRAMING_OK &&
			      v22bis_constellation_point(
				      (uint8_t)((quadrant << 2) | 1u),
				      &expected) == V22_FRAMING_OK,
		      "V.22bis 1200-bit/s point %u was unavailable", quadrant);
		cross = points[quadrant].in_phase * expected.quadrature -
			points[quadrant].quadrature * expected.in_phase;
		dot = points[quadrant].in_phase * expected.in_phase +
		      points[quadrant].quadrature * expected.quadrature;
		CHECK(fabs(cross) <= 1e-12 && dot > 0.0 &&
			      points[quadrant].in_phase == expected.in_phase &&
			      points[quadrant].quadrature == expected.quadrature,
		      "V.22bis 1200-bit/s point %u left its Figure 2 01 element",
		      quadrant);
		power = points[quadrant].in_phase *
				points[quadrant].in_phase +
			points[quadrant].quadrature *
				points[quadrant].quadrature;
		CHECK(fabs(power - 10.0 / 18.0) <= 1e-12,
		      "V.22bis 1200-bit/s point %u changed power", quadrant);
		signalling_power += power;
	}
	signalling_power /= QPSK_QUADRANT_COUNT;
	CHECK(fabs(online_power - 10.0 / 18.0) <= 1e-12 &&
		      fabs(signalling_power - 10.0 / 18.0) <= 1e-12 &&
		      fabs(signalling_power / online_power - 1.0) <= 1e-12,
	      "V.22bis 1200-to-2400 transition power changed");
	CHECK(fabs(points[0].in_phase - 3.0 * points[0].quadrature) <=
		      1e-12 &&
		      points[1].in_phase == -points[0].quadrature &&
		      points[1].quadrature == points[0].in_phase &&
		      points[2].in_phase == -points[0].in_phase &&
		      points[2].quadrature == -points[0].quadrature &&
		      points[3].in_phase == points[0].quadrature &&
		      points[3].quadrature == -points[0].in_phase,
	      "V.22bis 1200-bit/s 01 points changed orientation");
	CHECK(v22bis_1200_constellation_point(4, &invalid) ==
		      V22_FRAMING_INVALID_ARGUMENT &&
		      invalid.in_phase == 7.0 && invalid.quadrature == 9.0 &&
		      v22bis_1200_constellation_point(0, NULL) ==
			      V22_FRAMING_INVALID_ARGUMENT,
	      "V.22bis 1200-bit/s points accepted invalid input");
	return true;
}

static bool test_v22bis_symbol_round_trip(void)
{
	struct v22bis_symbol_encoder encoder;
	struct v22bis_symbol_decoder decoder;
	uint32_t random = UINT32_C(0x1222400b);

	CHECK(v22bis_symbol_encoder_init(&encoder, 2) ==
		      V22_FRAMING_OK &&
		      v22bis_symbol_decoder_init(&decoder, 2) ==
			      V22_FRAMING_OK,
	      "V.22bis symbol endpoints did not initialize");
	for (size_t index = 0; index < TEST_STREAM_BITS; index += 4) {
		uint8_t bits[4];
		uint8_t decoded[4];
		uint8_t symbol;

		for (size_t bit = 0; bit < ARRAY_LENGTH(bits); ++bit)
			bits[bit] = (uint8_t)(fixture_random(&random) >> 31);
		CHECK(v22bis_symbol_encode(
			      &encoder, bits, &symbol) ==
			      V22_FRAMING_OK &&
			      v22bis_symbol_decode(
				      &decoder, symbol, decoded) ==
				      V22_FRAMING_OK &&
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
	struct v22bis_symbol_encoder encoder;
	struct v22bis_symbol_decoder decoder;
	struct v22bis_symbol_encoder saved_encoder;
	struct qam_point point = { 7.0, 9.0 };
	uint8_t invalid_bits[4] = { 0, 1, 2, 0 };
	uint8_t decoded[4];
	uint8_t symbol = UINT8_MAX;

	CHECK(v22bis_symbol_encoder_init(&encoder, 1) ==
		      V22_FRAMING_OK &&
		      v22bis_symbol_decoder_init(&decoder, 1) ==
			      V22_FRAMING_OK,
	      "V.22bis validation endpoints did not initialize");
	saved_encoder = encoder;
	CHECK(v22bis_symbol_encoder_init(NULL, 0) ==
		      V22_FRAMING_INVALID_ARGUMENT &&
		      v22bis_symbol_decoder_init(&decoder, 4) ==
			      V22_FRAMING_INVALID_ARGUMENT &&
		      v22bis_symbol_encode(
			      &encoder, invalid_bits, &symbol) ==
			      V22_FRAMING_INVALID_ARGUMENT &&
		      memcmp(&encoder, &saved_encoder, sizeof(encoder)) == 0 &&
		      symbol == UINT8_MAX &&
		      v22bis_symbol_decode(
			      &decoder, 16, decoded) ==
			      V22_FRAMING_INVALID_ARGUMENT,
	      "V.22bis framing accepted invalid input or mutated state");
	CHECK(v22bis_constellation_point(16, &point) ==
		      V22_FRAMING_INVALID_ARGUMENT &&
		      point.in_phase == 7.0 && point.quadrature == 9.0 &&
		      v22bis_constellation_point(0, NULL) ==
			      V22_FRAMING_INVALID_ARGUMENT,
	      "V.22bis constellation accepted invalid input");
	return true;
}

static bool test_v22bis_rate_signal_detection(void)
{
	struct v22bis_rate_signal_detector detector;
	struct v22bis_rate_signal_detector saved;
	enum v22bis_rate_signal_event event = V22BIS_RATE_SIGNAL_ENDED;
	unsigned int starts = 0;

	v22bis_rate_signal_detector_reset(&detector);
	CHECK(v22bis_rate_signal_detector_is_valid(&detector),
	      "V.22bis rate-signal detector did not initialize");
	for (size_t index = 0;
	     index < V22BIS_RATE_SIGNAL_MIN_SYMBOLS - 1u; ++index) {
		uint8_t turn = (index & 1u) == 0 ? 1u : 3u;

		CHECK(v22bis_rate_signal_observe(
			      &detector, turn, &event) == V22_FRAMING_OK &&
			      event == V22BIS_RATE_SIGNAL_NONE &&
			      !detector.present,
		      "V.22bis rate signal started on a short run at %zu",
		      index);
	}
	CHECK(v22bis_rate_signal_observe(&detector, 0, &event) ==
		      V22_FRAMING_OK &&
		      event == V22BIS_RATE_SIGNAL_NONE && !detector.present,
	      "V.22bis rate signal accepted an interrupted run");

	v22bis_rate_signal_detector_reset(&detector);
	for (size_t index = 0;
	     index < V22BIS_RATE_SIGNAL_SYMBOLS - 1u; ++index) {
		uint8_t turn = (index & 1u) == 0 ? 1u : 3u;

		CHECK(v22bis_rate_signal_observe(
			      &detector, turn, &event) == V22_FRAMING_OK,
		      "V.22bis rate signal rejected short symbol %zu", index);
	}
	CHECK(detector.present &&
		      v22bis_rate_signal_observe(&detector, 1, &event) ==
			      V22_FRAMING_OK &&
		      event == V22BIS_RATE_SIGNAL_ENDED && !detector.present,
	      "V.22bis rate signal did not distinguish a 59-symbol run");

	v22bis_rate_signal_detector_reset(&detector);
	for (size_t index = 0; index < V22BIS_RATE_SIGNAL_SYMBOLS;
	     ++index) {
		uint8_t turn;

		CHECK(v22bis_rate_signal_turn(
			      (uint32_t)index, &turn) == V22_FRAMING_OK &&
			      turn == ((index & 1u) == 0 ? 1u : 3u) &&
			      v22bis_rate_signal_observe(
				      &detector, turn, &event) ==
				      V22_FRAMING_OK,
		      "V.22bis rate signal rejected generated symbol %zu",
		      index);
		if (event == V22BIS_RATE_SIGNAL_STARTED)
			starts++;
		CHECK(event == V22BIS_RATE_SIGNAL_NONE ||
			      (event == V22BIS_RATE_SIGNAL_STARTED &&
			       index + 1u == V22BIS_RATE_SIGNAL_MIN_SYMBOLS),
		      "V.22bis rate signal changed state at symbol %zu", index);
	}
	CHECK(starts == 1 && detector.present &&
		      detector.alternating_symbols ==
			      V22BIS_RATE_SIGNAL_SYMBOLS &&
		      v22bis_rate_signal_observe(&detector, 3, &event) ==
			      V22_FRAMING_OK &&
		      event == V22BIS_RATE_SIGNAL_FULLY_ENDED &&
		      !detector.present,
	      "V.22bis rate signal did not end on the following mark");

	saved = detector;
	event = V22BIS_RATE_SIGNAL_STARTED;
	{
		uint8_t turn = 7;

		CHECK(v22bis_rate_signal_turn(
			      V22BIS_RATE_SIGNAL_SYMBOLS, &turn) ==
			      V22_FRAMING_INVALID_ARGUMENT &&
			      turn == 7 &&
			      v22bis_rate_signal_turn(0, NULL) ==
				      V22_FRAMING_INVALID_ARGUMENT,
		      "V.22bis rate-signal generation accepted invalid bounds");
	}
	CHECK(v22bis_rate_signal_observe(&detector, 4, &event) ==
		      V22_FRAMING_INVALID_ARGUMENT &&
		      memcmp(&detector, &saved, sizeof(detector)) == 0 &&
		      event == V22BIS_RATE_SIGNAL_STARTED &&
		      v22bis_rate_signal_observe(&detector, 1, NULL) ==
			      V22_FRAMING_INVALID_ARGUMENT,
	      "V.22bis rate-signal validation was not transactional");
	return true;
}

static bool test_descrambler_resynchronization(void)
{
	struct v22_scrambler encoder;
	struct v22_scrambler decoder;
	uint8_t input[TEST_RESYNC_BITS];
	uint8_t output[TEST_RESYNC_BITS];
	uint32_t random = UINT32_C(0x22a74c91);

	v22_scrambler_reset(&encoder);
	decoder = (struct v22_scrambler){
		.history = UINT32_C(0x15555),
	};
	for (size_t index = 0; index < ARRAY_LENGTH(input); ++index) {
		uint8_t encoded;

		input[index] = (uint8_t)(fixture_random(&random) >> 31);
		CHECK(v22_scramble_bit(
			      &encoder, input[index], &encoded) ==
			      V22_FRAMING_OK &&
			      v22_descramble_bit(
				      &decoder, encoded, &output[index]) ==
				      V22_FRAMING_OK,
		      "V.22 resynchronization fixture failed at %zu", index);
	}
	CHECK(memcmp(input + V22_SCRAMBLER_BITS,
		     output + V22_SCRAMBLER_BITS,
		     sizeof(input) - V22_SCRAMBLER_BITS) == 0 &&
		      encoder.history == decoder.history,
	      "V.22 descrambler did not self-synchronize after 17 bits");
	return true;
}

static bool test_validation(void)
{
	struct v22_scrambler scrambler = { 0 };
	struct v22_symbol_encoder encoder;
	struct v22_symbol_decoder decoder;
	uint8_t value = 0;

	CHECK(v22_scramble_bit(NULL, 0, &value) ==
		      V22_FRAMING_INVALID_ARGUMENT &&
		      v22_scramble_bit(&scrambler, 2, &value) ==
			      V22_FRAMING_INVALID_ARGUMENT &&
		      v22_descramble_bit(&scrambler, 0, NULL) ==
			      V22_FRAMING_INVALID_ARGUMENT,
	      "V.22 framing accepted an invalid scalar argument");
	CHECK(v22_symbol_encoder_init(&encoder, 0) ==
		      V22_FRAMING_OK &&
		      v22_symbol_decoder_init(&decoder, 0) ==
			      V22_FRAMING_OK,
	      "V.22 validation endpoints did not initialize");
	encoder.scrambler.history = UINT32_C(1) <<
				     V22_SCRAMBLER_BITS;
	decoder.descrambler.consecutive_ones =
		V22_SCRAMBLER_ONES_LIMIT + 1;
	CHECK(v22_symbol_encode(&encoder, 0, 0, &value) ==
		      V22_FRAMING_INVALID_ARGUMENT &&
		      v22_symbol_decode(
			      &decoder, 0, &value, &value) ==
			      V22_FRAMING_INVALID_ARGUMENT &&
		      strcmp(v22_framing_result_name(
			     V22_FRAMING_INVALID_ARGUMENT),
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
		{ "V.22 600 symbol mapping", test_600_symbol_mapping },
		{ "V.22 600 symbol round trip", test_600_symbol_round_trip },
		{ "V.22 600 validation", test_600_validation },
		{ "V.22bis symbol mapping", test_v22bis_symbol_mapping },
		{ "V.22bis generated constellation", test_v22bis_constellation },
		{ "V.22bis 1200-bit/s constellation",
		  test_v22bis_1200_constellation },
		{ "V.22bis symbol round trip", test_v22bis_symbol_round_trip },
		{ "V.22bis validation", test_v22bis_validation },
		{ "V.22bis rate-signal generation and detection",
		  test_v22bis_rate_signal_detection },
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
