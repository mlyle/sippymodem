// SPDX-License-Identifier: BSD-3-Clause

#include <v90_profile.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

enum {
	DATA_RATE_BASE = 20,
};

static uint32_t expected_bit_rate(uint8_t rate_index)
{
	return ((uint32_t)(DATA_RATE_BASE + rate_index) * 8000u + 3u) / 6u;
}

static bool constellation_power(
	const struct pcm_constellation *constellation, enum g711_law law,
	uint64_t *power, uint8_t *count)
{
	uint64_t sum = 0;
	uint8_t members = 0;

	if (constellation == NULL || power == NULL || count == NULL)
		return false;
	for (uint8_t ucode = 0; ucode < PCM_CONSTELLATION_UCODES;
	     ++ucode) {
		int16_t sample;

		if (!pcm_constellation_contains(constellation, ucode))
			continue;
		if (!g711_ucode_sample(law, ucode, true, &sample))
			return false;
		sum += (uint64_t)((int32_t)sample * sample);
		members++;
	}
	if (members == 0)
		return false;
	*power = sum;
	*count = members;
	return true;
}

static bool test_downstream_rate_selection(void)
{
	for (uint8_t index = 1;
	     index <= V90_PROFILE_DOWNSTREAM_RATE_COUNT; ++index) {
		struct v90_downstream_selection selection;
		uint32_t bit_rate = expected_bit_rate(index);

		CHECK(v90_profile_downstream_bit_rate(index) == bit_rate &&
		      v90_profile_select_downstream(
			      &selection, bit_rate, bit_rate) == V90_PROFILE_OK &&
		      selection.enabled_rate_mask ==
			      (UINT32_C(1) << (index - 1u)) &&
		      selection.selected_rate_index == index &&
		      selection.selected_bit_rate == bit_rate,
		      "V.90 downstream index %u did not select %u bit/s",
		      index, bit_rate);
	}
	{
		struct v90_downstream_selection selection;

		CHECK(v90_profile_select_downstream(
			      &selection, 45,
			      V90_PROFILE_MAX_DOWNSTREAM_BIT_RATE) ==
			      V90_PROFILE_OK &&
		      selection.enabled_rate_mask ==
			      V90_JD_RATE_CAPABILITY_MASK &&
		      selection.selected_rate_index ==
			      V90_PROFILE_DOWNSTREAM_RATE_COUNT &&
		      selection.selected_bit_rate ==
			      V90_PROFILE_MAX_DOWNSTREAM_BIT_RATE &&
		      v90_profile_downstream_bit_rate(0) == 0 &&
		      v90_profile_downstream_bit_rate(
			      V90_PROFILE_DOWNSTREAM_RATE_COUNT + 1u) == 0,
		      "broad V.90 downstream range did not select all rates");
	}
	return true;
}

