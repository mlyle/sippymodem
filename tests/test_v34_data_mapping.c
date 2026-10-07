// SPDX-License-Identifier: BSD-3-Clause

#include <v34_data_mapping.h>

#include "test.h"
#include "v34_power_fixture.h"
#include "v34_encoder_test_helpers.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static struct v34_phase3_profile profile_fixture(
	enum v34_station_role role)
{
	return (struct v34_phase3_profile){
		.symbol_rate_numerator = 3000,
		.symbol_rate_denominator = 1,
		.carrier_frequency_numerator = 2000,
		.carrier_frequency_denominator = 1,
		.symbol_rate = V34_SYMBOL_RATE_3000,
		.local_role = role,
		.transmit_power_reduction_db = 2,
		.preemphasis_filter_index = 3,
		.projected_data_rate_2400_bps_units = 12,
		.high_carrier = true,
	};
}

static struct v34_data_direction_parameters parameters_fixture(void)
{
	return (struct v34_data_direction_parameters){
		.precoder_coefficients = {
			{ 4096, 2048 },
			{ -2048, 1024 },
			{ 1024, -2048 },
		},
		.trellis_selection = V34_TRELLIS_32_STATE,
		.nonlinear_parameter = V34_NONLINEAR_THETA_5_OVER_16,
		.primary_data_rate_2400_bps_units = 8,
		.expanded_shaping = true,
	};
}

static bool test_prepared_b1_mapping(void)
{
	struct v34_phase3_profile caller =
		profile_fixture(V34_CALLING_STATION);
	struct v34_phase3_profile answerer =
		profile_fixture(V34_ANSWERING_STATION);
	struct v34_data_direction_parameters parameters = parameters_fixture();
	struct v34_data_mapping first;
	struct v34_data_mapping repeated;
	struct v34_data_mapping reverse;

	CHECK(v34_data_mapping_prepare(
		      &first, &caller, &parameters, true) ==
		      V34_DATA_MAPPING_OK &&
	      v34_data_mapping_prepare(
		      &repeated, &caller, &parameters, true) ==
		      V34_DATA_MAPPING_OK &&
	      v34_data_mapping_prepare(
		      &reverse, &answerer, &parameters, true) ==
		      V34_DATA_MAPPING_OK &&
	      v34_data_mapping_is_valid(&first) &&
	      v34_data_mapping_is_valid(&repeated) &&
	      v34_data_mapping_is_valid(&reverse),
	      "valid directional mappings were not prepared");
	CHECK(first.schedule.geometry.symbol_rate == V34_SYMBOL_RATE_3000 &&
	      first.schedule.geometry.primary_data_rate_2400_bps_units == 8 &&
	      first.schedule.geometry.auxiliary_channel_enabled &&
	      first.schedule.data_frame_in_superframe + 1u ==
		      first.schedule.geometry.data_frames_per_superframe &&
	      first.schedule.next_mapping_frame == 0 &&
	      first.scrambler.history == 0 &&
	      first.scrambler.polynomial == V34_POLYNOMIAL_GPC &&
	      reverse.scrambler.polynomial == V34_POLYNOMIAL_GPA,
	      "prepared mapping did not begin at the directional B1 boundary");
	CHECK(first.encoder.convolutional_encoder.selection ==
		      parameters.trellis_selection &&
	      first.encoder.convolutional_encoder.state == 0 &&
	      first.encoder.previous_differential_output == 0 &&
	      memcmp(first.encoder.precoder.coefficients,
		     parameters.precoder_coefficients,
		     sizeof(parameters.precoder_coefficients)) == 0 &&
	      first.normalizer.nonlinear_parameter ==
		      parameters.nonlinear_parameter &&
	      v34_test_encoders_are_equal(&first.encoder, &repeated.encoder) &&
	      v34_normalizers_equal(&first.normalizer, &repeated.normalizer),
	      "directional mapping state or deterministic calibration changed");
	return true;
}

static bool test_validation_and_transactionality(void)
{
	struct v34_phase3_profile profile =
		profile_fixture(V34_CALLING_STATION);
	struct v34_data_direction_parameters parameters = parameters_fixture();
	struct v34_data_mapping destination;
	struct v34_data_mapping prepared;
	unsigned char saved[sizeof(destination)];

	memset(&destination, 0xa5, sizeof(destination));
	memcpy(saved, &destination, sizeof(saved));
	CHECK(v34_data_mapping_prepare(
		      NULL, &profile, &parameters, false) ==
		      V34_DATA_MAPPING_INVALID_ARGUMENT &&
	      v34_data_mapping_prepare(
		      &destination, NULL, &parameters, false) ==
		      V34_DATA_MAPPING_INVALID_ARGUMENT &&
	      v34_data_mapping_prepare(
		      &destination, &profile, NULL, false) ==
		      V34_DATA_MAPPING_INVALID_ARGUMENT &&
	      memcmp(&destination, saved, sizeof(destination)) == 0,
	      "invalid arguments changed the mapping destination");
	parameters.precoder_coefficients[0].real = INT16_MIN;
	CHECK(v34_data_mapping_prepare(
		      &destination, &profile, &parameters, false) ==
		      V34_DATA_MAPPING_INVALID_ARGUMENT &&
	      memcmp(&destination, saved, sizeof(destination)) == 0,
	      "invalid direction parameters changed the destination");
	parameters = parameters_fixture();
	parameters.primary_data_rate_2400_bps_units = 14;
	profile.projected_data_rate_2400_bps_units = 14;
	CHECK(v34_data_mapping_prepare(
		      &destination, &profile, &parameters, false) ==
		      V34_DATA_MAPPING_OUT_OF_RANGE &&
	      memcmp(&destination, saved, sizeof(destination)) == 0,
	      "unsupported symbol-rate geometry changed the destination");
	parameters = parameters_fixture();
	profile = profile_fixture(V34_CALLING_STATION);
	profile.projected_data_rate_2400_bps_units = 4;
	CHECK(v34_data_mapping_prepare(
		      &prepared, &profile, &parameters, false) ==
		      V34_DATA_MAPPING_OK &&
	      v34_data_mapping_is_valid(&prepared) &&
	      prepared.schedule.geometry
			      .primary_data_rate_2400_bps_units == 8,
	      "Phase 2 projection incorrectly limited Phase 4 mapping");

	parameters = parameters_fixture();
	profile = profile_fixture(V34_CALLING_STATION);
	CHECK(v34_data_mapping_prepare(
		      &prepared, &profile, &parameters, false) ==
		      V34_DATA_MAPPING_OK,
	      "validation fixture did not initialize");
	prepared.scrambler.history = 1;
	CHECK(!v34_data_mapping_is_valid(NULL) &&
	      !v34_data_mapping_is_valid(&prepared),
	      "advanced scrambler was accepted as a fresh B1 mapping");
	CHECK(v34_data_mapping_prepare(
		      &prepared, &profile, &parameters, false) ==
		      V34_DATA_MAPPING_OK,
	      "validation fixture did not reinitialize");
	prepared.encoder.previous_differential_output = 1;
	CHECK(!v34_data_mapping_is_valid(&prepared),
	      "advanced encoder was accepted as a fresh mapping");
	return true;
}

