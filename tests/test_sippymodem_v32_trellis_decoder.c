// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v32_constellation.h>
#include <sippymodem_v32_trellis.h>

#include "test.h"

#include <float.h>
#include <math.h>
#include <string.h>

enum {
	TEST_SYMBOLS = 1024,
};

static uint32_t fixture_random(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static double fixture_noise(uint32_t *random)
{
	double unit = (double)(fixture_random(random) & UINT32_C(0xffff)) /
		      65535.0;

	return (unit - 0.5) * 0.48;
}

static bool nearest_signal_symbol(
	const struct sippymodem_qam_point *point, uint8_t *nearest)
{
	double best_error = DBL_MAX;

	for (uint8_t symbol = 0;
	     symbol < SIPPYMODEM_V32_7200_SIGNAL_POINTS; ++symbol) {
		struct sippymodem_qam_point candidate;
		double in_phase;
		double quadrature;
		double error;

		if (sippymodem_v32_7200_constellation_point(
			    symbol, &candidate) != SIPPYMODEM_V32_CONSTELLATION_OK)
			return false;
		in_phase = point->in_phase - candidate.in_phase;
		quadrature = point->quadrature - candidate.quadrature;
		error = in_phase * in_phase + quadrature * quadrature;
		if (error < best_error) {
			best_error = error;
			*nearest = symbol;
		}
	}
	return best_error < DBL_MAX;
}

static bool check_round_trip(uint8_t initial_phase, bool add_noise)
{
	struct sippymodem_v32_trellis_encoder encoder;
	struct sippymodem_v32_7200_decoder decoder;
	uint8_t input[TEST_SYMBOLS];
	uint32_t random = UINT32_C(0x72003210) + initial_phase + add_noise;
	unsigned int hard_decision_errors = 0;

	CHECK(sippymodem_v32_trellis_encoder_init(
		      &encoder, SIPPYMODEM_V32_7200_DATA_BITS, initial_phase) ==
		      SIPPYMODEM_V32_TRELLIS_OK &&
		      sippymodem_v32_7200_decoder_init(&decoder, initial_phase) ==
		      SIPPYMODEM_V32_TRELLIS_OK,
	      "V.32bis 7200 endpoints did not initialize");
	for (size_t index = 0; index < ARRAY_LENGTH(input); ++index) {
		struct sippymodem_qam_point point;
		uint8_t decoded = UINT8_MAX;
		uint8_t signal_symbol;
		bool ready = true;

		input[index] = (uint8_t)(fixture_random(&random) & 7u);
		CHECK(sippymodem_v32_trellis_encode(
			      &encoder, input[index], &signal_symbol) ==
			      SIPPYMODEM_V32_TRELLIS_OK &&
			      sippymodem_v32_7200_constellation_point(
				      signal_symbol, &point) ==
				      SIPPYMODEM_V32_CONSTELLATION_OK,
		      "V.32bis 7200 fixture failed at %zu", index);
		if (add_noise) {
			uint8_t nearest = 0;

			point.in_phase += fixture_noise(&random);
			point.quadrature += fixture_noise(&random);
			CHECK(nearest_signal_symbol(&point, &nearest),
			      "V.32bis 7200 hard decision failed at %zu", index);
			if (nearest != signal_symbol)
				hard_decision_errors++;
		}
		CHECK(sippymodem_v32_7200_decode(
			      &decoder, &point, &decoded, &ready) ==
			      SIPPYMODEM_V32_TRELLIS_OK &&
			      ready ==
				      (index + 1 >=
				       SIPPYMODEM_V32_TRELLIS_TRACEBACK_SYMBOLS),
		      "V.32bis 7200 path timing changed at %zu", index);
		if (ready) {
			size_t expected =
				index + 1 -
				SIPPYMODEM_V32_TRELLIS_TRACEBACK_SYMBOLS;

			CHECK(decoded == input[expected],
			      "V.32bis 7200 decoded %u instead of %u at %zu",
			      decoded, input[expected], index);
		} else {
			CHECK(decoded == UINT8_MAX,
			      "V.32bis 7200 changed output before traceback");
		}
	}
	CHECK(sippymodem_v32_trellis_encoder_is_valid(&encoder) &&
		      sippymodem_v32_7200_decoder_is_valid(&decoder) &&
		      encoder.carrier_phase_quadrant ==
			      decoder.carrier_phase_quadrant,
	      "V.32bis 7200 endpoints ended with inconsistent state");
	CHECK(!add_noise || hard_decision_errors > 0,
	      "V.32bis 7200 noise never exercised path correction");
	return true;
}

static bool test_noiseless_round_trip(void)
{
	for (uint8_t phase = 0;
	     phase < SIPPYMODEM_V32_TRELLIS_QUADRANT_COUNT; ++phase) {
		if (!check_round_trip(phase, false))
			return false;
	}
	return true;
}

static bool test_noisy_round_trip(void)
{
	for (uint8_t phase = 0;
	     phase < SIPPYMODEM_V32_TRELLIS_QUADRANT_COUNT; ++phase) {
		if (!check_round_trip(phase, true))
			return false;
	}
	return true;
}

static bool test_validation_is_transactional(void)
{
	struct sippymodem_v32_7200_decoder decoder;
	struct sippymodem_v32_7200_decoder saved;
	struct sippymodem_qam_point point = { 0.25, -0.5 };
	struct sippymodem_qam_point invalid = { NAN, 0.0 };
	uint8_t output = 0xa5;
	bool ready = true;

	CHECK(sippymodem_v32_7200_decoder_init(&decoder, 2) ==
		      SIPPYMODEM_V32_TRELLIS_OK,
	      "V.32bis 7200 validation fixture did not initialize");
	saved = decoder;
	CHECK(sippymodem_v32_7200_decoder_is_valid(&decoder) &&
		      !sippymodem_v32_7200_decoder_is_valid(NULL) &&
		      sippymodem_v32_7200_decoder_init(NULL, 0) ==
			      SIPPYMODEM_V32_TRELLIS_INVALID_ARGUMENT &&
		      sippymodem_v32_7200_decoder_init(&decoder, 4) ==
			      SIPPYMODEM_V32_TRELLIS_INVALID_ARGUMENT &&
		      memcmp(&decoder, &saved, sizeof(decoder)) == 0 &&
		      sippymodem_v32_7200_decode(
			      NULL, &point, &output, &ready) ==
			      SIPPYMODEM_V32_TRELLIS_INVALID_ARGUMENT &&
		      sippymodem_v32_7200_decode(
			      &decoder, NULL, &output, &ready) ==
			      SIPPYMODEM_V32_TRELLIS_INVALID_ARGUMENT &&
		      sippymodem_v32_7200_decode(
			      &decoder, &invalid, &output, &ready) ==
			      SIPPYMODEM_V32_TRELLIS_INVALID_ARGUMENT &&
		      sippymodem_v32_7200_decode(
			      &decoder, &point, NULL, &ready) ==
			      SIPPYMODEM_V32_TRELLIS_INVALID_ARGUMENT &&
		      sippymodem_v32_7200_decode(
			      &decoder, &point, &output, NULL) ==
			      SIPPYMODEM_V32_TRELLIS_INVALID_ARGUMENT &&
		      memcmp(&decoder, &saved, sizeof(decoder)) == 0 &&
		      output == 0xa5 && ready,
	      "V.32bis 7200 invalid input changed decoder state");

	decoder = saved;
	decoder.path_metric[0] = 1.0;
	CHECK(!sippymodem_v32_7200_decoder_is_valid(&decoder),
	      "V.32bis 7200 accepted unnormalized metrics");
	decoder = saved;
	decoder.path[0].predecessor[0] =
		SIPPYMODEM_V32_TRELLIS_STATE_COUNT;
	CHECK(!sippymodem_v32_7200_decoder_is_valid(&decoder),
	      "V.32bis 7200 accepted an invalid predecessor");
	decoder = saved;
	decoder.path_symbols =
		SIPPYMODEM_V32_TRELLIS_TRACEBACK_SYMBOLS + 1u;
	CHECK(!sippymodem_v32_7200_decoder_is_valid(&decoder),
	      "V.32bis 7200 accepted invalid traceback occupancy");
	decoder = saved;
	decoder.path_index = 1;
	CHECK(!sippymodem_v32_7200_decoder_is_valid(&decoder),
	      "V.32bis 7200 accepted inconsistent traceback indexes");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "noiseless path recovery", test_noiseless_round_trip },
		{ "noisy path recovery", test_noisy_round_trip },
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