static bool test_generated_profiles(void)
{
	for (unsigned int law_index = G711_LAW_PCMU;
	     law_index <= G711_LAW_PCMA; ++law_index) {
		enum g711_law law = (enum g711_law)law_index;

		for (uint8_t rate_index = 1;
		     rate_index <= V90_PROFILE_DOWNSTREAM_RATE_COUNT;
		     ++rate_index) {
			uint32_t bit_rate = expected_bit_rate(rate_index);

			for (unsigned int role_index = V90_ANALOG_MODEM;
			     role_index <= V90_DIGITAL_MODEM; ++role_index) {
				enum v90_modem_role role =
					(enum v90_modem_role)role_index;
				struct pcm_call_configuration configuration;
				struct pcm_call call;

				CHECK(v90_profile_init(
					      &configuration, role, law,
					      bit_rate, bit_rate, NULL) == V90_PROFILE_OK &&
				      pcm_call_init(&call, &configuration) ==
					      PCM_CALL_OK &&
				      pcm_call_is_valid(&call) &&
				      call.local_role == role && call.law == law,
				      "role %u law %u rate %u profile did not initialize",
				      role_index, law_index, bit_rate);
				if (role == V90_ANALOG_MODEM) {
					struct v90_pcm_mapping mapping;
					uint64_t training_power;
					uint64_t data_power;
					uint8_t training_points;
					uint8_t data_points;

					CHECK(configuration.analog_phase4
						      .training_parameters.kind ==
						      V90_CP_TRAINING &&
					      configuration.analog_phase4
						      .data_parameters.kind ==
						      V90_CP_DATA &&
					      configuration.analog_phase4
						      .data_parameters
						      .selected_rate_index == rate_index &&
					      v90_pcm_mapping_init(
						      &mapping,
						      &configuration.analog_phase4
						       .data_parameters) ==
						      V90_PCM_MAPPING_OK &&
					      mapping.frame_bits ==
						      DATA_RATE_BASE + rate_index &&
					      constellation_power(
						      &configuration.analog_phase4
						       .training_parameters
						       .transmit_constellations[0],
						      law, &training_power,
						      &training_points) &&
					      constellation_power(
						      &configuration.analog_phase4
						       .data_parameters
						       .transmit_constellations[0],
						      law, &data_power, &data_points) &&
					      data_power * training_points <=
						      2u * training_power * data_points,
					      "analogue profile did not generate rate %u",
					      bit_rate);
				} else {
					CHECK(configuration.local_info0.digital.pcm_law ==
						      (law == G711_LAW_PCMA ?
							       V90_PCM_A_LAW :
							       V90_PCM_MU_LAW) &&
					      configuration.phase3.digital.jd
						      .training_constellation ==
						      V34_CONSTELLATION_4_POINT &&
					      configuration.phase3.digital.jd
						      .data_rate_capability_mask ==
						      (UINT32_C(1) <<
						       (rate_index - 1u)) &&
					      v90_mp_record_is_valid(
						      &configuration.digital_phase4
						       .initial_mp),
					      "digital profile did not advertise rate %u",
					      bit_rate);
				}
			}
		}
	}
	return true;
}

static bool test_upstream_policy(void)
{
	const struct v90_upstream_policy policy = {
		.minimum_bit_rate = 26400,
		.maximum_bit_rate = 26400,
		.maximum_symbol_rate = 3200,
	};
	const uint16_t expected_mask = UINT16_C(1) << (11u - 2u);
	struct pcm_call_configuration analog;
	struct pcm_call_configuration digital;
	struct pcm_call_configuration rejected;
	struct pcm_call_configuration saved;
	const struct v90_upstream_policy impossible = {
		.minimum_bit_rate = 31200,
		.maximum_bit_rate = 31200,
		.maximum_symbol_rate = 3000,
	};

	CHECK(v90_profile_init(
		      &analog, V90_ANALOG_MODEM, G711_LAW_PCMU,
		      28000, 28000, &policy) == V90_PROFILE_OK &&
		      analog.local_info1.analog.upstream_symbol_rate ==
			      V34_SYMBOL_RATE_3200 &&
		      analog.analog_phase4.training_parameters
			      .upstream_rate_mask == expected_mask &&
		      analog.analog_phase4.data_parameters
			      .upstream_rate_mask == expected_mask,
	      "analogue profile did not apply its upstream policy");
	CHECK(v90_profile_init(
		      &digital, V90_DIGITAL_MODEM, G711_LAW_PCMA,
		      28000, 28000, &policy) == V90_PROFILE_OK &&
		      digital.local_info1.digital
			      .answer_transmit[V34_SYMBOL_RATE_3200]
			      .projected_data_rate_2400_bps_units == 11 &&
		      digital.local_info1.digital
			      .answer_transmit[V34_SYMBOL_RATE_3429]
			      .projected_data_rate_2400_bps_units == 0 &&
		      digital.digital_phase4.initial_mp.type ==
			      V90_MP_RECORD_TYPE0 &&
		      digital.digital_phase4.initial_mp.parameters.type0
			      .maximum_upstream_rate_2400_bps_units == 11 &&
		      digital.digital_phase4.initial_mp.parameters.type0
			      .upstream_rate_capability_mask == expected_mask,
	      "digital profile did not apply its upstream policy");
	memset(&rejected, 0xa5, sizeof(rejected));
	saved = rejected;
	CHECK(v90_profile_init(
		      &rejected, V90_DIGITAL_MODEM, G711_LAW_PCMU,
		      28000, 28000, &impossible) ==
			      V90_PROFILE_UNSUPPORTED_RATE &&
		      memcmp(&rejected, &saved, sizeof(rejected)) == 0,
	      "impossible upstream policy changed its destination");
	return true;
}

