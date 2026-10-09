// SPDX-License-Identifier: BSD-3-Clause

#include <v22_framing.h>
#include <v22bis_symbols.h>

#include "test.h"

#include <math.h>
#include <stddef.h>
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
			      &scrambler, impulse, &encoded, true) ==
			      V22_FRAMING_OK &&
			      encoded == expected,
		      "V.22 scrambler polynomial changed at bit %u", index);
		CHECK(v22_descramble_bit(
			      &descrambler, impulse, &decoded, true) ==
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

		CHECK(v22_scramble_bit(&encoder, 1, &encoded, true) ==
			      V22_FRAMING_OK &&
			      encoded == expected_encoded &&
			      v22_descramble_bit(
				      &decoder, encoded, &decoded, true) ==
				      V22_FRAMING_OK &&
			      decoded == 1,
		      "V.22 lockup escape changed bit %u", index);
	}
	CHECK(encoder.consecutive_ones == 0 &&
		      decoder.consecutive_ones == 0,
	      "V.22 lockup escape did not clear its detector");
	return true;
}

static bool test_training_scrambler_policy(void)
{
	struct v22_scrambler encoder = {
		.history = V22_SCRAMBLER_HISTORY_MASK,
		.consecutive_ones = V22_SCRAMBLER_ONES_LIMIT,
	};
	struct v22_scrambler decoder = encoder;

	/* V.22 clause 5: no repair during handshake, even at a pending escape.
	 * Resuming data starts a fresh detector without resetting wire history. */
	for (unsigned int index = 0; index < 192; ++index) {
		uint8_t wire = 0, data = 0;

		CHECK(v22_scramble_bit(&encoder, 1, &wire, false) == V22_FRAMING_OK &&
		      v22_descramble_bit(&decoder, wire, &data, false) == V22_FRAMING_OK &&
		      wire == 1 && data == 1 && encoder.consecutive_ones == 0 &&
		      decoder.consecutive_ones == 0 &&
		      encoder.history == V22_SCRAMBLER_HISTORY_MASK &&
		      decoder.history == V22_SCRAMBLER_HISTORY_MASK,
		      "disabled repair changed training at bit %u", index);
	}
	for (unsigned int index = 0; index <= V22_SCRAMBLER_ONES_LIMIT; ++index) {
		uint8_t wire = 1, data = 0;

		CHECK(v22_scramble_bit(&encoder, 1, &wire, true) == V22_FRAMING_OK &&
		      v22_descramble_bit(&decoder, wire, &data, true) == V22_FRAMING_OK &&
		      wire == (index < V22_SCRAMBLER_ONES_LIMIT) && data == 1,
		      "resumed repair changed data at bit %u", index);
	}
	struct v22_symbol_encoder bis_encoder;
	struct v22_symbol_decoder bis_decoder;
	const uint8_t mark[4] = { 1, 1, 1, 1 };
	uint8_t decoded[4], symbol;

	CHECK(v22_symbol_encoder_init(&bis_encoder, 0) == V22_FRAMING_OK &&
	      v22_symbol_decoder_init(&bis_decoder, 0) == V22_FRAMING_OK,
	      "V.22bis repair endpoints did not initialize");
	bis_encoder.scrambler = (struct v22_scrambler){ V22_SCRAMBLER_HISTORY_MASK, 64 };
	bis_decoder.descrambler = bis_encoder.scrambler;
	/* V.22bis keeps repair active: wire 0111 maps to quadrant 0, point 3. */
	CHECK(v22bis_symbol_encode(&bis_encoder, mark, &symbol) == V22_FRAMING_OK &&
	      symbol == 3 && bis_encoder.scrambler.consecutive_ones == 3 &&
	      v22bis_symbol_decode(&bis_decoder, symbol, decoded) == V22_FRAMING_OK &&
	      memcmp(decoded, mark, sizeof(mark)) == 0 &&
	      bis_decoder.descrambler.consecutive_ones == 3,
	      "V.22bis disabled its repair detector");
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
		uint8_t decoded[2] = { UINT8_MAX, UINT8_MAX };
		uint8_t quadrant = UINT8_MAX;

		CHECK(v22_symbol_encode(
			      &encoder, first, second, &quadrant, true) ==
			      V22_FRAMING_OK &&
			      v22_symbol_decode(
				      &decoder, quadrant, decoded, true) ==
				      V22_FRAMING_OK &&
			      decoded[0] == first &&
			      decoded[1] == second,
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
				      &encoder, bit, &quadrant, true) ==
				      V22_FRAMING_OK &&
			      quadrant == expected &&
			      v22_600_symbol_decode(
				      &decoder, quadrant, &decoded, true) ==
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
			      &encoder, bit, &quadrant, true) == V22_FRAMING_OK &&
			      v22_600_symbol_decode(
				      &decoder, quadrant, &decoded, true) ==
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
	CHECK(v22_600_symbol_encode(&encoder, 2, &value, true) ==
		      V22_FRAMING_INVALID_ARGUMENT &&
		      memcmp(&encoder, &saved_encoder, sizeof(encoder)) == 0 &&
		      value == UINT8_MAX &&
		      v22_600_symbol_decode(&decoder, 0, &value, true) ==
			      V22_FRAMING_INVALID_ARGUMENT &&
		      memcmp(&decoder, &saved_decoder, sizeof(decoder)) == 0 &&
		      value == UINT8_MAX &&
		      v22_600_symbol_decode(&decoder, 2, &value, true) ==
			      V22_FRAMING_INVALID_ARGUMENT &&
		      memcmp(&decoder, &saved_decoder, sizeof(decoder)) == 0 &&
		      value == UINT8_MAX,
	      "V.22 600 framing accepted invalid input or mutated state");
	CHECK(v22_600_symbol_encode(NULL, 0, &value, true) ==
		      V22_FRAMING_INVALID_ARGUMENT &&
		      v22_600_symbol_encode(&encoder, 0, NULL, true) ==
			      V22_FRAMING_INVALID_ARGUMENT &&
		      v22_600_symbol_decode(NULL, 1, &value, true) ==
			      V22_FRAMING_INVALID_ARGUMENT &&
		      v22_600_symbol_decode(&decoder, 1, NULL, true) ==
			      V22_FRAMING_INVALID_ARGUMENT &&
		      v22_600_symbol_decode(&decoder, 4, &value, true) ==
			      V22_FRAMING_INVALID_ARGUMENT &&
		      memcmp(&encoder, &saved_encoder, sizeof(encoder)) == 0 &&
		      memcmp(&decoder, &saved_decoder, sizeof(decoder)) == 0 &&
		      value == UINT8_MAX,
	      "V.22 600 scalar validation changed state or output");
	for (unsigned int invalid = 0; invalid < 3; ++invalid) {
		encoder = saved_encoder;
		decoder = saved_decoder;
		if (invalid == 0) {
			encoder.scrambler.history = UINT32_C(1) << 17;
			decoder.descrambler.history = UINT32_C(1) << 17;
		} else if (invalid == 1) {
			encoder.scrambler.consecutive_ones = 65;
			decoder.descrambler.consecutive_ones = 65;
		} else {
			encoder.phase_quadrant = 4;
			decoder.phase_quadrant = 4;
		}
		{
			struct v22_symbol_encoder before_tx = encoder;
			struct v22_symbol_decoder before_rx = decoder;

			CHECK(v22_600_symbol_encode(&encoder, 0, &value, true) ==
				      V22_FRAMING_INVALID_ARGUMENT &&
				      v22_600_symbol_decode(&decoder, 1, &value, true) ==
					      V22_FRAMING_INVALID_ARGUMENT &&
				      memcmp(&encoder, &before_tx, sizeof(encoder)) == 0 &&
				      memcmp(&decoder, &before_rx, sizeof(decoder)) == 0 &&
				      value == UINT8_MAX,
			      "V.22 600 corrupt state %u was not rejected atomically",
			      invalid);
		}
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
			struct v22_symbol_encoder encoder;
			struct v22_symbol_decoder decoder;
			uint8_t decoded[4] = { UINT8_MAX, UINT8_MAX,
					       UINT8_MAX, UINT8_MAX };
			uint8_t bits[4];
			uint8_t expected_quadrant;
			uint8_t symbol = UINT8_MAX;

			bits_from_nibble(nibble, bits);
			CHECK(v22_symbol_encoder_init(
				      &encoder, previous) ==
				      V22_FRAMING_OK &&
				      v22_symbol_decoder_init(
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
		CHECK(points[0].in_phase == (double)(base_in_phase * scale) &&
			      points[0].quadrature == (double)(base_quadrature * scale) &&
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
	struct v22_symbol_encoder encoder;
	struct v22_symbol_decoder decoder;
	uint32_t random = UINT32_C(0x1222400b);

	CHECK(v22_symbol_encoder_init(&encoder, 2) ==
		      V22_FRAMING_OK &&
		      v22_symbol_decoder_init(&decoder, 2) ==
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
	struct v22_symbol_encoder encoder;
	struct v22_symbol_decoder decoder;
	struct v22_symbol_encoder saved_encoder;
	struct qam_point point = { 7.0, 9.0 };
	uint8_t invalid_bits[4] = { 0, 1, 2, 0 };
	uint8_t decoded[4];
	uint8_t symbol = UINT8_MAX;

	CHECK(v22_symbol_encoder_init(&encoder, 1) ==
		      V22_FRAMING_OK &&
		      v22_symbol_decoder_init(&decoder, 1) ==
			      V22_FRAMING_OK,
	      "V.22bis validation endpoints did not initialize");
	saved_encoder = encoder;
	CHECK(v22_symbol_encoder_init(NULL, 0) ==
		      V22_FRAMING_INVALID_ARGUMENT &&
		      v22_symbol_decoder_init(&decoder, 4) ==
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

static bool test_v22bis_symbol_aliasing(void)
{
	struct decode_fixture {
		uint8_t prefix[V22BIS_BITS_PER_SYMBOL];
		struct v22_symbol_decoder owner;
		uint8_t suffix[V22BIS_BITS_PER_SYMBOL];
	} initial, fixture;

	for (uint8_t ones = V22_SCRAMBLER_ONES_LIMIT - 4u;
	     ones <= V22_SCRAMBLER_ONES_LIMIT; ++ones) {
		for (uint8_t phase = 0; phase < QPSK_QUADRANT_COUNT; ++phase) {
			struct v22_symbol_encoder encoder, before, expected;
			uint8_t bits[V22BIS_BITS_PER_SYMBOL], symbol, expected_symbol;

			CHECK(v22_symbol_encoder_init(&encoder, phase) == V22_FRAMING_OK,
			      "alias encoder did not initialize");
			encoder.scrambler.history = UINT32_C(0x101);
			encoder.scrambler.consecutive_ones = ones;
			before = expected = encoder;
			memcpy(bits, &encoder.scrambler.history, sizeof(bits));
			CHECK(v22bis_symbol_encode(&expected, bits, &expected_symbol) == V22_FRAMING_OK &&
			      v22bis_symbol_encode(&encoder,
				      (const uint8_t *)&encoder.scrambler.history, &symbol) == V22_FRAMING_OK &&
			      symbol == expected_symbol && memcmp(&encoder, &expected, sizeof(encoder)) == 0,
			      "input/owner alias changed at %u/%u", ones, phase);
			for (size_t offset = 0; offset < sizeof(encoder); ++offset) {
				encoder = before;
				CHECK(v22bis_symbol_encode(&encoder, bits, (uint8_t *)&encoder + offset) ==
					      V22_FRAMING_INVALID_ARGUMENT &&
				      memcmp(&encoder, &before, sizeof(encoder)) == 0,
				      "encoder output overlap changed state at %u/%u/%zu", ones, phase, offset);
			}
			for (size_t offset = 0; offset < sizeof(bits); ++offset) {
				uint8_t inplace[V22BIS_BITS_PER_SYMBOL];

				encoder = before;
				memcpy(inplace, bits, sizeof(bits));
				CHECK(v22bis_symbol_encode(&encoder, inplace, &inplace[offset]) == V22_FRAMING_OK &&
				      inplace[offset] == expected_symbol &&
				      memcmp(&encoder, &expected, sizeof(encoder)) == 0,
				      "input/output alias changed at %u/%u/%zu", ones, phase, offset);
			}
			for (uint8_t value = 0; value < V22BIS_SYMBOL_COUNT; ++value) {
				struct v22_symbol_decoder finished;

				memset(&initial, 0xa5, sizeof(initial));
				CHECK(v22_symbol_decoder_init(&initial.owner, phase) == V22_FRAMING_OK,
				      "alias decoder did not initialize");
				initial.owner.descrambler.history = V22_SCRAMBLER_HISTORY_MASK;
				initial.owner.descrambler.consecutive_ones = ones;
				finished = initial.owner;
				CHECK(v22bis_symbol_decode(&finished, value, bits) == V22_FRAMING_OK,
				      "ordinary decoding failed");
				for (int offset = 1 - V22BIS_BITS_PER_SYMBOL;
				     offset < (int)sizeof(initial.owner); ++offset) {
					fixture = initial;
					uint8_t *output = (uint8_t *)&fixture +
						offsetof(struct decode_fixture, owner) + offset;

					CHECK(v22bis_symbol_decode(&fixture.owner, value, output) ==
						      V22_FRAMING_INVALID_ARGUMENT &&
					      memcmp(&fixture, &initial, sizeof(fixture)) == 0,
					      "decoder output overlap changed state at %u/%u/%u/%d",
					      ones, phase, value, offset);
				}
				for (unsigned side = 0; side < 2; ++side) {
					fixture = initial;
					uint8_t *output = side ? fixture.suffix : fixture.prefix;

					CHECK(v22bis_symbol_decode(&fixture.owner, value, output) == V22_FRAMING_OK &&
					      memcmp(output, bits, sizeof(bits)) == 0 &&
					      memcmp(&fixture.owner, &finished, sizeof(finished)) == 0,
					      "adjacent decoder output changed state at %u/%u/%u/%u",
					      ones, phase, value, side);
				}
			}
		}
	}
	return true;
}

static bool test_v22bis_owner_rejection(void)
{
	struct v22_symbol_encoder encoder, saved_encoder;
	struct v22_symbol_decoder decoder, saved_decoder;
	const uint8_t input[V22BIS_BITS_PER_SYMBOL] = { 1, 0, 1, 0 };
	const uint8_t sentinel[V22BIS_BITS_PER_SYMBOL] = { 7, 8, 9, 10 };
	uint8_t decoded[V22BIS_BITS_PER_SYMBOL], symbol = UINT8_MAX;

	memcpy(decoded, sentinel, sizeof(decoded));
	for (unsigned field = 0; field < 3; ++field) {
		CHECK(v22_symbol_encoder_init(&encoder, 0) == V22_FRAMING_OK &&
		      v22_symbol_decoder_init(&decoder, 0) == V22_FRAMING_OK,
		      "rejection owners did not initialize");
		if (field == 0) {
			encoder.scrambler.history = decoder.descrambler.history =
				UINT32_C(1) << V22_SCRAMBLER_BITS;
		} else if (field == 1) {
			encoder.scrambler.consecutive_ones =
				decoder.descrambler.consecutive_ones =
					V22_SCRAMBLER_ONES_LIMIT + 1u;
		} else {
			encoder.phase_quadrant = decoder.phase_quadrant = QPSK_QUADRANT_COUNT;
		}
		saved_encoder = encoder;
		saved_decoder = decoder;
		CHECK(v22bis_symbol_encode(&encoder, input, &symbol) ==
			      V22_FRAMING_INVALID_ARGUMENT && symbol == UINT8_MAX &&
		      memcmp(&encoder, &saved_encoder, sizeof(encoder)) == 0 &&
		      v22bis_symbol_decode(&decoder, 0, decoded) ==
			      V22_FRAMING_INVALID_ARGUMENT &&
		      memcmp(decoded, sentinel, sizeof(decoded)) == 0 &&
		      memcmp(&decoder, &saved_decoder, sizeof(decoder)) == 0,
		      "invalid owner field %u changed state or output", field);
	}
	CHECK(v22_symbol_encoder_init(&encoder, 0) == V22_FRAMING_OK &&
	      v22_symbol_decoder_init(&decoder, 0) == V22_FRAMING_OK,
	      "null-argument owners did not initialize");
	saved_encoder = encoder;
	saved_decoder = decoder;
	CHECK(v22bis_symbol_encode(NULL, input, &symbol) == V22_FRAMING_INVALID_ARGUMENT &&
	      v22bis_symbol_encode(&encoder, NULL, &symbol) == V22_FRAMING_INVALID_ARGUMENT &&
	      v22bis_symbol_encode(&encoder, input, NULL) == V22_FRAMING_INVALID_ARGUMENT &&
	      v22bis_symbol_decode(NULL, 0, decoded) == V22_FRAMING_INVALID_ARGUMENT &&
	      v22bis_symbol_decode(&decoder, 0, NULL) == V22_FRAMING_INVALID_ARGUMENT &&
	      memcmp(&encoder, &saved_encoder, sizeof(encoder)) == 0 &&
	      memcmp(&decoder, &saved_decoder, sizeof(decoder)) == 0 &&
	      symbol == UINT8_MAX && memcmp(decoded, sentinel, sizeof(decoded)) == 0,
	      "null symbol arguments changed owner or output");
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
	     index < V22BIS_RATE_SIGNAL_COMPLETE_SYMBOLS - 1u; ++index) {
		uint8_t turn = (index & 1u) == 0 ? 1u : 3u;

		CHECK(v22bis_rate_signal_observe(
			      &detector, turn, &event) == V22_FRAMING_OK,
		      "V.22bis rate signal rejected short symbol %zu", index);
	}
	CHECK(detector.present &&
		      v22bis_rate_signal_observe(&detector, 3, &event) ==
			      V22_FRAMING_OK &&
		      event == V22BIS_RATE_SIGNAL_ENDED && !detector.present,
	      "V.22bis rate signal accepted a marker missing two symbols");

	v22bis_rate_signal_detector_reset(&detector);
	for (size_t index = 0;
	     index < V22BIS_RATE_SIGNAL_COMPLETE_SYMBOLS; ++index) {
		uint8_t turn = (index & 1u) == 0 ? 1u : 3u;

		CHECK(v22bis_rate_signal_observe(
			      &detector, turn, &event) == V22_FRAMING_OK,
		      "V.22bis rate signal rejected boundary symbol %zu", index);
	}
	CHECK(detector.present &&
		      v22bis_rate_signal_observe(&detector, 1, &event) ==
			      V22_FRAMING_OK &&
		      event == V22BIS_RATE_SIGNAL_FULLY_ENDED &&
		      !detector.present,
	      "V.22bis rate signal rejected one missing edge observation");

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

	v22bis_rate_signal_detector_reset(&detector);
	for (size_t index = 0;
	     index < V22BIS_ALTERNATING_RUN_MAX_SYMBOLS + 5u; ++index) {
		uint8_t turn = (index & 1u) == 0 ? 1u : 3u;

		CHECK(v22bis_rate_signal_observe(
			      &detector, turn, &event) == V22_FRAMING_OK,
		      "V.22bis detector rejected extended symbol %zu", index);
	}
	CHECK(detector.present &&
		      detector.alternating_symbols ==
			      V22BIS_ALTERNATING_RUN_MAX_SYMBOLS &&
		      v22bis_rate_signal_observe(&detector, 1, &event) ==
			      V22_FRAMING_OK &&
		      event == V22BIS_RATE_SIGNAL_FULLY_ENDED &&
		      detector.alternating_symbols == 1 && !detector.present,
	      "V.22bis detector did not bound an extended candidate");

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
	detector = saved;
	detector.alternating_symbols =
		V22BIS_ALTERNATING_RUN_MAX_SYMBOLS + 1u;
	event = V22BIS_RATE_SIGNAL_STARTED;
	CHECK(!v22bis_rate_signal_detector_is_valid(&detector) &&
		      v22bis_rate_signal_observe(&detector, 1, &event) ==
			      V22_FRAMING_INVALID_ARGUMENT &&
		      detector.alternating_symbols ==
			      V22BIS_ALTERNATING_RUN_MAX_SYMBOLS + 1u &&
		      event == V22BIS_RATE_SIGNAL_STARTED,
	      "V.22bis detector accepted an oversized candidate");
	return true;
}

static bool test_v22bis_rate_signal_transitions(void)
{
	/* Cover every valid detector state, including saturated marker runs. */
	for (uint8_t count = 0; count <= 64; ++count) {
		for (uint8_t previous = 0; previous < 4; ++previous) {
			for (uint8_t turn = 0; turn < 4; ++turn) {
				struct v22bis_rate_signal_detector detector = {
					.alternating_symbols = count,
					.previous_turn = previous,
					.present = count >= 12,
				};
				bool is_turn = turn == 1 || turn == 3;
				bool continues = is_turn &&
					(count == 0 || turn != previous);
				uint8_t expected_count = continues ?
					(count == 64 ? 64 : count + 1) :
					(is_turn ? 1 : 0);
				enum v22bis_rate_signal_event expected =
					V22BIS_RATE_SIGNAL_NONE;
				enum v22bis_rate_signal_event event =
					V22BIS_RATE_SIGNAL_STARTED;

				if (!continues && count >= 12)
					expected = count >= 59 ?
						V22BIS_RATE_SIGNAL_FULLY_ENDED :
						V22BIS_RATE_SIGNAL_ENDED;
				else if (continues && count == 11)
					expected = V22BIS_RATE_SIGNAL_STARTED;
				CHECK(v22bis_rate_signal_observe(
					      &detector, turn, &event) ==
					      V22_FRAMING_OK &&
					      v22bis_rate_signal_detector_is_valid(&detector) &&
					      detector.alternating_symbols == expected_count &&
					      detector.previous_turn == turn &&
					      detector.present == (expected_count >= 12) &&
					      event == expected,
				      "V.22bis marker transition changed at %u/%u/%u",
				      count, previous, turn);
			}
		}
	}
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
			      &encoder, input[index], &encoded, true) ==
			      V22_FRAMING_OK &&
			      v22_descramble_bit(
				      &decoder, encoded, &output[index], true) ==
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

static bool test_framing_output_ownership(void)
{
	struct bit_fixture {
		uint8_t prefix;
		struct v22_scrambler owner;
		uint8_t suffix;
	} bit_initial, bit_fixture;
	struct encode_fixture {
		uint8_t prefix;
		struct v22_symbol_encoder owner;
		uint8_t suffix;
	} encode_initial, encode_fixture;
	struct decode_fixture {
		uint8_t prefix[2];
		struct v22_symbol_decoder owner;
		uint8_t suffix[2];
	} decode_initial, decode_fixture;

	for (unsigned repair = 0; repair < 2; ++repair) {
		for (unsigned descramble = 0; descramble < 2; ++descramble) {
			for (uint8_t input = 0; input < 2; ++input) {
				memset(&bit_initial, 0xa5, sizeof(bit_initial));
				v22_scrambler_reset(&bit_initial.owner);
				bit_initial.owner.history = V22_SCRAMBLER_HISTORY_MASK;
				bit_initial.owner.consecutive_ones = V22_SCRAMBLER_ONES_LIMIT;
				struct v22_scrambler expected = bit_initial.owner;
				uint8_t output;
				enum v22_framing_result (*transform)(struct v22_scrambler *,
					uint8_t, uint8_t *, bool) = descramble ?
					v22_descramble_bit : v22_scramble_bit;

				CHECK(transform(&expected, input, &output, repair) == V22_FRAMING_OK,
				      "disjoint bit output failed");
				for (size_t offset = 0; offset < sizeof(bit_initial.owner); ++offset) {
					memcpy(&bit_fixture, &bit_initial, sizeof(bit_fixture));
					CHECK(transform(&bit_fixture.owner, input,
						      (uint8_t *)&bit_fixture.owner + offset, repair) ==
						      V22_FRAMING_INVALID_ARGUMENT &&
					      memcmp(&bit_fixture, &bit_initial, sizeof(bit_fixture)) == 0,
					      "bit output overlap changed storage at %u/%u/%u/%zu",
					      repair, descramble, input, offset);
				}
				for (unsigned side = 0; side < 2; ++side) {
					memcpy(&bit_fixture, &bit_initial, sizeof(bit_fixture));
					uint8_t *adjacent = side ? &bit_fixture.suffix :
						(uint8_t *)&bit_fixture + offsetof(struct bit_fixture, owner) - 1;

					CHECK(transform(&bit_fixture.owner, input, adjacent, repair) == V22_FRAMING_OK &&
					      *adjacent == output && bit_fixture.owner.history == expected.history &&
					      bit_fixture.owner.consecutive_ones == expected.consecutive_ones,
					      "adjacent bit output changed result");
				}
			}
		}
		for (unsigned rate600 = 0; rate600 < 2; ++rate600) {
			for (uint8_t phase = 0; phase < QPSK_QUADRANT_COUNT; ++phase) {
				for (uint8_t input = 0; input < (rate600 ? 2u : 4u); ++input) {
					memset(&encode_initial, 0xa5, sizeof(encode_initial));
					CHECK(v22_symbol_encoder_init(&encode_initial.owner, phase) == V22_FRAMING_OK,
					      "ownership encoder did not initialize");
					encode_initial.owner.scrambler.history = V22_SCRAMBLER_HISTORY_MASK;
					encode_initial.owner.scrambler.consecutive_ones = V22_SCRAMBLER_ONES_LIMIT;
					struct v22_symbol_encoder expected = encode_initial.owner;
					uint8_t symbol;
					enum v22_framing_result normal = rate600 ?
						v22_600_symbol_encode(&expected, input, &symbol, repair) :
						v22_symbol_encode(&expected, input >> 1, input & 1u, &symbol, repair);

					CHECK(normal == V22_FRAMING_OK, "disjoint encoder output failed");
					for (size_t offset = 0; offset < sizeof(encode_initial.owner); ++offset) {
						memcpy(&encode_fixture, &encode_initial, sizeof(encode_fixture));
						uint8_t *output = (uint8_t *)&encode_fixture.owner + offset;
						enum v22_framing_result result = rate600 ?
							v22_600_symbol_encode(&encode_fixture.owner, input, output, repair) :
							v22_symbol_encode(&encode_fixture.owner, input >> 1, input & 1u, output, repair);

						CHECK(result == V22_FRAMING_INVALID_ARGUMENT &&
						      memcmp(&encode_fixture, &encode_initial, sizeof(encode_fixture)) == 0,
						      "symbol encoder overlap changed storage at %u/%u/%u/%u/%zu",
						      repair, rate600, phase, input, offset);
					}
					for (unsigned side = 0; side < 2; ++side) {
						memcpy(&encode_fixture, &encode_initial, sizeof(encode_fixture));
						uint8_t *output = side ? &encode_fixture.suffix :
							(uint8_t *)&encode_fixture + offsetof(struct encode_fixture, owner) - 1;
						enum v22_framing_result result = rate600 ?
							v22_600_symbol_encode(&encode_fixture.owner, input, output, repair) :
							v22_symbol_encode(&encode_fixture.owner, input >> 1, input & 1u, output, repair);

						CHECK(result == V22_FRAMING_OK && *output == symbol &&
						      encode_fixture.owner.scrambler.history == expected.scrambler.history &&
						      encode_fixture.owner.scrambler.consecutive_ones == expected.scrambler.consecutive_ones &&
						      encode_fixture.owner.phase_quadrant == expected.phase_quadrant,
						      "adjacent encoder output changed result");
					}
				}
				for (uint8_t quadrant = 0; quadrant < QPSK_QUADRANT_COUNT; ++quadrant) {
					if (rate600 && ((quadrant - phase) & 1u) == 0)
						continue;
					memset(&decode_initial, 0xa5, sizeof(decode_initial));
					CHECK(v22_symbol_decoder_init(&decode_initial.owner, phase) == V22_FRAMING_OK,
					      "ownership decoder did not initialize");
					decode_initial.owner.descrambler.history = V22_SCRAMBLER_HISTORY_MASK;
					decode_initial.owner.descrambler.consecutive_ones = V22_SCRAMBLER_ONES_LIMIT;
					struct v22_symbol_decoder expected = decode_initial.owner;
					uint8_t bits[2];
					enum v22_framing_result normal = rate600 ?
						v22_600_symbol_decode(&expected, quadrant, &bits[0], repair) :
						v22_symbol_decode(&expected, quadrant, bits, repair);

					CHECK(normal == V22_FRAMING_OK, "disjoint decoder outputs failed");
					/* Include an array starting just before the owner: only its
					 * second byte overlaps. Scalar outputs start at the owner. */
					for (size_t offset = rate600 ? 1u : 0u;
					     offset <= sizeof(decode_initial.owner); ++offset) {
						memcpy(&decode_fixture, &decode_initial, sizeof(decode_fixture));
						uint8_t *overlap = (uint8_t *)&decode_fixture +
							offsetof(struct decode_fixture, owner) + offset - 1u;
						enum v22_framing_result result = rate600 ?
							v22_600_symbol_decode(&decode_fixture.owner, quadrant, overlap, repair) :
							v22_symbol_decode(&decode_fixture.owner, quadrant, overlap, repair);

						CHECK(result == V22_FRAMING_INVALID_ARGUMENT &&
						      memcmp(&decode_fixture, &decode_initial, sizeof(decode_fixture)) == 0,
						      "symbol decoder overlap changed storage at %u/%u/%u/%u/%zu",
						      repair, rate600, phase, quadrant, offset);
					}
					for (unsigned side = 0; side < 2; ++side) {
						memcpy(&decode_fixture, &decode_initial, sizeof(decode_fixture));
						uint8_t *output = side ? decode_fixture.suffix :
							(uint8_t *)&decode_fixture + offsetof(struct decode_fixture, owner) - 2;
						enum v22_framing_result result = rate600 ?
							v22_600_symbol_decode(&decode_fixture.owner, quadrant, output, repair) :
							v22_symbol_decode(&decode_fixture.owner, quadrant, output, repair);

						CHECK(result == V22_FRAMING_OK &&
						      memcmp(output, bits, rate600 ? 1u : 2u) == 0 &&
						      decode_fixture.owner.descrambler.history == expected.descrambler.history &&
						      decode_fixture.owner.descrambler.consecutive_ones == expected.descrambler.consecutive_ones &&
						      decode_fixture.owner.phase_quadrant == expected.phase_quadrant,
						      "adjacent decoder outputs changed result");
					}
				}
			}
		}
	}
	return true;
}


static bool test_validation(void)
{
	struct v22_scrambler scrambler = { 0 };
	struct v22_symbol_encoder encoder;
	struct v22_symbol_decoder decoder;
	uint8_t value = 0;
	uint8_t bits[2] = { 0 };

	CHECK(v22_scramble_bit(NULL, 0, &value, true) ==
		      V22_FRAMING_INVALID_ARGUMENT &&
		      v22_scramble_bit(&scrambler, 2, &value, true) ==
			      V22_FRAMING_INVALID_ARGUMENT &&
		      v22_descramble_bit(&scrambler, 0, NULL, true) ==
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
	CHECK(v22_symbol_encode(&encoder, 0, 0, &value, true) ==
		      V22_FRAMING_INVALID_ARGUMENT &&
		      v22_symbol_decode(
			      &decoder, 0, bits, true) ==
			      V22_FRAMING_INVALID_ARGUMENT,
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
		{ "training scrambler policy", test_training_scrambler_policy },
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
		{ "V.22bis symbol aliasing", test_v22bis_symbol_aliasing },
		{ "V.22bis owner rejection", test_v22bis_owner_rejection },
		{ "V.22bis rate-signal generation and detection",
		  test_v22bis_rate_signal_detection },
		{ "V.22bis complete rate-signal transitions",
		  test_v22bis_rate_signal_transitions },
		{ "descrambler resynchronization",
		  test_descrambler_resynchronization },
		{ "framing output ownership", test_framing_output_ownership },
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
