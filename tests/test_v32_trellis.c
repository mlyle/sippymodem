// SPDX-License-Identifier: BSD-3-Clause

#include <trellis.h>

#include "test.h"

#include <string.h>

struct trellis_fixture {
	uint8_t data_bits;
	uint8_t input[16];
	uint8_t signal_symbol[16];
	uint8_t final_state;
	uint8_t final_quadrant;
};

static bool test_normative_sequences(void)
{
	static const struct trellis_fixture fixtures[] = {
		{
			3,
			{ 3, 0, 2, 5, 6, 7, 4, 1, 0, 3, 1, 2, 4, 5, 7, 6 },
			{ 3, 10, 0, 13, 0, 7, 0, 1,
			  0, 3, 11, 0, 12, 3, 13, 8 },
			5,
			0,
		},
		{
			4,
			{ 4, 9, 3, 14, 7, 8, 13, 2, 9, 4, 2, 3, 13, 14, 8, 7 },
			{ 4, 29, 31, 6, 19, 8, 1, 2,
			  9, 12, 14, 31, 5, 10, 4, 3 },
			4,
			0,
		},
		{
			5,
			{ 5, 10, 20, 15, 8, 25, 30, 3,
			  10, 5, 3, 20, 30, 15, 25, 8 },
			{ 5, 10, 60, 55, 56, 9, 54, 51,
			  26, 29, 27, 36, 30, 55, 1, 8 },
			2,
			1,
		},
		{
			6,
			{ 6, 11, 21, 16, 41, 26, 31, 36,
			  11, 6, 36, 21, 31, 16, 26, 41 },
			{ 6, 11, 21, 64, 105, 58, 47, 84,
			  27, 22, 116, 101, 127, 32, 58, 73 },
			1,
			0,
		},
	};

	for (size_t fixture = 0; fixture < ARRAY_LENGTH(fixtures); ++fixture) {
		struct v32_trellis_encoder encoder;
		const struct trellis_fixture *expected = &fixtures[fixture];

		CHECK(v32_trellis_encoder_init(
			      &encoder, expected->data_bits, 0) ==
			      V32_TRELLIS_OK,
		      "V.32 trellis fixture %zu did not initialize", fixture);
		for (size_t index = 0; index < ARRAY_LENGTH(expected->input);
		     ++index) {
			uint8_t signal_symbol = UINT8_MAX;

			CHECK(v32_trellis_encode(
				      &encoder, expected->input[index],
				      &signal_symbol) ==
				      V32_TRELLIS_OK &&
			      signal_symbol == expected->signal_symbol[index] &&
			      signal_symbol <
				      (UINT8_C(1) <<
				       (expected->data_bits + 1u)) &&
			      v32_trellis_encoder_is_valid(&encoder),
			      "V.32 trellis fixture %zu changed at symbol %zu",
			      fixture, index);
		}
		CHECK(encoder.trellis_state == expected->final_state &&
			      encoder.differential_output ==
				      expected->final_quadrant,
		      "V.32 trellis fixture %zu ended in the wrong state",
		      fixture);
	}
	return true;
}

static bool test_initial_differential_output(void)
{
	static const uint8_t expected[] = { 0, 2, 4, 6 };

	for (uint8_t initial = 0; initial < ARRAY_LENGTH(expected); ++initial) {
		struct v32_trellis_encoder encoder;
		uint8_t signal_symbol = UINT8_MAX;

		CHECK(v32_trellis_encoder_init(&encoder, 3, initial) ==
			      V32_TRELLIS_OK &&
			      v32_trellis_encode(
				      &encoder, 0, &signal_symbol) ==
				      V32_TRELLIS_OK &&
			      signal_symbol == expected[initial],
		      "V.32 initial differential output %u changed", initial);
	}
	return true;
}

static bool test_validation_is_transactional(void)
{
	struct v32_trellis_encoder encoder = {
		.trellis_state = 2,
		.differential_output = 1,
		.data_bits_per_symbol = 4,
	};
	struct v32_trellis_encoder saved = encoder;
	uint8_t signal_symbol = 0xa5;

	CHECK(!v32_trellis_encoder_is_valid(NULL) &&
		      v32_trellis_encoder_init(NULL, 3, 0) ==
			      V32_TRELLIS_INVALID_ARGUMENT &&
		      v32_trellis_encoder_init(&encoder, 2, 0) ==
			      V32_TRELLIS_INVALID_ARGUMENT &&
		      memcmp(&encoder, &saved, sizeof(encoder)) == 0 &&
		      v32_trellis_encoder_init(&encoder, 7, 0) ==
			      V32_TRELLIS_INVALID_ARGUMENT &&
		      memcmp(&encoder, &saved, sizeof(encoder)) == 0 &&
		      v32_trellis_encoder_init(&encoder, 3, 4) ==
			      V32_TRELLIS_INVALID_ARGUMENT &&
		      memcmp(&encoder, &saved, sizeof(encoder)) == 0,
	      "V.32 trellis initialization accepted invalid state");

	CHECK(v32_trellis_encode(&encoder, 16, &signal_symbol) ==
			      V32_TRELLIS_INVALID_ARGUMENT &&
		      signal_symbol == 0xa5 &&
		      memcmp(&encoder, &saved, sizeof(encoder)) == 0 &&
		      v32_trellis_encode(&encoder, 0, NULL) ==
			      V32_TRELLIS_INVALID_ARGUMENT &&
		      memcmp(&encoder, &saved, sizeof(encoder)) == 0,
	      "V.32 trellis encoding changed state on invalid input");

	encoder = saved;
	encoder.trellis_state = V32_TRELLIS_STATE_COUNT;
	CHECK(!v32_trellis_encoder_is_valid(&encoder),
	      "V.32 trellis accepted an invalid convolutional state");
	encoder = saved;
	encoder.differential_output =
		V32_TRELLIS_QUADRANT_COUNT;
	CHECK(!v32_trellis_encoder_is_valid(&encoder),
	      "V.32 trellis accepted an invalid differential output");
	encoder = saved;
	encoder.data_bits_per_symbol = 0;
	CHECK(!v32_trellis_encoder_is_valid(&encoder) &&
		      strcmp(v32_trellis_result_name(
			     V32_TRELLIS_INVALID_ARGUMENT),
			     "invalid argument") == 0 &&
		      strcmp(v32_trellis_result_name(
			     (enum v32_trellis_result)99),
			     "unknown") == 0,
	      "V.32 trellis validation diagnostics changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "normative sequences", test_normative_sequences },
		{ "initial differential output",
		  test_initial_differential_output },
		{ "transactional validation", test_validation_is_transactional },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (!tests[index].run()) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0 ? 0 : 1;
}