static bool test_transactionality(void)
{
	struct pcm_call_configuration configuration;
	struct pcm_call_configuration saved;
	struct v90_downstream_selection selection;
	struct v90_downstream_selection saved_selection;

	memset(&configuration, 0xa5, sizeof(configuration));
	saved = configuration;
	memset(&selection, 0x5a, sizeof(selection));
	saved_selection = selection;
	CHECK(v90_profile_init(
		      NULL, V90_ANALOG_MODEM, G711_LAW_PCMU, 45, 56000, NULL) ==
		      V90_PROFILE_INVALID_ARGUMENT &&
	      v90_profile_init(
		      &configuration, (enum v90_modem_role)-1,
		      G711_LAW_PCMU, 45, 56000, NULL) ==
		      V90_PROFILE_INVALID_ARGUMENT &&
	      v90_profile_init(
		      &configuration, V90_ANALOG_MODEM,
		      (enum g711_law)-1, 45, 56000, NULL) ==
		      V90_PROFILE_INVALID_ARGUMENT &&
	      v90_profile_init(
		      &configuration, V90_ANALOG_MODEM,
		      G711_LAW_PCMU, 56000, 45, NULL) ==
		      V90_PROFILE_INVALID_ARGUMENT &&
	      v90_profile_init(
		      &configuration, V90_ANALOG_MODEM,
		      G711_LAW_PCMU, 45,
		      V90_PROFILE_MIN_DOWNSTREAM_BIT_RATE - 1u, NULL) ==
		      V90_PROFILE_UNSUPPORTED_RATE &&
	      v90_profile_init(
		      &configuration, V90_ANALOG_MODEM,
		      G711_LAW_PCMU,
		      V90_PROFILE_MAX_DOWNSTREAM_BIT_RATE + 1u,
		      V90_PROFILE_MAX_DOWNSTREAM_BIT_RATE + 1000u, NULL) ==
		      V90_PROFILE_UNSUPPORTED_RATE &&
	      v90_profile_init(
		      &configuration, V90_ANALOG_MODEM,
		      G711_LAW_PCMU, 28001, 29332, NULL) ==
		      V90_PROFILE_UNSUPPORTED_RATE &&
	      v90_profile_select_downstream(NULL, 28000, 56000) ==
		      V90_PROFILE_INVALID_ARGUMENT &&
	      v90_profile_select_downstream(&selection, 56000, 28000) ==
		      V90_PROFILE_INVALID_ARGUMENT &&
	      v90_profile_select_downstream(&selection, 28001, 29332) ==
		      V90_PROFILE_UNSUPPORTED_RATE &&
	      memcmp(&configuration, &saved, sizeof(configuration)) == 0 &&
	      memcmp(&selection, &saved_selection, sizeof(selection)) == 0,
	      "rejected V.90 profile changed its destination");
	return true;
}

static bool test_result_names(void)
{
	static const char *const expected[] = {
		"ok",
		"invalid argument",
		"unsupported rate",
	};

	for (size_t index = 0; index < ARRAY_LENGTH(expected); ++index)
		CHECK(strcmp(
			      v90_profile_result_name(
				      (enum v90_profile_result)index),
			      expected[index]) == 0,
		      "V.90 profile result %zu changed", index);
	CHECK(strcmp(
		      v90_profile_result_name((enum v90_profile_result)-1),
		      "unknown") == 0,
	      "unknown V.90 profile result changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "downstream rate selection", test_downstream_rate_selection },
		{ "generated role and media profiles", test_generated_profiles },
		{ "upstream policy", test_upstream_policy },
		{ "rate bounds and transactionality", test_transactionality },
		{ "result names", test_result_names },
	};
	int failed = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (!tests[index].run()) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failed++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failed == 0 ? 0 : 1;
}
