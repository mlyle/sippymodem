// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v34_data_carrier.h>
#include <sippymodem_v34_power_calibration.h>

#include "test.h"

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

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "streaming handoff", test_streaming_handoff },
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
