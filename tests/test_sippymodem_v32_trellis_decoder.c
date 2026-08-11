// SPDX-License-Identifier: BSD-3-Clause

#include <constellation.h>
#include <trellis.h>

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

static double fixture_noise(uint32_t *random, double span)
{
	double unit = (double)(fixture_random(random) & UINT32_C(0xffff)) /
		      65535.0;

	return (unit - 0.5) * span;
}

static bool nearest_signal_symbol(
	uint8_t data_bits_per_symbol,
	const struct sippymodem_qam_point *point, uint8_t *nearest)
{
	double best_error = DBL_MAX;
	uint8_t signal_points =
		(uint8_t)(UINT8_C(1) << (data_bits_per_symbol + 1u));

	for (uint8_t symbol = 0; symbol < signal_points; ++symbol) {
		struct sippymodem_qam_point candidate;
		double in_phase;
		double quadrature;
		double error;

		if (sippymodem_v32_trellis_constellation_point(
			    data_bits_per_symbol, symbol, &candidate) !=
		    SIPPYMODEM_V32_CONSTELLATION_OK)
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

static bool check_round_trip(
	uint8_t data_bits_per_symbol, uint8_t initial_phase, bool add_noise)
{
	struct sippymodem_v32_trellis_encoder encoder;
	struct sippymodem_v32_trellis_decoder decoder;
	uint8_t input[TEST_SYMBOLS];
	uint32_t random = UINT32_C(0x72003210) + data_bits_per_symbol * 17u +
			  initial_phase + add_noise;
	unsigned int hard_decision_errors = 0;
	uint8_t data_mask =
		(uint8_t)((UINT8_C(1) << data_bits_per_symbol) - 1u);
	double noise_span =
		sqrt((double)(1u << (7u - data_bits_per_symbol))) /
		(2.0 * sqrt(18.0)) * 1.02;

	CHECK(sippymodem_v32_trellis_encoder_init(
		      &encoder, data_bits_per_symbol, initial_phase) ==
		      SIPPYMODEM_V32_TRELLIS_OK &&
		      sippymodem_v32_trellis_decoder_init(
			      &decoder, data_bits_per_symbol, initial_phase) ==
		      SIPPYMODEM_V32_TRELLIS_OK,
	      "V.32bis %u-bit endpoints did not initialize",
	      data_bits_per_symbol);
	for (size_t index = 0; index < ARRAY_LENGTH(input); ++index) {
		struct sippymodem_qam_point point;
		uint8_t decoded = UINT8_MAX;
		uint8_t signal_symbol;
		bool ready = true;

		input[index] = (uint8_t)(fixture_random(&random) & data_mask);
		CHECK(sippymodem_v32_trellis_encode(
			      &encoder, input[index], &signal_symbol) ==
			      SIPPYMODEM_V32_TRELLIS_OK &&
			      sippymodem_v32_trellis_constellation_point(
				      data_bits_per_symbol, signal_symbol, &point) ==
				      SIPPYMODEM_V32_CONSTELLATION_OK,
		      "V.32bis %u-bit fixture failed at %zu",
		      data_bits_per_symbol, index);
		if (add_noise) {
			uint8_t nearest = 0;

			point.in_phase +=
				fixture_noise(&random, noise_span);
			point.quadrature +=
				fixture_noise(&random, noise_span);
			CHECK(nearest_signal_symbol(
				      data_bits_per_symbol, &point, &nearest),
			      "V.32bis %u-bit hard decision failed at %zu",
			      data_bits_per_symbol, index);
			if (nearest != signal_symbol)
				hard_decision_errors++;
		}
		CHECK(sippymodem_v32_trellis_decode(
			      &decoder, &point, &decoded, &ready) ==
			      SIPPYMODEM_V32_TRELLIS_OK &&
			      ready ==
				      (index + 1 >=
				       SIPPYMODEM_V32_TRELLIS_TRACEBACK_SYMBOLS),
		      "V.32bis %u-bit path timing changed at %zu",
		      data_bits_per_symbol, index);
		if (ready) {
			size_t expected =
				index + 1 -
				SIPPYMODEM_V32_TRELLIS_TRACEBACK_SYMBOLS;

			CHECK(decoded == input[expected],
			      "V.32bis %u-bit decoded %u instead of %u at %zu",
			      data_bits_per_symbol, decoded, input[expected], index);
		} else {
			CHECK(decoded == UINT8_MAX,
			      "V.32bis %u-bit changed output before traceback",
			      data_bits_per_symbol);
		}
	}
	CHECK(sippymodem_v32_trellis_encoder_is_valid(&encoder) &&
		      sippymodem_v32_trellis_decoder_is_valid(&decoder) &&
		      decoder.data_bits_per_symbol == data_bits_per_symbol &&
		      encoder.carrier_phase_quadrant ==
			      decoder.carrier_phase_quadrant,
	      "V.32bis %u-bit endpoints ended with inconsistent state",
	      data_bits_per_symbol);
	CHECK(!add_noise || hard_decision_errors > 0,
	      "V.32bis %u-bit noise never exercised path correction",
	      data_bits_per_symbol);
	return true;
}

static bool test_noiseless_round_trip(void)
{
	for (uint8_t data_bits = SIPPYMODEM_V32_TRELLIS_MIN_DATA_BITS;
	     data_bits <= SIPPYMODEM_V32_TRELLIS_MAX_DATA_BITS; ++data_bits) {
		for (uint8_t phase = 0;
		     phase < SIPPYMODEM_V32_TRELLIS_QUADRANT_COUNT; ++phase) {
			if (!check_round_trip(data_bits, phase, false))
				return false;
		}
	}
	return true;
}

static bool test_noisy_round_trip(void)
{
	for (uint8_t data_bits = SIPPYMODEM_V32_TRELLIS_MIN_DATA_BITS;
	     data_bits <= SIPPYMODEM_V32_TRELLIS_MAX_DATA_BITS; ++data_bits) {
		for (uint8_t phase = 0;
		     phase < SIPPYMODEM_V32_TRELLIS_QUADRANT_COUNT; ++phase) {
			if (!check_round_trip(data_bits, phase, true))
				return false;
		}
	}
	return true;
}

static bool test_validation_is_transactional(void)
{
	struct sippymodem_v32_trellis_decoder decoder;
	struct sippymodem_v32_trellis_decoder saved;
	struct sippymodem_qam_point point = { 0.25, -0.5 };
	struct sippymodem_qam_point invalid = { NAN, 0.0 };
	uint8_t output = 0xa5;
	bool ready = true;

	CHECK(sippymodem_v32_trellis_decoder_init(
		      &decoder, SIPPYMODEM_V32_7200_DATA_BITS, 2) ==
		      SIPPYMODEM_V32_TRELLIS_OK,
	      "V.32bis validation fixture did not initialize");
	saved = decoder;
	CHECK(sippymodem_v32_trellis_decoder_is_valid(&decoder) &&
		      !sippymodem_v32_trellis_decoder_is_valid(NULL) &&
		      sippymodem_v32_trellis_decoder_init(NULL, 3, 0) ==
			      SIPPYMODEM_V32_TRELLIS_INVALID_ARGUMENT &&
		      sippymodem_v32_trellis_decoder_init(&decoder, 2, 0) ==
			      SIPPYMODEM_V32_TRELLIS_INVALID_ARGUMENT &&
		      sippymodem_v32_trellis_decoder_init(&decoder, 7, 0) ==
			      SIPPYMODEM_V32_TRELLIS_INVALID_ARGUMENT &&
		      sippymodem_v32_trellis_decoder_init(&decoder, 3, 4) ==
			      SIPPYMODEM_V32_TRELLIS_INVALID_ARGUMENT &&
		      memcmp(&decoder, &saved, sizeof(decoder)) == 0 &&
		      sippymodem_v32_trellis_decode(
			      NULL, &point, &output, &ready) ==
			      SIPPYMODEM_V32_TRELLIS_INVALID_ARGUMENT &&
		      sippymodem_v32_trellis_decode(
			      &decoder, NULL, &output, &ready) ==
			      SIPPYMODEM_V32_TRELLIS_INVALID_ARGUMENT &&
		      sippymodem_v32_trellis_decode(
			      &decoder, &invalid, &output, &ready) ==
			      SIPPYMODEM_V32_TRELLIS_INVALID_ARGUMENT &&
		      sippymodem_v32_trellis_decode(
			      &decoder, &point, NULL, &ready) ==
			      SIPPYMODEM_V32_TRELLIS_INVALID_ARGUMENT &&
		      sippymodem_v32_trellis_decode(
			      &decoder, &point, &output, NULL) ==
			      SIPPYMODEM_V32_TRELLIS_INVALID_ARGUMENT &&
		      memcmp(&decoder, &saved, sizeof(decoder)) == 0 &&
		      output == 0xa5 && ready,
	      "V.32bis invalid input changed decoder state");

	decoder = saved;
	decoder.path_metric[0] = 1.0;
	CHECK(!sippymodem_v32_trellis_decoder_is_valid(&decoder),
	      "V.32bis accepted unnormalized metrics");
	decoder = saved;
	decoder.path[0].predecessor[0] =
		SIPPYMODEM_V32_TRELLIS_STATE_COUNT;
	CHECK(!sippymodem_v32_trellis_decoder_is_valid(&decoder),
	      "V.32bis accepted an invalid predecessor");
	decoder = saved;
	decoder.path[0].differential_symbol[0] =
		UINT8_C(1) << decoder.data_bits_per_symbol;
	CHECK(!sippymodem_v32_trellis_decoder_is_valid(&decoder),
	      "V.32bis accepted an out-of-range path symbol");
	decoder = saved;
	decoder.path_symbols =
		SIPPYMODEM_V32_TRELLIS_TRACEBACK_SYMBOLS + 1u;
	CHECK(!sippymodem_v32_trellis_decoder_is_valid(&decoder),
	      "V.32bis accepted invalid traceback occupancy");
	decoder = saved;
	decoder.path_index = 1;
	CHECK(!sippymodem_v32_trellis_decoder_is_valid(&decoder),
	      "V.32bis accepted inconsistent traceback indexes");
	decoder = saved;
	decoder.data_bits_per_symbol = 2;
	CHECK(!sippymodem_v32_trellis_decoder_is_valid(&decoder),
	      "V.32bis accepted an invalid data-group width");
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