static bool test_calibration_output_ownership(void)
{
	struct v34_phase3_profile profile = profile_fixture(V34_CALLING_STATION);
	struct v34_data_direction_parameters parameters = parameters_fixture();
	union {
		struct v34_data_mapping mapping;
		struct v34_mapping_power_calibration calibration;
		struct { max_align_t prefix; struct v34_data_mapping value; } shifted_mapping;
		struct { max_align_t prefix; struct v34_mapping_power_calibration value; } shifted_job;
	} storage;
	unsigned char saved[sizeof(storage)];

	for (unsigned placement = 0; placement < 3; ++placement) {
		struct v34_data_mapping *mapping = placement == 1 ?
			&storage.shifted_mapping.value : &storage.mapping;
		struct v34_mapping_power_calibration *calibration = placement == 2 ?
			&storage.shifted_job.value : &storage.calibration;

		memset(&storage, 0xa5, sizeof(storage));
		memcpy(saved, &storage, sizeof(saved));
		CHECK(v34_data_mapping_begin_calibration(mapping, calibration,
			      &profile, &parameters, true) ==
			      V34_DATA_MAPPING_INVALID_ARGUMENT &&
		      memcmp(saved, &storage, sizeof(saved)) == 0,
		      "overlapping mapping/calibration outputs were not rejected unchanged");
	}
	return true;
}

static bool test_calibration_input_snapshot(void)
{
	struct v34_phase3_profile profile = profile_fixture(V34_ANSWERING_STATION);
	struct v34_data_direction_parameters parameters = parameters_fixture();
	struct v34_data_mapping reference;
	struct {
		struct v34_data_mapping mapping;
		union {
			struct v34_mapping_power_calibration calibration;
			struct {
				struct v34_phase3_profile profile;
				struct v34_data_direction_parameters parameters;
			} inputs;
		} job;
	} storage;

	CHECK(v34_data_mapping_prepare(&reference, &profile, &parameters, true) ==
		      V34_DATA_MAPPING_OK, "reference mapping did not initialize");
	/* Adjacent outputs, then profile in the mapping, then both inputs in the job. */
	for (unsigned placement = 0; placement < 3; ++placement) {
		const struct v34_phase3_profile *input_profile = &profile;
		const struct v34_data_direction_parameters *input_parameters = &parameters;

		memset(&storage, 0xa5, sizeof(storage));
		if (placement == 1) {
			storage.mapping.transmitting_profile = profile;
			input_profile = &storage.mapping.transmitting_profile;
		} else if (placement == 2) {
			storage.job.inputs.profile = profile;
			storage.job.inputs.parameters = parameters;
			input_profile = &storage.job.inputs.profile;
			input_parameters = &storage.job.inputs.parameters;
		}
		CHECK(v34_data_mapping_begin_calibration(&storage.mapping,
			      &storage.job.calibration, input_profile, input_parameters, true) ==
			      V34_DATA_MAPPING_OK &&
		      v34_phase3_profile_is_equal(
			      &storage.mapping.transmitting_profile, &profile),
		      "calibration output overwrote a profile input before its snapshot");
		CHECK(v34_mapping_power_calibration_step(&storage.job.calibration, 1,
			      &storage.mapping.normalizer) == V34_POWER_CALIBRATION_IN_PROGRESS &&
		      storage.job.calibration.frame == 1 &&
		      v34_mapping_power_calibration_step(&storage.job.calibration, UINT32_MAX,
			      &storage.mapping.normalizer) == V34_POWER_CALIBRATION_OK &&
		      v34_data_mapping_is_valid(&storage.mapping) &&
		      v34_test_encoders_are_equal(&storage.mapping.encoder, &reference.encoder) &&
		      v34_normalizers_equal(&storage.mapping.normalizer, &reference.normalizer),
		      "input/output placement changed bounded calibration or the prepared mapping");
	}
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "prepared B1 mapping", test_prepared_b1_mapping },
		{ "calibration output ownership", test_calibration_output_ownership },
		{ "calibration input snapshot", test_calibration_input_snapshot },
		{ "validation and transactionality",
		  test_validation_and_transactionality },
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
