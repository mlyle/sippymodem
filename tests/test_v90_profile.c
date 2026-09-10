// SPDX-License-Identifier: BSD-3-Clause

#include <v90_profile.h>
#include <v90_info0.h>
#include <v90_jd.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

enum {
	PHASE2_NOMINAL_AMPLITUDE = 5320,
	DIGITAL_PHASE2_NOMINAL_POWER_CODE = 4,
	DIGITAL_PHASE2_MAXIMUM_POWER_CODE = 23,
};

static uint32_t expected_bit_rate(uint8_t rate_index)
{
	return ((uint32_t)(PCM_DOWNSTREAM_BASE_FRAME_BITS + rate_index) *
		8000u + 3u) / 6u;
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
	for (uint8_t index = PCM_DOWNSTREAM_MIN_DATA_RATE_NUMBER;
	     index <= PCM_DOWNSTREAM_MAX_DATA_RATE_NUMBER; ++index) {
		struct pcm_downstream_rate_selection selection;
		uint32_t bit_rate = expected_bit_rate(index);

		CHECK(pcm_downstream_bit_rate(index) == bit_rate &&
		      pcm_downstream_rates_select(
			      &selection, bit_rate, bit_rate) &&
		      selection.enabled_mask ==
			      (UINT32_C(1) << (index - 1u)) &&
		      selection.selected_data_rate_number == index &&
		      selection.selected_bit_rate == bit_rate,
		      "V.90 downstream index %u did not select %u bit/s",
		      index, bit_rate);
	}
	{
		struct pcm_downstream_rate_selection selection;

		CHECK(pcm_downstream_rates_select(
			      &selection, 45,
			      V90_PROFILE_MAX_DOWNSTREAM_BIT_RATE) &&
		      selection.enabled_mask ==
			      V90_JD_RATE_CAPABILITY_MASK &&
		      selection.selected_data_rate_number ==
			      PCM_DOWNSTREAM_MAX_DATA_RATE_NUMBER &&
		      selection.selected_bit_rate ==
			      V90_PROFILE_MAX_DOWNSTREAM_BIT_RATE &&
		      pcm_downstream_bit_rate(0) == 0 &&
		      pcm_downstream_bit_rate(
			      PCM_DOWNSTREAM_MAX_DATA_RATE_NUMBER + 1u) == 0,
		      "broad V.90 downstream range did not select all rates");
	}
	return true;
}

static bool test_digital_info0_profile(void)
{
	static const uint8_t expected[][V90_INFO0D_PAYLOAD_BYTES] = {
		[G711_LAW_PCMU] = { 0xff, 0x84, 0x17, 0x68 },
		[G711_LAW_PCMA] = { 0xff, 0x84, 0x17, 0x78 },
	};

	for (unsigned int law_index = G711_LAW_PCMU;
	     law_index <= G711_LAW_PCMA; ++law_index) {
		enum g711_law law = (enum g711_law)law_index;
		struct pcm_call_configuration configuration;
		uint8_t payload[V90_INFO0D_PAYLOAD_BYTES];

		CHECK(v90_profile_init(
			      &configuration, V90_DIGITAL_MODEM, law,
			      28000, 56000, NULL) == V90_PROFILE_OK &&
		      v90_info0d_encode(
			      &configuration.local_info0.digital, payload,
			      sizeof(payload)) == V90_INFO0_OK &&
		      memcmp(payload, expected[law_index], sizeof(payload)) == 0,
		      "digital V.90 law %u INFO0d profile changed", law_index);
	}
	return true;
}

