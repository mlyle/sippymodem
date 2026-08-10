// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v34_data_carrier.h>
#include <sippymodem_v34_data_line_transmitter.h>
#include <sippymodem_v34_power_calibration.h>

#include "test.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const struct sippymodem_v34_q14_coefficient COEFFICIENTS[] = {
	{ 4096, 2048 },
	{ -2048, 1024 },
	{ 1024, -2048 },
};

struct carrier_fixture {
	struct sippymodem_v34_phase3_profile profile;
	struct sippymodem_v34_frame_geometry geometry;
	struct sippymodem_v34_mapping_parameters parameters;
	struct sippymodem_v34_frame_schedule schedule;
	struct sippymodem_v34_scrambler scrambler;
	struct sippymodem_v34_4d_encoder encoder;
	struct sippymodem_v34_power_normalizer normalizer;
	struct sippymodem_v34_transmitted_mapping_frame frame;
};

struct exact_profile_rates {
	uint32_t symbol_numerator;
	uint32_t symbol_denominator;
	uint32_t carrier_numerator;
	uint32_t carrier_denominator;
};

static struct exact_profile_rates profile_rates(
	enum sippymodem_v34_symbol_rate rate, bool high_carrier)
{
	switch (rate) {
	case SIPPYMODEM_V34_SYMBOL_RATE_2400:
		return (struct exact_profile_rates){
			2400, 1, high_carrier ? 1800u : 1600u, 1,
		};
	case SIPPYMODEM_V34_SYMBOL_RATE_2743:
		return (struct exact_profile_rates){
			19200, 7, high_carrier ? 12800u : 11520u, 7,
		};
	case SIPPYMODEM_V34_SYMBOL_RATE_2800:
		return (struct exact_profile_rates){
			2800, 1, high_carrier ? 5600u : 1680u,
			high_carrier ? 3u : 1u,
		};
	case SIPPYMODEM_V34_SYMBOL_RATE_3000:
		return (struct exact_profile_rates){
			3000, 1, high_carrier ? 2000u : 1800u, 1,
		};
	case SIPPYMODEM_V34_SYMBOL_RATE_3200:
		return (struct exact_profile_rates){
			3200, 1, high_carrier ? 1920u : 12800u,
			high_carrier ? 1u : 7u,
		};
	case SIPPYMODEM_V34_SYMBOL_RATE_3429:
		return (struct exact_profile_rates){ 24000, 7, 96000, 49 };
	case SIPPYMODEM_V34_SYMBOL_RATE_COUNT:
		break;
	}
	return (struct exact_profile_rates){ 0 };
}

static struct sippymodem_v34_phase3_profile profile_for_rate(
	enum sippymodem_v34_symbol_rate rate, bool high_carrier,
	uint8_t primary_rate)
{
	struct exact_profile_rates rates = profile_rates(rate, high_carrier);

	return (struct sippymodem_v34_phase3_profile){
		.symbol_rate_numerator = rates.symbol_numerator,
		.symbol_rate_denominator = rates.symbol_denominator,
		.carrier_frequency_numerator = rates.carrier_numerator,
		.carrier_frequency_denominator = rates.carrier_denominator,
		.symbol_rate = rate,
		.local_role = SIPPYMODEM_V34_CALLING_STATION,
		.projected_data_rate_2400_bps_units = primary_rate,
		.high_carrier = high_carrier,
	};
}

static uint8_t next_generated_bit(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return (uint8_t)(*state >> 31);
}

