// SPDX-License-Identifier: BSD-3-Clause

#include <v34_data_startup.h>

#include "test.h"
#include "v34_power_fixture.h"
#include "v34_encoder_test_helpers.h"

#include <stddef.h>
#include <string.h>

static struct v34_phase3_profile caller_profile(void)
{
	return (struct v34_phase3_profile){
		.symbol_rate_numerator = 3000,
		.symbol_rate_denominator = 1,
		.carrier_frequency_numerator = 2000,
		.carrier_frequency_denominator = 1,
		.symbol_rate = V34_SYMBOL_RATE_3000,
		.local_role = V34_CALLING_STATION,
		.projected_data_rate_2400_bps_units = 12,
		.high_carrier = true,
	};
}

static struct v34_phase3_profile answerer_profile(void)
{
	return (struct v34_phase3_profile){
		.symbol_rate_numerator = 3200,
		.symbol_rate_denominator = 1,
		.carrier_frequency_numerator = 12800,
		.carrier_frequency_denominator = 7,
		.symbol_rate = V34_SYMBOL_RATE_3200,
		.local_role = V34_ANSWERING_STATION,
		.projected_data_rate_2400_bps_units = 13,
	};
}

static struct v34_data_mode caller_mode(void)
{
	return (struct v34_data_mode){
		.transmit = {
			.trellis_selection = V34_TRELLIS_16_STATE,
			.nonlinear_parameter = V34_NONLINEAR_THETA_ZERO,
			.primary_data_rate_2400_bps_units = 8,
			.expanded_shaping = true,
		},
		.receive = {
			.precoder_coefficients = {
				{ 4096, -2048 },
				{ 1024, 3072 },
				{ -1536, 512 },
			},
			.trellis_selection = V34_TRELLIS_64_STATE,
			.nonlinear_parameter =
				V34_NONLINEAR_THETA_5_OVER_16,
			.primary_data_rate_2400_bps_units = 9,
		},
		.auxiliary_channel_enabled = true,
		.asymmetric_rates_enabled = true,
	};
}

static struct v34_data_mode answerer_mode(void)
{
	struct v34_data_mode caller = caller_mode();
	struct v34_data_mode answerer = {
		.transmit = caller.receive,
		.receive = caller.transmit,
		.auxiliary_channel_enabled = caller.auxiliary_channel_enabled,
		.asymmetric_rates_enabled = caller.asymmetric_rates_enabled,
	};

	return answerer;
}

static bool mapping_matches_receiver(
	const struct v34_data_mapping *mapping,
	const struct v34_data_line_receiver_configuration *receiver)
{
	return mapping->transmitting_profile.local_role ==
		       receiver->transmitting_profile.local_role &&
	       mapping->transmitting_profile.symbol_rate ==
		       receiver->transmitting_profile.symbol_rate &&
	       v34_frame_geometry_is_equal(
		       &mapping->schedule.geometry, &receiver->geometry) &&
	       v34_normalizers_equal(&mapping->normalizer, &receiver->normalizer) &&
	       mapping->encoder.convolutional_encoder.selection ==
		       receiver->trellis_selection &&
	       mapping->encoder.parameters.expanded_shaping ==
		       receiver->expanded_shaping &&
	       memcmp(mapping->encoder.precoder.coefficients,
		      receiver->precoder_coefficients,
		      sizeof(receiver->precoder_coefficients)) == 0;
}

static bool test_complementary_paths(void)
{
	struct v34_phase3_profile caller = caller_profile();
	struct v34_phase3_profile answerer = answerer_profile();
	struct v34_data_mode caller_selection = caller_mode();
	struct v34_data_mode answerer_selection = answerer_mode();
	struct v34_data_startup caller_startup;
	struct v34_data_startup answerer_startup;

	CHECK(v34_data_startup_prepare(
		      &caller_startup, &caller_selection, &caller, &answerer, 8000) ==
		      V34_DATA_STARTUP_OK &&
	      v34_data_startup_prepare(
		      &answerer_startup, &answerer_selection, &answerer, &caller, 8000) ==
		      V34_DATA_STARTUP_OK &&
	      v34_data_startup_is_valid(&caller_startup) &&
	      v34_data_startup_is_valid(&answerer_startup),
	      "complementary startup paths did not initialize");
	CHECK(mapping_matches_receiver(
		      &caller_startup.transmit.mapping, &answerer_startup.receive) &&
	      mapping_matches_receiver(
		      &answerer_startup.transmit.mapping, &caller_startup.receive),
	      "the stations prepared incompatible directional mappings");
	CHECK(caller_startup.transmit.mapping.schedule.geometry
			      .primary_data_rate_2400_bps_units == 8 &&
	      caller_startup.receive.geometry
			      .primary_data_rate_2400_bps_units == 9,
	      "startup did not retain directional rates");
	return true;
}

static bool object_is_filled(
	const void *object, size_t size, unsigned char value)
{
	const unsigned char *bytes = object;

	for (size_t index = 0; index < size; ++index) {
		if (bytes[index] != value)
			return false;
	}
	return true;
}