static bool test_generated_profiles(void)
{
	static const uint8_t maximum_rate_jd[V90_JD_FRAME_BYTES] = {
		0xff, 0xff, 0x80, 0x00, 0x00, 0x80, 0x46, 0x84, 0x60,
	};

	for (unsigned int law_index = G711_LAW_PCMU;
	     law_index <= G711_LAW_PCMA; ++law_index) {
		enum g711_law law = (enum g711_law)law_index;

		for (uint8_t rate_index = PCM_DOWNSTREAM_MIN_DATA_RATE_NUMBER;
		     rate_index <= PCM_DOWNSTREAM_MAX_DATA_RATE_NUMBER;
		     ++rate_index) {
			uint32_t bit_rate = expected_bit_rate(rate_index);

			for (unsigned int role_index = V90_ANALOG_MODEM;
			     role_index <= V90_DIGITAL_MODEM; ++role_index) {
				enum v90_modem_role role =
					(enum v90_modem_role)role_index;
				struct pcm_call_configuration configuration;
				struct pcm_call call;
				const struct v34_phase2_tone_transmitter *tone;

				CHECK(v90_profile_init(
					      &configuration, role, law,
					      bit_rate, bit_rate, NULL) == V90_PROFILE_OK &&
				      pcm_call_init(&call, &configuration) ==
					      PCM_CALL_OK &&
				      pcm_call_is_valid(&call) &&
				      call.local_role == role && call.law == law &&
				      configuration.phase2_nominal_amplitude ==
					      PHASE2_NOMINAL_AMPLITUDE &&
				      call.phase2_nominal_amplitude ==
					      configuration.phase2_nominal_amplitude,
				      "role %u law %u rate %u profile did not initialize",
				      role_index, law_index, bit_rate);
				CHECK((role == V90_DIGITAL_MODEM ?
					       call.local_info1.digital.common
						       .frequency_offset_1_50_hz_units :
					       call.local_info1.analog
						       .frequency_offset_1_50_hz_units) ==
					      V34_INFO1_FREQUENCY_OFFSET_UNAVAILABLE,
				      "V.90 profile advertised an unmeasured frequency offset");
				tone = &call.state.phase2.state.ranging.info0_exchange
						.transport.tone_transmitter;
				CHECK(role == V90_DIGITAL_MODEM ?
					      tone->carrier_amplitude ==
						      PHASE2_NOMINAL_AMPLITUDE &&
						      tone->guard_amplitude == 0 :
					      tone->guard_amplitude ==
						      PHASE2_NOMINAL_AMPLITUDE,
				      "role %u law %u rate %u Phase 2 level changed",
				      role_index, law_index, bit_rate);
				if (role == V90_ANALOG_MODEM) {
					struct v90_pcm_mapping mapping;
					uint64_t training_power;
					uint64_t data_power;
					uint8_t training_points;
					uint8_t data_points;

					CHECK(configuration.local_info1.analog
						      .local_md_length_35_ms_units == 0 &&
					      configuration.phase3.analog.dil_profile ==
						      V90_DIL_PROFILE_CONVENTIONAL &&
					      configuration.analog_phase4
						      .training_parameters.kind ==
						      V90_CP_TRAINING &&
					      configuration.analog_phase4
						      .training_parameters.sign_redundancy == 0 &&
					      configuration.analog_phase4
						      .training_parameters.spectral_lookahead == 0 &&
					      configuration.analog_phase4
						      .training_parameters.shaping_a1_q1_6 == 0 &&
					      configuration.analog_phase4
						      .training_parameters.shaping_a2_q1_6 == 0 &&
					      configuration.analog_phase4
						      .training_parameters.shaping_b1_q1_6 == 0 &&
					      configuration.analog_phase4
						      .training_parameters.shaping_b2_q1_6 == 0 &&
					      configuration.analog_phase4
						      .data_parameters.kind ==
						      V90_CP_DATA &&
					      configuration.analog_phase4
						      .data_parameters.sign_redundancy == 1 &&
					      configuration.analog_phase4
						      .data_parameters.spectral_lookahead == 1 &&
					      configuration.analog_phase4
						      .data_parameters
						      .selected_rate_index == rate_index &&
					      v90_pcm_mapping_init(
						      &mapping,
						      &configuration.analog_phase4
						       .data_parameters) ==
						      V90_PCM_MAPPING_OK &&
					      mapping.frame_bits ==
						      PCM_DOWNSTREAM_BASE_FRAME_BITS +
							      rate_index &&
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
					uint8_t jd[V90_JD_FRAME_BYTES];
					bool jd_matches =
						rate_index !=
							PCM_DOWNSTREAM_MAX_DATA_RATE_NUMBER ||
						(v90_jd_encode(
							 &configuration.phase3.digital.jd,
							 jd, sizeof(jd)) == V90_JD_OK &&
						 memcmp(jd, maximum_rate_jd, sizeof(jd)) == 0);

					CHECK(configuration.local_info0.digital
						      .nominal_transmit_power ==
						      DIGITAL_PHASE2_NOMINAL_POWER_CODE &&
					      configuration.local_info0.digital
						      .maximum_transmit_power ==
						      DIGITAL_PHASE2_MAXIMUM_POWER_CODE &&
					      configuration.local_info0.digital.pcm_law ==
						      (law == G711_LAW_PCMA ?
							       V90_PCM_A_LAW :
							       V90_PCM_MU_LAW) &&
					      configuration.phase3.digital.jd
						      .training_constellation ==
						      V34_CONSTELLATION_4_POINT &&
					      configuration.phase3.digital.jd
						      .maximum_spectral_shaping_lookahead == 1 &&
					      configuration.phase3.digital.jd
						      .data_rate_capability_mask ==
						      (UINT32_C(1) <<
						       (rate_index - 1u)) &&
					      jd_matches &&
					      !configuration.digital_phase4.initial_mp
					       .parameters.type0
					       .upstream_nonlinear_encoding &&
					      configuration.digital_phase4.initial_mp
					      .parameters.type0
					      .upstream_expanded_shaping &&
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
	const struct v90_upstream_policy mandatory_fallback = {
		.minimum_bit_rate = 19200,
		.maximum_bit_rate = 19200,
		.maximum_symbol_rate = 3000,
	};
	const uint16_t fallback_mask = UINT16_C(1) << (8u - 2u);
	const struct v90_upstream_policy conservative_info1d = {
		.minimum_bit_rate = 4800,
		.maximum_bit_rate = 31200,
		.maximum_symbol_rate = 3429,
		.suppress_optional_info1d_3429 = true,
	};
	const struct v90_upstream_policy maximum_upstream = {
		.minimum_bit_rate = 33600,
		.maximum_bit_rate = 33600,
		.maximum_symbol_rate = 3429,
	};
	struct pcm_call_configuration analog;
	struct pcm_call_configuration digital;
	struct pcm_call_configuration conservative_digital;
	struct pcm_call_configuration maximum_digital;
	struct pcm_call_configuration fallback_analog;
	struct pcm_call_configuration fallback_digital;
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
		      analog.local_info0.analog.low_carrier_3200_supported &&
		      analog.local_info0.analog.high_carrier_3200_supported &&
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
		      digital.local_info0.digital.common
			      .low_carrier_3200_supported &&
		      digital.local_info0.digital.common
			      .high_carrier_3200_supported &&
		      digital.local_info1.digital
			      .answer_transmit[V34_SYMBOL_RATE_3200]
			      .projected_data_rate_2400_bps_units == 13 &&
		      digital.local_info1.digital
			      .answer_transmit[V34_SYMBOL_RATE_3429]
			      .projected_data_rate_2400_bps_units == 0 &&
		      digital.digital_phase4.initial_mp.type ==
			      V90_MP_RECORD_TYPE0 &&
		      digital.digital_phase4.initial_mp.parameters.type0
			      .maximum_upstream_rate_2400_bps_units == 11 &&
		      digital.digital_phase4.initial_mp.parameters.type0
			      .upstream_trellis == V34_TRELLIS_16_STATE &&
		      !digital.digital_phase4.initial_mp.parameters.type0
			       .upstream_nonlinear_encoding &&
		      digital.digital_phase4.initial_mp.parameters.type0
			      .upstream_expanded_shaping &&
		      digital.digital_phase4.initial_mp.parameters.type0
			      .upstream_rate_capability_mask == expected_mask,
	      "digital profile did not apply its upstream policy");
	CHECK(v90_profile_init(
		      &fallback_analog, V90_ANALOG_MODEM, G711_LAW_PCMU,
		      28000, 28000, &mandatory_fallback) == V90_PROFILE_OK &&
	      v90_profile_init(
		      &fallback_digital, V90_DIGITAL_MODEM, G711_LAW_PCMU,
		      28000, 28000, &mandatory_fallback) == V90_PROFILE_OK &&
	      fallback_analog.local_info1.analog.upstream_symbol_rate ==
		      V34_SYMBOL_RATE_3000 &&
	      fallback_digital.local_info1.digital
		      .answer_transmit[V34_SYMBOL_RATE_3000]
		      .projected_data_rate_2400_bps_units == 12 &&
	      fallback_digital.local_info1.digital
		      .answer_transmit[V34_SYMBOL_RATE_3200]
		      .projected_data_rate_2400_bps_units == 13 &&
	      fallback_digital.local_info1.digital
		      .answer_transmit[V34_SYMBOL_RATE_3429]
		      .projected_data_rate_2400_bps_units == 0 &&
	      fallback_analog.analog_phase4.training_parameters
		      .upstream_rate_mask == fallback_mask &&
	      fallback_analog.analog_phase4.data_parameters
		      .upstream_rate_mask == fallback_mask &&
	      fallback_digital.digital_phase4.initial_mp.parameters.type0
		      .maximum_upstream_rate_2400_bps_units == 8 &&
	      fallback_digital.digital_phase4.initial_mp.parameters.type0
		      .upstream_rate_capability_mask == fallback_mask,
	      "3000-symbol/s preference withdrew mandatory 3200 fallback");
	CHECK(v90_profile_init(
		      &conservative_digital, V90_DIGITAL_MODEM,
		      G711_LAW_PCMU, 28000, 56000,
		      &conservative_info1d) == V90_PROFILE_OK &&
	      conservative_digital.local_info0.digital.common
		      .symbol_rate_3429_supported &&
	      conservative_digital.local_info1.digital
		      .answer_transmit[V34_SYMBOL_RATE_3000]
		      .projected_data_rate_2400_bps_units == 12 &&
	      conservative_digital.local_info1.digital
		      .answer_transmit[V34_SYMBOL_RATE_3200]
		      .projected_data_rate_2400_bps_units == 13 &&
	      conservative_digital.local_info1.digital
		      .answer_transmit[V34_SYMBOL_RATE_3429]
		      .projected_data_rate_2400_bps_units == 0 &&
	      conservative_digital.digital_phase4.initial_mp.parameters.type0
		      .maximum_upstream_rate_2400_bps_units == 13 &&
	      !conservative_digital.digital_phase4.initial_mp.parameters.type0
		       .upstream_nonlinear_encoding,
	      "conservative INFO1d policy changed mandatory upstream rates");
	CHECK(v90_profile_init(
		      &maximum_digital, V90_DIGITAL_MODEM, G711_LAW_PCMU,
		      28000, 56000, &maximum_upstream) == V90_PROFILE_OK &&
	      maximum_digital.digital_phase4.initial_mp.type ==
		      V90_MP_RECORD_TYPE0 &&
	      maximum_digital.digital_phase4.initial_mp.parameters.type0
		      .maximum_upstream_rate_2400_bps_units == 14u &&
	      !maximum_digital.digital_phase4.initial_mp.parameters.type0
		       .upstream_nonlinear_encoding,
	      "maximum upstream profile changed its linear rate policy");
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
	struct pcm_downstream_rate_selection selection;
	struct pcm_downstream_rate_selection saved_selection;

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
	      !pcm_downstream_rates_select(NULL, 28000, 56000) &&
	      !pcm_downstream_rates_select(&selection, 56000, 28000) &&
	      !pcm_downstream_rates_select(&selection, 28001, 29332) &&
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
		{ "digital INFO0 profile", test_digital_info0_profile },
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