static bool initialize_fixture(struct carrier_fixture *fixture)
{
	struct carrier_fixture generated = { 0 };
	struct sippymodem_v34_frame_schedule preview;
	struct sippymodem_v34_mapping_frame mapping_frame;
	uint8_t primary_bits[SIPPYMODEM_V34_MAX_MAPPING_FRAME_BITS];
	uint8_t auxiliary_bit;
	const uint8_t *auxiliary_pointer = NULL;
	uint32_t random = UINT32_C(0xc411e221);

	generated.profile = (struct sippymodem_v34_phase3_profile){
		.symbol_rate_numerator = 3000,
		.symbol_rate_denominator = 1,
		.carrier_frequency_numerator = 2000,
		.carrier_frequency_denominator = 1,
		.symbol_rate = SIPPYMODEM_V34_SYMBOL_RATE_3000,
		.local_role = SIPPYMODEM_V34_CALLING_STATION,
		.projected_data_rate_2400_bps_units = 8,
		.high_carrier = true,
	};
	if (!sippymodem_v34_phase3_profile_is_valid(&generated.profile) ||
	    sippymodem_v34_frame_geometry_init(
		    &generated.geometry, SIPPYMODEM_V34_SYMBOL_RATE_3000, 8,
		    true) != SIPPYMODEM_V34_FRAME_SCHEDULE_OK ||
	    sippymodem_v34_mapping_parameters_init(
		    &generated.parameters, &generated.geometry, false) !=
		    SIPPYMODEM_V34_MAPPING_PARAMETERS_OK ||
	    sippymodem_v34_frame_schedule_init(
		    &generated.schedule, &generated.geometry, 3) !=
		    SIPPYMODEM_V34_FRAME_SCHEDULE_OK ||
	    sippymodem_v34_transmit_scrambler_init(
		    &generated.scrambler, SIPPYMODEM_V34_CALLING_STATION) !=
		    SIPPYMODEM_V34_FRAMING_OK ||
	    sippymodem_v34_4d_encoder_init(
		    &generated.encoder, &generated.parameters, COEFFICIENTS,
		    SIPPYMODEM_V34_TRELLIS_32_STATE) !=
		    SIPPYMODEM_V34_4D_ENCODER_OK ||
	    sippymodem_v34_calibrate_mapping_power(
		    &generated.schedule, &generated.scrambler,
		    &generated.encoder,
		    SIPPYMODEM_V34_NONLINEAR_THETA_5_OVER_16,
		    &generated.normalizer) !=
		    SIPPYMODEM_V34_POWER_CALIBRATION_OK)
		return false;
	preview = generated.schedule;
	if (sippymodem_v34_frame_schedule_next(&preview, &mapping_frame) !=
	    SIPPYMODEM_V34_FRAME_SCHEDULE_OK)
		return false;
	for (size_t bit = 0; bit < mapping_frame.primary_bits; ++bit)
		primary_bits[bit] = next_generated_bit(&random);
	if (mapping_frame.carries_auxiliary_bit) {
		auxiliary_bit = next_generated_bit(&random);
		auxiliary_pointer = &auxiliary_bit;
	}
	if (sippymodem_v34_transmit_mapping_frame(
		    &generated.schedule, &generated.scrambler,
		    &generated.encoder, primary_bits, mapping_frame.primary_bits,
		    auxiliary_pointer,
		    SIPPYMODEM_V34_NONLINEAR_THETA_5_OVER_16,
		    generated.normalizer.precoded_mean_squared_magnitude,
		    &generated.frame) !=
	    SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_OK)
		return false;
	*fixture = generated;
	return true;
}

static bool test_streaming_handoff(void)
{
	struct carrier_fixture fixture;
	struct sippymodem_v34_data_carrier carrier;
	struct sippymodem_v34_data_carrier waiting;
	struct sippymodem_qam_modulator model;
	size_t model_symbol = 0;
	size_t sample_count = 0;
	bool nonzero_sample_seen = false;
	double waiting_sample = 123.0;
	bool waiting_accepted = true;

	CHECK(initialize_fixture(&fixture) &&
	      sippymodem_v34_data_carrier_init(
		      &carrier, &fixture.profile, &fixture.geometry,
		      &fixture.normalizer, 8000) ==
		      SIPPYMODEM_V34_DATA_CARRIER_OK &&
	      sippymodem_v34_data_carrier_needs_mapping_frame(&carrier) &&
	      carrier.modulator.maximum_point_power ==
		      fixture.normalizer.maximum_normalized_point_power &&
	      carrier.modulator.channel.symbol_rate_numerator == 3000u &&
	      carrier.modulator.channel.carrier_frequency_numerator == 2000u,
	      "data-carrier fixture did not initialize");
	waiting = carrier;
	CHECK(sippymodem_v34_data_carrier_sample(
		      &carrier, &waiting_sample, &waiting_accepted) ==
		      SIPPYMODEM_V34_DATA_CARRIER_MAPPING_FRAME_REQUIRED &&
	      memcmp(&carrier, &waiting, sizeof(carrier)) == 0 &&
	      waiting_sample == 123.0 && waiting_accepted,
	      "empty data carrier advanced before a mapping frame");
	model = carrier.modulator;
	CHECK(sippymodem_v34_data_carrier_load_mapping_frame(
		      &carrier, &fixture.frame) ==
		      SIPPYMODEM_V34_DATA_CARRIER_OK &&
	      !sippymodem_v34_data_carrier_needs_mapping_frame(&carrier),
	      "data carrier did not load a mapping frame");
	for (size_t symbol = 0;
	     symbol < SIPPYMODEM_V34_SYMBOLS_PER_MAPPING_FRAME; ++symbol) {
		struct sippymodem_qam_point expected;

		CHECK(sippymodem_v34_normalize_power(
			      &fixture.normalizer,
			      &fixture.frame.projected_symbol[symbol], &expected) ==
			      SIPPYMODEM_V34_SYMBOL_POWER_OK &&
		      carrier.normalized_symbol[symbol].in_phase ==
			      expected.in_phase &&
		      carrier.normalized_symbol[symbol].quadrature ==
			      expected.quadrature,
		      "data carrier changed normalized symbol %zu", symbol);
	}
	while (!sippymodem_v34_data_carrier_needs_mapping_frame(&carrier)) {
		const struct sippymodem_qam_point *point =
			&carrier.normalized_symbol[model_symbol];
		double expected_sample;
		double actual_sample;
		bool expected_accepted;
		bool actual_accepted;

		CHECK(sample_count < 64u &&
		      sippymodem_qam_modulator_sample_f64(
			      &model, point, &expected_sample,
			      &expected_accepted) == SIPPYMODEM_QAM_OK &&
		      sippymodem_v34_data_carrier_sample(
			      &carrier, &actual_sample, &actual_accepted) ==
			      SIPPYMODEM_V34_DATA_CARRIER_OK &&
		      actual_sample == expected_sample &&
		      actual_accepted == expected_accepted,
		      "data carrier diverged at sample %zu", sample_count);
		if (actual_accepted)
			model_symbol++;
		nonzero_sample_seen |= actual_sample != 0.0;
		sample_count++;
	}
	CHECK(model_symbol == SIPPYMODEM_V34_SYMBOLS_PER_MAPPING_FRAME &&
	      carrier.next_symbol == SIPPYMODEM_V34_SYMBOLS_PER_MAPPING_FRAME &&
	      memcmp(&carrier.modulator, &model, sizeof(model)) == 0 &&
	      nonzero_sample_seen && sample_count > model_symbol &&
	      sippymodem_v34_data_carrier_load_mapping_frame(
		      &carrier, &fixture.frame) ==
		      SIPPYMODEM_V34_DATA_CARRIER_OK,
	      "data carrier did not consume and reload the mapping frame");
	return true;
}