static bool test_validation_and_transactionality(void)
{
	struct v34_phase3_profile caller = caller_profile();
	struct v34_phase3_profile answerer = answerer_profile();
	struct v34_data_mode mode = caller_mode();
	struct v34_data_startup destination;
	struct v34_data_startup prepared;

	caller.projected_data_rate_2400_bps_units = 4;
	answerer.projected_data_rate_2400_bps_units = 5;
	CHECK(v34_data_startup_prepare(
		      &prepared, &mode, &caller, &answerer, 8000) ==
		      V34_DATA_STARTUP_OK &&
	      v34_data_startup_is_valid(&prepared) &&
	      prepared.transmit.mapping.schedule.geometry
			      .primary_data_rate_2400_bps_units == 8 &&
	      prepared.receive.geometry
			      .primary_data_rate_2400_bps_units == 9,
	      "Phase 2 projections incorrectly limited Phase 4 startup");
	caller = caller_profile();
	answerer = answerer_profile();
	memset(&destination, 0xa5, sizeof(destination));
	CHECK(v34_data_startup_prepare(
		      NULL, &mode, &caller, &answerer, 8000) ==
		      V34_DATA_STARTUP_INVALID_ARGUMENT &&
	      v34_data_startup_prepare(
		      &destination, NULL, &caller, &answerer, 8000) ==
		      V34_DATA_STARTUP_INVALID_ARGUMENT &&
	      v34_data_startup_prepare(
		      &destination, &mode, NULL, &answerer, 8000) ==
		      V34_DATA_STARTUP_INVALID_ARGUMENT &&
	      v34_data_startup_prepare(
		      &destination, &mode, &caller, NULL, 8000) ==
		      V34_DATA_STARTUP_INVALID_ARGUMENT &&
	      v34_data_startup_prepare(
		      &destination, &mode, &caller, &caller, 8000) ==
		      V34_DATA_STARTUP_INVALID_ARGUMENT &&
	      v34_data_startup_prepare(
		      &destination, &mode, &caller, &answerer, 0) ==
		      V34_DATA_STARTUP_INVALID_ARGUMENT &&
	      object_is_filled(&destination, sizeof(destination), 0xa5),
	      "invalid startup input changed the destination");
	mode.transmit.primary_data_rate_2400_bps_units = 14;
	caller.projected_data_rate_2400_bps_units = 14;
	CHECK(v34_data_startup_prepare(
		      &destination, &mode, &caller, &answerer, 8000) ==
		      V34_DATA_STARTUP_OUT_OF_RANGE &&
	      object_is_filled(&destination, sizeof(destination), 0xa5),
	      "unsupported transmit geometry changed the destination");
	mode = caller_mode();
	mode.receive.primary_data_rate_2400_bps_units = 14;
	answerer.projected_data_rate_2400_bps_units = 14;
	CHECK(v34_data_startup_prepare(
		      &destination, &mode, &caller, &answerer, 8000) ==
		      V34_DATA_STARTUP_OUT_OF_RANGE &&
	      object_is_filled(&destination, sizeof(destination), 0xa5),
	      "receive failure exposed a partially prepared path");

	mode = caller_mode();
	caller = caller_profile();
	answerer = answerer_profile();
	CHECK(v34_data_startup_prepare(
		      &prepared, &mode, &caller, &answerer, 8000) ==
		      V34_DATA_STARTUP_OK,
	      "startup validation fixture did not initialize");
	prepared.receive.geometry.symbol_rate = V34_SYMBOL_RATE_2400;
	CHECK(!v34_data_startup_is_valid(NULL) &&
	      !v34_data_startup_is_valid(&prepared),
	      "mismatched receive geometry was accepted");
	CHECK(v34_data_startup_prepare(
		      &prepared, &mode, &caller, &answerer, 8000) ==
		      V34_DATA_STARTUP_OK,
	      "startup validation fixture did not reinitialize");
	prepared.transmit.mapping.encoder.previous_differential_output = 1;
	CHECK(!v34_data_startup_is_valid(&prepared),
	      "advanced transmit mapping was accepted");
	return true;
}

static bool test_incremental_startup(void)
{
	struct v34_phase3_profile profiles[] = { caller_profile(), answerer_profile() };
	struct v34_data_mode modes[] = { caller_mode(), answerer_mode() };
	static const uint32_t BUDGETS[] = { 1, 23, UINT32_MAX };

	for (unsigned int role = 0; role < 2; ++role) {
		struct v34_data_startup expected;
		CHECK(v34_data_startup_prepare(&expected, &modes[role], &profiles[role],
			&profiles[role ^ 1u], 8000) == V34_DATA_STARTUP_OK,
			"startup reference failed");
		for (size_t i = 0; i < ARRAY_LENGTH(BUDGETS); ++i) {
			struct v34_data_startup_preparation job;
			struct v34_data_startup actual, saved;
			enum v34_data_startup_result result;
			unsigned int steps = 0;

			CHECK(v34_data_startup_begin_preparation(&job, &modes[role],
				&profiles[role], &profiles[role ^ 1u], 8000) == V34_DATA_STARTUP_OK,
				"startup job did not begin");
			memset(&actual, 0xa5, sizeof(actual));
			saved = actual;
			do {
				result = v34_data_startup_step_preparation(&job, BUDGETS[i], &actual);
				CHECK(++steps < 4000, "startup did not finish its bounded work");
				if (result == V34_DATA_STARTUP_IN_PROGRESS)
					CHECK(memcmp(&actual, &saved, sizeof(actual)) == 0,
						"startup exposed an incomplete preparation");
			} while (result == V34_DATA_STARTUP_IN_PROGRESS);
			CHECK(result == V34_DATA_STARTUP_OK && v34_data_startup_is_valid(&actual) &&
			      v34_normalizers_equal(&actual.transmit.mapping.normalizer,
				&expected.transmit.mapping.normalizer) &&
			      actual.transmit.line_peak == expected.transmit.line_peak &&
			      v34_normalizers_equal(&actual.receive.normalizer, &expected.receive.normalizer),
			      "incremental preparation changed negotiated power");
		}
	}
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "complementary paths", test_complementary_paths },
		{ "incremental startup", test_incremental_startup },
		{ "validation and transactionality",
		  test_validation_and_transactionality },
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
