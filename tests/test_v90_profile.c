// SPDX-License-Identifier: BSD-3-Clause

#include <v90_profile.h>

#include "test.h"

#include <string.h>

static bool test_generated_profiles(void)
{
	for (unsigned int law_index = G711_LAW_PCMU;
	     law_index <= G711_LAW_PCMA; ++law_index) {
		enum g711_law law = (enum g711_law)law_index;

		for (unsigned int role_index = V90_ANALOG_MODEM;
		     role_index <= V90_DIGITAL_MODEM; ++role_index) {
			enum v90_modem_role role =
				(enum v90_modem_role)role_index;
			struct v90_call_configuration configuration;
			struct v90_call call;

			CHECK(v90_profile_init(
				      &configuration, role, law, 45, 56000) ==
				      V90_PROFILE_OK &&
			      v90_call_init(&call, &configuration) == V90_CALL_OK &&
			      v90_call_is_valid(&call) && call.local_role == role &&
			      call.law == law,
			      "role %u law %u profile did not initialize",
			      role_index, law_index);
			if (role == V90_ANALOG_MODEM) {
				struct v90_pcm_mapping mapping;

				CHECK(configuration.analog_phase4.training_parameters
					      .kind == V90_CP_TRAINING &&
				      configuration.analog_phase4.data_parameters
					      .kind == V90_CP_DATA &&
				      v90_pcm_mapping_init(
					      &mapping,
					      &configuration.analog_phase4
						       .data_parameters) ==
					      V90_PCM_MAPPING_OK &&
				      (unsigned int)mapping.frame_bits * 8000u /
					      V90_CP_FRAME_INTERVALS ==
					      V90_PROFILE_DOWNSTREAM_BIT_RATE,
				      "analogue profile did not own the initial rate");
			} else {
				CHECK(configuration.local_info0.digital.pcm_law ==
					      (law == G711_LAW_PCMA ?
						       V90_PCM_A_LAW : V90_PCM_MU_LAW) &&
				      v90_mp_record_is_valid(
					      &configuration.digital_phase4.initial_mp),
				      "digital profile did not match its media law");
			}
		}
	}
	return true;
}

static bool test_transactionality(void)
{
	struct v90_call_configuration configuration;
	struct v90_call_configuration saved;

	memset(&configuration, 0xa5, sizeof(configuration));
	saved = configuration;
	CHECK(v90_profile_init(
		      NULL, V90_ANALOG_MODEM, G711_LAW_PCMU, 45, 56000) ==
		      V90_PROFILE_INVALID_ARGUMENT &&
	      v90_profile_init(
		      &configuration, (enum v90_modem_role)-1,
		      G711_LAW_PCMU, 45, 56000) ==
		      V90_PROFILE_INVALID_ARGUMENT &&
	      v90_profile_init(
		      &configuration, V90_ANALOG_MODEM,
		      (enum g711_law)-1, 45, 56000) ==
		      V90_PROFILE_INVALID_ARGUMENT &&
	      v90_profile_init(
		      &configuration, V90_ANALOG_MODEM,
		      G711_LAW_PCMU, 56000, 45) ==
		      V90_PROFILE_INVALID_ARGUMENT &&
	      v90_profile_init(
		      &configuration, V90_ANALOG_MODEM,
		      G711_LAW_PCMU, 45,
		      V90_PROFILE_DOWNSTREAM_BIT_RATE - 1u) ==
		      V90_PROFILE_UNSUPPORTED_RATE &&
	      v90_profile_init(
		      &configuration, V90_ANALOG_MODEM,
		      G711_LAW_PCMU,
		      V90_PROFILE_DOWNSTREAM_BIT_RATE + 1u, 56000) ==
		      V90_PROFILE_UNSUPPORTED_RATE &&
	      memcmp(&configuration, &saved, sizeof(configuration)) == 0,
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
		{ "generated role and media profiles", test_generated_profiles },
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