static bool test_line_output(void)
{
	struct carrier_fixture fixture;
	struct sippymodem_v34_data_line_transmitter transmitter;
	struct sippymodem_v34_data_line_transmitter waiting;
	struct sippymodem_v34_data_carrier model_carrier;
	struct sippymodem_v34_preemphasis model_filter;
	size_t samples = 0;
	bool nonzero_seen = false;
	int16_t waiting_sample = 1234;
	bool waiting_accepted = true;

	CHECK(initialize_fixture(&fixture) &&
	      sippymodem_v34_data_line_transmitter_init(
		      &transmitter, &fixture.schedule, &fixture.scrambler,
		      &fixture.encoder, &fixture.normalizer, &fixture.profile,
		      8000, 4000) == SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_OK &&
	      sippymodem_v34_data_line_transmitter_needs_mapping_frame(
		      &transmitter),
	      "data line transmitter did not initialize");
	waiting = transmitter;
	CHECK(sippymodem_v34_data_line_transmitter_sample(
		      &transmitter, &waiting_sample, &waiting_accepted) ==
		      SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_MAPPING_FRAME_REQUIRED &&
	      memcmp(&transmitter, &waiting, sizeof(transmitter)) == 0 &&
	      waiting_sample == 1234 && waiting_accepted &&
	      sippymodem_v34_data_line_transmitter_load_mapping_frame(
		      &transmitter, &fixture.frame) ==
		      SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_OK,
	      "data line transmitter changed while waiting for a frame");
	model_carrier = transmitter.carrier;
	model_filter = transmitter.preemphasis;
	while (!sippymodem_v34_data_line_transmitter_needs_mapping_frame(
		&transmitter)) {
		double carrier_sample;
		double filtered_sample;
		long expected_sample;
		int16_t actual_sample;
		bool expected_accepted;
		bool actual_accepted;

		CHECK(samples < 64u &&
		      sippymodem_v34_data_carrier_sample(
			      &model_carrier, &carrier_sample,
			      &expected_accepted) ==
			      SIPPYMODEM_V34_DATA_CARRIER_OK &&
		      sippymodem_v34_preemphasis_sample(
			      &model_filter, carrier_sample, &filtered_sample) ==
			      SIPPYMODEM_V34_PREEMPHASIS_OK,
		      "data line model failed at sample %zu", samples);
		expected_sample = lround(filtered_sample * transmitter.output_gain);
		CHECK(sippymodem_v34_data_line_transmitter_sample(
			      &transmitter, &actual_sample, &actual_accepted) ==
			      SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_OK &&
		      actual_sample == expected_sample &&
		      actual_accepted == expected_accepted,
		      "data line output diverged at sample %zu", samples);
		nonzero_seen |= actual_sample != 0;
		samples++;
	}
	CHECK(samples > SIPPYMODEM_V34_SYMBOLS_PER_MAPPING_FRAME &&
	      transmitter.generated_samples == samples && nonzero_seen &&
	      memcmp(&transmitter.carrier, &model_carrier,
		     sizeof(model_carrier)) == 0 &&
	      memcmp(&transmitter.preemphasis, &model_filter,
		     sizeof(model_filter)) == 0,
	      "data line transmitter did not consume a mapping frame");
	return true;
}

static bool test_line_power_reduction(void)
{
	struct carrier_fixture fixture;
	struct sippymodem_v34_phase3_profile reduced_profile;
	struct sippymodem_v34_data_line_transmitter full;
	struct sippymodem_v34_data_line_transmitter reduced;
	long double full_sum_squares = 0.0L;
	long double reduced_sum_squares = 0.0L;
	double expected_ratio = pow(10.0, -6.0 / 20.0);
	size_t samples = 0;

	CHECK(initialize_fixture(&fixture),
	      "line-power fixture did not initialize");
	reduced_profile = fixture.profile;
	reduced_profile.transmit_power_reduction_db = 6;
	CHECK(sippymodem_v34_data_line_transmitter_init(
		      &full, &fixture.schedule, &fixture.scrambler,
		      &fixture.encoder, &fixture.normalizer, &fixture.profile,
		      8000, 4000) == SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_OK &&
	      sippymodem_v34_data_line_transmitter_init(
		      &reduced, &fixture.schedule, &fixture.scrambler,
		      &fixture.encoder, &fixture.normalizer, &reduced_profile,
		      8000, 4000) == SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_OK &&
	      memcmp(&full.calibration, &reduced.calibration,
		     sizeof(full.calibration)) == 0 &&
	      fabs(reduced.target_rms / full.target_rms - expected_ratio) <
		      1e-12 &&
	      fabs(reduced.output_gain / full.output_gain - expected_ratio) <
		      1e-12,
	      "negotiated reduction changed calibration or gain");
	CHECK(sippymodem_v34_data_line_transmitter_load_mapping_frame(
		      &full, &fixture.frame) ==
		      SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_OK &&
	      sippymodem_v34_data_line_transmitter_load_mapping_frame(
		      &reduced, &fixture.frame) ==
		      SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_OK,
	      "line-power comparison frame did not load");
	while (!sippymodem_v34_data_line_transmitter_needs_mapping_frame(
		&full)) {
		int16_t full_sample;
		int16_t reduced_sample;
		bool full_accepted;
		bool reduced_accepted;

		CHECK(samples < 64u &&
		      !sippymodem_v34_data_line_transmitter_needs_mapping_frame(
			      &reduced) &&
		      sippymodem_v34_data_line_transmitter_sample(
			      &full, &full_sample, &full_accepted) ==
			      SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_OK &&
		      sippymodem_v34_data_line_transmitter_sample(
			      &reduced, &reduced_sample, &reduced_accepted) ==
			      SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_OK &&
		      full_accepted == reduced_accepted,
		      "reduced line output diverged at sample %zu", samples);
		full_sum_squares += (long double)full_sample * full_sample;
		reduced_sum_squares +=
			(long double)reduced_sample * reduced_sample;
		samples++;
	}
	CHECK(samples != 0 &&
	      sippymodem_v34_data_line_transmitter_needs_mapping_frame(
		      &reduced) &&
	      fabsl(sqrtl(reduced_sum_squares / samples) /
			    sqrtl(full_sum_squares / samples) -
		    expected_ratio) < 0.001L,
	      "rendered data did not honor the negotiated reduction");
	return true;
}

static bool test_line_transactional_validation(void)
{
	struct carrier_fixture fixture;
	struct sippymodem_v34_data_line_transmitter transmitter;
	struct sippymodem_v34_data_line_transmitter before;
	struct sippymodem_v34_data_line_transmitter corrupt;
	struct sippymodem_v34_phase3_profile invalid_profile;
	int16_t sample = 1234;
	bool accepted = true;

	CHECK(initialize_fixture(&fixture),
	      "data-line validation fixture did not initialize");
	memset(&transmitter, 0xa5, sizeof(transmitter));
	before = transmitter;
	invalid_profile = fixture.profile;
	invalid_profile.transmit_power_reduction_db = UINT8_MAX;
	CHECK(sippymodem_v34_data_line_transmitter_init(
		      NULL, &fixture.schedule, &fixture.scrambler,
		      &fixture.encoder, &fixture.normalizer, &fixture.profile,
		      8000, 4000) ==
		      SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT &&
	      sippymodem_v34_data_line_transmitter_init(
		      &transmitter, NULL, &fixture.scrambler, &fixture.encoder,
		      &fixture.normalizer, &fixture.profile, 8000, 4000) ==
		      SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT &&
	      sippymodem_v34_data_line_transmitter_init(
		      &transmitter, &fixture.schedule, NULL, &fixture.encoder,
		      &fixture.normalizer, &fixture.profile, 8000, 4000) ==
		      SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT &&
	      sippymodem_v34_data_line_transmitter_init(
		      &transmitter, &fixture.schedule, &fixture.scrambler, NULL,
		      &fixture.normalizer, &fixture.profile, 8000, 4000) ==
		      SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT &&
	      sippymodem_v34_data_line_transmitter_init(
		      &transmitter, &fixture.schedule, &fixture.scrambler,
		      &fixture.encoder, NULL, &fixture.profile, 8000, 4000) ==
		      SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT &&
	      sippymodem_v34_data_line_transmitter_init(
		      &transmitter, &fixture.schedule, &fixture.scrambler,
		      &fixture.encoder, &fixture.normalizer, NULL, 8000, 4000) ==
		      SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT &&
	      sippymodem_v34_data_line_transmitter_init(
		      &transmitter, &fixture.schedule, &fixture.scrambler,
		      &fixture.encoder, &fixture.normalizer, &invalid_profile,
		      8000, 4000) ==
		      SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT &&
	      sippymodem_v34_data_line_transmitter_init(
		      &transmitter, &fixture.schedule, &fixture.scrambler,
		      &fixture.encoder, &fixture.normalizer, &fixture.profile,
		      0, 4000) ==
		      SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT &&
	      sippymodem_v34_data_line_transmitter_init(
		      &transmitter, &fixture.schedule, &fixture.scrambler,
		      &fixture.encoder, &fixture.normalizer, &fixture.profile,
		      8000, 0) ==
		      SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT &&
	      memcmp(&transmitter, &before, sizeof(transmitter)) == 0,
	      "rejected line initialization changed its destination");
	CHECK(sippymodem_v34_data_line_transmitter_init(
		      &transmitter, &fixture.schedule, &fixture.scrambler,
		      &fixture.encoder, &fixture.normalizer, &fixture.profile,
		      8000, INT16_MAX) ==
		      SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_CLIPPING_RISK &&
	      memcmp(&transmitter, &before, sizeof(transmitter)) == 0,
	      "clipping-risk initialization changed its destination");
	CHECK(sippymodem_v34_data_line_transmitter_init(
		      &transmitter, &fixture.schedule, &fixture.scrambler,
		      &fixture.encoder, &fixture.normalizer, &fixture.profile,
		      8000, 4000) == SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_OK,
	      "valid data-line state did not initialize");
	before = transmitter;
	CHECK(!sippymodem_v34_data_line_transmitter_is_valid(NULL) &&
	      !sippymodem_v34_data_line_transmitter_needs_mapping_frame(NULL) &&
	      sippymodem_v34_data_line_transmitter_load_mapping_frame(
		      NULL, &fixture.frame) ==
		      SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT &&
	      sippymodem_v34_data_line_transmitter_load_mapping_frame(
		      &transmitter, NULL) ==
		      SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT &&
	      sippymodem_v34_data_line_transmitter_sample(
		      NULL, &sample, &accepted) ==
		      SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT &&
	      sippymodem_v34_data_line_transmitter_sample(
		      &transmitter, NULL, &accepted) ==
		      SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT &&
	      sippymodem_v34_data_line_transmitter_sample(
		      &transmitter, &sample, NULL) ==
		      SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT &&
	      memcmp(&transmitter, &before, sizeof(transmitter)) == 0 &&
	      sample == 1234 && accepted,
	      "invalid data-line operation changed state or output");
	CHECK(sippymodem_v34_data_line_transmitter_load_mapping_frame(
		      &transmitter, &fixture.frame) ==
		      SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_OK,
	      "validation frame did not load");
	before = transmitter;
	CHECK(sippymodem_v34_data_line_transmitter_load_mapping_frame(
		      &transmitter, &fixture.frame) ==
		      SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_MAPPING_FRAME_PENDING &&
	      memcmp(&transmitter, &before, sizeof(transmitter)) == 0,
	      "data line replaced a pending mapping frame");

	corrupt = before;
	corrupt.generated_samples = 1;
	CHECK(!sippymodem_v34_data_line_transmitter_is_valid(&corrupt) &&
	      sippymodem_v34_data_line_transmitter_sample(
		      &corrupt, &sample, &accepted) ==
		      SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT &&
	      sample == 1234 && accepted,
	      "data line accepted inconsistent clock state");
	corrupt = before;
	corrupt.generated_samples = UINT64_MAX;
	corrupt.preemphasis.next_history =
		UINT64_MAX % SIPPYMODEM_V34_PREEMPHASIS_TAPS;
	corrupt.carrier.modulator.carrier_phase =
		(uint32_t)UINT64_MAX *
		corrupt.carrier.modulator.carrier_phase_step;
	corrupt.carrier.modulator.symbol_phase_units =
		(UINT64_MAX % corrupt.carrier.modulator.symbol_period_units) *
		corrupt.carrier.profile.symbol_rate_numerator %
		corrupt.carrier.modulator.symbol_period_units;
	before = corrupt;
	CHECK(sippymodem_v34_data_line_transmitter_is_valid(&corrupt) &&
	      sippymodem_v34_data_line_transmitter_sample(
		      &corrupt, &sample, &accepted) ==
		      SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_OUT_OF_RANGE &&
	      memcmp(&corrupt, &before, sizeof(corrupt)) == 0 &&
	      sample == 1234 && accepted,
	      "exhausted sample count changed state or output");
	CHECK(strcmp(sippymodem_v34_data_line_transmitter_result_name(
		     SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_OK),
		     "ok") == 0 &&
	      strcmp(sippymodem_v34_data_line_transmitter_result_name(
		     SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_MAPPING_FRAME_REQUIRED),
		     "mapping frame required") == 0 &&
	      strcmp(sippymodem_v34_data_line_transmitter_result_name(
		     SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_MAPPING_FRAME_PENDING),
		     "mapping frame pending") == 0 &&
	      strcmp(sippymodem_v34_data_line_transmitter_result_name(
		     SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(sippymodem_v34_data_line_transmitter_result_name(
		     SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_OUT_OF_RANGE),
		     "out of range") == 0 &&
	      strcmp(sippymodem_v34_data_line_transmitter_result_name(
		     SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_CLIPPING_RISK),
		     "clipping risk") == 0 &&
	      strcmp(sippymodem_v34_data_line_transmitter_result_name(
		     SIPPYMODEM_V34_DATA_LINE_TRANSMITTER_NUMERIC_FAILURE),
		     "numeric failure") == 0 &&
	      strcmp(sippymodem_v34_data_line_transmitter_result_name(
		     (enum sippymodem_v34_data_line_transmitter_result)99),
		     "unknown") == 0,
	      "data-line result names changed");
	return true;
}

static bool test_all_line_profiles(void)
{
	static const uint8_t PRIMARY_RATES[] = { 1, 2, 2, 2, 2, 2 };
	size_t configuration_count = 0;

	for (unsigned int rate_index = 0;
	     rate_index < SIPPYMODEM_V34_SYMBOL_RATE_COUNT; ++rate_index) {
		enum sippymodem_v34_symbol_rate rate =
			(enum sippymodem_v34_symbol_rate)rate_index;
		unsigned int carrier_count =
			rate == SIPPYMODEM_V34_SYMBOL_RATE_3429 ? 1u : 2u;

		for (unsigned int high = 0; high < carrier_count; ++high) {
			struct sippymodem_v34_phase3_profile profile =
				profile_for_rate(
					rate, high != 0, PRIMARY_RATES[rate_index]);
			struct sippymodem_v34_frame_geometry geometry;
			struct sippymodem_v34_mapping_parameters parameters;
			struct sippymodem_v34_symbol_power_measurement measurement = {
				.mean_squared_magnitude = 1.0,
				.symbol_count = 1,
			};
			struct sippymodem_v34_power_normalizer normalizer;
			struct sippymodem_v34_data_carrier carrier;

			CHECK(sippymodem_v34_phase3_profile_is_valid(&profile) &&
			      sippymodem_v34_frame_geometry_init(
				      &geometry, rate, PRIMARY_RATES[rate_index],
				      false) ==
				      SIPPYMODEM_V34_FRAME_SCHEDULE_OK &&
			      sippymodem_v34_mapping_parameters_init(
				      &parameters, &geometry, false) ==
				      SIPPYMODEM_V34_MAPPING_PARAMETERS_OK &&
			      sippymodem_v34_power_normalizer_init(
				      &normalizer, &parameters,
				      SIPPYMODEM_V34_NONLINEAR_THETA_ZERO,
				      &measurement, &measurement) ==
				      SIPPYMODEM_V34_SYMBOL_POWER_OK &&
			      sippymodem_v34_data_carrier_init(
				      &carrier, &profile, &geometry, &normalizer,
				      8000) == SIPPYMODEM_V34_DATA_CARRIER_OK &&
			      sippymodem_v34_data_carrier_is_valid(&carrier) &&
			      carrier.modulator.channel.symbol_rate_numerator ==
				      profile.symbol_rate_numerator &&
			      carrier.modulator.channel.symbol_rate_denominator ==
				      profile.symbol_rate_denominator &&
			      carrier.modulator.channel
				      .carrier_frequency_numerator ==
				      profile.carrier_frequency_numerator &&
			      carrier.modulator.channel
				      .carrier_frequency_denominator ==
				      profile.carrier_frequency_denominator,
			      "data carrier profile %u/%u changed", rate_index,
			      high);
			configuration_count++;
		}
	}
	CHECK(configuration_count == 11u,
	      "data carrier covered %zu line profiles", configuration_count);
	return true;
}

static bool bytes_are_filled(
	const void *object, size_t size, unsigned char value)
{
	const unsigned char *bytes = object;

	for (size_t index = 0; index < size; ++index) {
		if (bytes[index] != value)
			return false;
	}
	return true;
}

static bool rejected_frame_is_transactional(
	struct sippymodem_v34_data_carrier *carrier,
	const struct sippymodem_v34_transmitted_mapping_frame *frame,
	enum sippymodem_v34_data_carrier_result expected)
{
	struct sippymodem_v34_data_carrier saved = *carrier;

	return sippymodem_v34_data_carrier_load_mapping_frame(carrier, frame) ==
		       expected &&
	       memcmp(carrier, &saved, sizeof(saved)) == 0;
}

static bool test_transactional_validation(void)
{
	struct carrier_fixture fixture;
	struct sippymodem_v34_data_carrier carrier;
	struct sippymodem_v34_data_carrier saved;
	struct sippymodem_v34_data_carrier corrupt;
	struct sippymodem_v34_transmitted_mapping_frame invalid_frame;
	struct sippymodem_v34_phase3_profile limited_profile;
	struct sippymodem_v34_phase3_profile mismatched_profile;
	struct sippymodem_v34_power_normalizer mismatched_normalizer;
	struct sippymodem_v34_frame_geometry mismatched_geometry;
	struct sippymodem_v34_mapping_parameters mismatched_parameters;
	double sample = 123.0;
	bool accepted = true;

	CHECK(initialize_fixture(&fixture) &&
	      sippymodem_v34_data_carrier_init(
		      &carrier, &fixture.profile, &fixture.geometry,
		      &fixture.normalizer, 8000) ==
		      SIPPYMODEM_V34_DATA_CARRIER_OK,
	      "data-carrier validation fixture did not initialize");
	invalid_frame = fixture.frame;
	invalid_frame.projected_symbol[7].in_phase = NAN;
	CHECK(rejected_frame_is_transactional(
		      &carrier, &invalid_frame,
		      SIPPYMODEM_V34_DATA_CARRIER_INVALID_ARGUMENT),
	      "data carrier accepted a nonfinite late symbol");
	invalid_frame = fixture.frame;
	invalid_frame.projected_symbol[7].in_phase = DBL_MAX;
	CHECK(rejected_frame_is_transactional(
		      &carrier, &invalid_frame,
		      SIPPYMODEM_V34_DATA_CARRIER_NUMERIC_FAILURE),
	      "data carrier accepted an overflowing late symbol");
	invalid_frame = fixture.frame;
	invalid_frame.projected_symbol[7] = (struct sippymodem_qam_point){
		.in_phase =
			sqrt(fixture.normalizer.maximum_projected_point_power) *
			1.001,
	};
	CHECK(rejected_frame_is_transactional(
		      &carrier, &invalid_frame,
		      SIPPYMODEM_V34_DATA_CARRIER_OUT_OF_RANGE),
	      "data carrier accepted an excessive late symbol");
	CHECK(sippymodem_v34_data_carrier_load_mapping_frame(
		      &carrier, &fixture.frame) ==
		      SIPPYMODEM_V34_DATA_CARRIER_OK,
	      "data carrier did not load the validation frame");
	saved = carrier;
	CHECK(sippymodem_v34_data_carrier_load_mapping_frame(
		      &carrier, &fixture.frame) ==
		      SIPPYMODEM_V34_DATA_CARRIER_MAPPING_FRAME_PENDING &&
	      memcmp(&carrier, &saved, sizeof(saved)) == 0,
	      "data carrier replaced a pending mapping frame");

	limited_profile = fixture.profile;
	limited_profile.projected_data_rate_2400_bps_units = 7;
	mismatched_profile = profile_for_rate(
		SIPPYMODEM_V34_SYMBOL_RATE_2800, false, 8);
	CHECK(sippymodem_v34_frame_geometry_init(
		      &mismatched_geometry, SIPPYMODEM_V34_SYMBOL_RATE_3000, 7,
		      true) == SIPPYMODEM_V34_FRAME_SCHEDULE_OK &&
	      sippymodem_v34_mapping_parameters_init(
		      &mismatched_parameters, &mismatched_geometry, false) ==
		      SIPPYMODEM_V34_MAPPING_PARAMETERS_OK,
	      "mismatched data-carrier geometry did not initialize");
	mismatched_normalizer = fixture.normalizer;
	mismatched_normalizer.parameters = mismatched_parameters;
	memset(&carrier, 0xa5, sizeof(carrier));
	CHECK(sippymodem_v34_data_carrier_init(
		      NULL, &fixture.profile, &fixture.geometry,
		      &fixture.normalizer, 8000) ==
		      SIPPYMODEM_V34_DATA_CARRIER_INVALID_ARGUMENT &&
	      sippymodem_v34_data_carrier_init(
		      &carrier, NULL, &fixture.geometry, &fixture.normalizer,
		      8000) == SIPPYMODEM_V34_DATA_CARRIER_INVALID_ARGUMENT &&
	      sippymodem_v34_data_carrier_init(
		      &carrier, &fixture.profile, NULL, &fixture.normalizer,
		      8000) == SIPPYMODEM_V34_DATA_CARRIER_INVALID_ARGUMENT &&
	      sippymodem_v34_data_carrier_init(
		      &carrier, &fixture.profile, &fixture.geometry, NULL,
		      8000) == SIPPYMODEM_V34_DATA_CARRIER_INVALID_ARGUMENT &&
	      sippymodem_v34_data_carrier_init(
		      &carrier, &fixture.profile, &fixture.geometry,
		      &fixture.normalizer, 0) ==
		      SIPPYMODEM_V34_DATA_CARRIER_INVALID_ARGUMENT &&
	      sippymodem_v34_data_carrier_init(
		      &carrier, &limited_profile, &fixture.geometry,
		      &fixture.normalizer, 8000) ==
		      SIPPYMODEM_V34_DATA_CARRIER_INVALID_ARGUMENT &&
	      sippymodem_v34_data_carrier_init(
		      &carrier, &mismatched_profile, &fixture.geometry,
		      &fixture.normalizer, 8000) ==
		      SIPPYMODEM_V34_DATA_CARRIER_INVALID_ARGUMENT &&
	      sippymodem_v34_data_carrier_init(
		      &carrier, &fixture.profile, &fixture.geometry,
		      &mismatched_normalizer, 8000) ==
		      SIPPYMODEM_V34_DATA_CARRIER_INVALID_ARGUMENT &&
	      sippymodem_v34_data_carrier_init(
		      &carrier, &fixture.profile, &fixture.geometry,
		      &fixture.normalizer, 7000) ==
		      SIPPYMODEM_V34_DATA_CARRIER_INVALID_ARGUMENT &&
	      bytes_are_filled(&carrier, sizeof(carrier), 0xa5),
	      "invalid data-carrier initialization changed its destination");

	corrupt = saved;
	corrupt.modulator.maximum_point_power = NAN;
	CHECK(!sippymodem_v34_data_carrier_is_valid(NULL) &&
	      !sippymodem_v34_data_carrier_needs_mapping_frame(NULL) &&
	      sippymodem_v34_data_carrier_load_mapping_frame(
		      NULL, &fixture.frame) ==
		      SIPPYMODEM_V34_DATA_CARRIER_INVALID_ARGUMENT &&
	      sippymodem_v34_data_carrier_load_mapping_frame(&saved, NULL) ==
		      SIPPYMODEM_V34_DATA_CARRIER_INVALID_ARGUMENT &&
	      sippymodem_v34_data_carrier_sample(
		      NULL, &sample, &accepted) ==
		      SIPPYMODEM_V34_DATA_CARRIER_INVALID_ARGUMENT &&
	      sippymodem_v34_data_carrier_sample(&saved, NULL, &accepted) ==
		      SIPPYMODEM_V34_DATA_CARRIER_INVALID_ARGUMENT &&
	      sippymodem_v34_data_carrier_sample(&saved, &sample, NULL) ==
		      SIPPYMODEM_V34_DATA_CARRIER_INVALID_ARGUMENT &&
	      !sippymodem_v34_data_carrier_is_valid(&corrupt) &&
	      sippymodem_v34_data_carrier_load_mapping_frame(
		      &corrupt, &fixture.frame) ==
		      SIPPYMODEM_V34_DATA_CARRIER_INVALID_ARGUMENT &&
	      sippymodem_v34_data_carrier_sample(
		      &corrupt, &sample, &accepted) ==
		      SIPPYMODEM_V34_DATA_CARRIER_INVALID_ARGUMENT &&
	      sample == 123.0 && accepted,
	      "corrupt data carrier changed outputs");
	CHECK(strcmp(sippymodem_v34_data_carrier_result_name(
		     SIPPYMODEM_V34_DATA_CARRIER_OK),
		     "ok") == 0 &&
	      strcmp(sippymodem_v34_data_carrier_result_name(
		     SIPPYMODEM_V34_DATA_CARRIER_MAPPING_FRAME_REQUIRED),
		     "mapping frame required") == 0 &&
	      strcmp(sippymodem_v34_data_carrier_result_name(
		     SIPPYMODEM_V34_DATA_CARRIER_MAPPING_FRAME_PENDING),
		     "mapping frame pending") == 0 &&
	      strcmp(sippymodem_v34_data_carrier_result_name(
		     SIPPYMODEM_V34_DATA_CARRIER_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(sippymodem_v34_data_carrier_result_name(
		     SIPPYMODEM_V34_DATA_CARRIER_OUT_OF_RANGE),
		     "out of range") == 0 &&
	      strcmp(sippymodem_v34_data_carrier_result_name(
		     SIPPYMODEM_V34_DATA_CARRIER_NUMERIC_FAILURE),
		     "numeric failure") == 0 &&
	      strcmp(sippymodem_v34_data_carrier_result_name(
		     (enum sippymodem_v34_data_carrier_result)6),
		     "unknown") == 0,
	      "data-carrier result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "streaming handoff", test_streaming_handoff },
		{ "line output", test_line_output },
		{ "line power reduction", test_line_power_reduction },
		{ "line transactional validation",
		  test_line_transactional_validation },
		{ "all line profiles", test_all_line_profiles },
		{ "transactional validation", test_transactional_validation },
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
