// SPDX-License-Identifier: BSD-3-Clause

#include <v92_profile.h>

#include <pcm_rate.h>
#include <v90_pcm_mapping.h>
#include <v92_cpd_direct.h>
#include <v92_pcm_dil.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

enum {
	ANALOG_PHASE2_AMPLITUDE = 12000,
	DIGITAL_PHASE2_AMPLITUDE = 5320,
	DIGITAL_PHASE2_NOMINAL_POWER_CODE = 4,
	DIGITAL_PHASE2_MAXIMUM_POWER_CODE = 23,
};

static uint8_t constellation_members(
	const struct pcm_constellation *constellation)
{
	uint8_t count = 0;

	for (uint8_t ucode = 0; ucode < PCM_CONSTELLATION_UCODES; ++ucode) {
		if (pcm_constellation_contains(constellation, ucode))
			count++;
	}
	return count;
}

static bool cpt_constellation_is_canonical(
	const struct pcm_constellation *constellation)
{
	static const uint8_t expected[] = { 76, 68, 57, 35 };

	if (constellation_members(constellation) !=
	    sizeof(expected) / sizeof(expected[0]))
		return false;
	for (size_t index = 0; index < sizeof(expected) / sizeof(expected[0]);
	     ++index) {
		if (!pcm_constellation_contains(constellation, expected[index]))
			return false;
	}
	return true;
}

static bool test_generated_profiles(void)
{
	for (unsigned int law_index = G711_LAW_PCMU;
	     law_index <= G711_LAW_PCMA; ++law_index) {
		for (unsigned int role_index = V90_ANALOG_MODEM;
		     role_index <= V90_DIGITAL_MODEM; ++role_index) {
			for (uint8_t rate_number =
				     PCM_DOWNSTREAM_MIN_DATA_RATE_NUMBER;
			     rate_number <= PCM_DOWNSTREAM_MAX_DATA_RATE_NUMBER;
			     ++rate_number) {
				struct pcm_downstream_rate_selection downstream;
				struct pcm_call_configuration configuration;
				struct pcm_call call;
				const struct v34_phase2_tone_transmitter *tone;
				struct v90_pcm_mapping cpt_mapping;
				unsigned int rate =
					pcm_downstream_bit_rate(rate_number);
				uint8_t upstream_rate =
					rate_number <=
						V92_PROFILE_MAX_UPSTREAM_DATA_RATE_NUMBER ?
							rate_number :
							rate_number -
							V92_PROFILE_MAX_UPSTREAM_DATA_RATE_NUMBER;
				uint32_t upstream_mask;
				enum g711_law law = (enum g711_law)law_index;
				enum v90_modem_role role =
					(enum v90_modem_role)role_index;

				CHECK(pcm_downstream_rates_select(
					      &downstream, rate, rate) &&
				      v92_pcm_dil_rate_mask_through(
					      upstream_rate, &upstream_mask) &&
				      v92_pcm_profile_init(
					      &configuration, role, law,
					      rate, rate,
					      upstream_rate) ==
					      V92_PROFILE_OK &&
				      pcm_call_init(&call, &configuration) ==
					      PCM_CALL_OK &&
				      pcm_call_is_valid(&call) &&
				      configuration.protocol == PCM_PROTOCOL_V92 &&
				      configuration.enabled_downstream_rate_mask ==
					      downstream.enabled_mask &&
				      configuration.phase2_nominal_amplitude ==
					      (role == V90_DIGITAL_MODEM ?
						       DIGITAL_PHASE2_AMPLITUDE :
						       ANALOG_PHASE2_AMPLITUDE) &&
				      call.phase2_nominal_amplitude ==
					      configuration.phase2_nominal_amplitude,
				      "role %u law %u rate %u profile did not initialize",
				      role_index, law_index, rate);
				tone = &call.state.phase2.state.ranging.info0_exchange
						.transport.tone_transmitter;
				CHECK(role == V90_DIGITAL_MODEM ?
					      tone->carrier_amplitude ==
						      DIGITAL_PHASE2_AMPLITUDE &&
						      tone->guard_amplitude == 0 :
					      tone->guard_amplitude ==
						      ANALOG_PHASE2_AMPLITUDE,
				      "role %u law %u rate %u Phase 2 level changed",
				      role_index, law_index, rate);
				if (role == V90_ANALOG_MODEM) {
					const struct v92_phase3_analog_station_configuration
						*phase3 = &configuration.phase3.v92_analog;

					CHECK(configuration.local_info0.v92_analog
						      .v92_capable &&
					      !configuration.local_info0.v92_analog
						       .short_phase2_requested &&
					      configuration.local_info1.v92_analog
						      .maximum_total_coefficients == 256 &&
					      phase3->upstream_rate_mask == upstream_mask &&
					      phase3->initial_trn1u_symbols ==
						      V92_PROFILE_INITIAL_TRN1U_SYMBOLS &&
					      phase3->cpt.selected_rate_index ==
						      9 &&
					      configuration.v92_analog_phase4.cpu
						      .data_rate_number ==
						      downstream.selected_data_rate_number &&
					      cpt_constellation_is_canonical(
						      &phase3->cpt
						       .transmit_constellations[0]) &&
					      v90_pcm_mapping_init_from_v92_cpt(
						      &cpt_mapping, &phase3->cpt) ==
						      V90_PCM_MAPPING_OK &&
					      v90_pcm_mapping_is_valid(&cpt_mapping) &&
					      cpt_mapping.kind == V90_CP_TRAINING &&
					      cpt_mapping.law == law &&
					      cpt_mapping.frame_bits == 17 &&
					      cpt_mapping.modulus_bits == 12 &&
					      constellation_members(
						      &configuration.v92_analog_phase4.cpu
						       .transmit_constellations[0]) >= 8,
					      "analogue profile did not generate rate %u",
					      rate);
				} else {
					struct v92_cpd_parameters expected;
					const struct v92_cpd_base_parameters *cpd =
						&configuration.v92_digital_phase4.cpd;

					CHECK(configuration.local_info0.v92_digital.v90
						      .nominal_transmit_power ==
						      DIGITAL_PHASE2_NOMINAL_POWER_CODE &&
					      configuration.local_info0.v92_digital.v90
						      .maximum_transmit_power ==
						      DIGITAL_PHASE2_MAXIMUM_POWER_CODE &&
					      configuration.local_info0.v92_digital
						      .v92_capable &&
					      !configuration.local_info0.v92_digital
						       .short_phase2_requested &&
					      configuration.local_info1.v92_digital
						      .pcm_upstream_supported &&
					      configuration.phase3.v92_digital.jd
						      .data_rate_capability_mask ==
						      downstream.enabled_mask &&
					      v92_cpd_direct_init(
						      &expected, law, upstream_rate,
						      false) == V92_CPD_OK &&
					      cpd->four_times_gain_q0_16 ==
						      expected.four_times_gain_q0_16 &&
					      cpd->data_rate_number ==
						      expected.data_rate_number &&
					      cpd->trellis == expected.trellis &&
					      cpd->extend_e2u == expected.extend_e2u &&
					      cpd->acknowledges_cpu ==
						      expected.acknowledges_cpu,
					      "digital profile did not generate rate %u",
					      rate);
				}
			}
		}
	}
	return true;
}

static bool test_transactionality(void)
{
	struct pcm_call_configuration configuration;
	struct pcm_call_configuration saved;

	memset(&configuration, 0xa5, sizeof(configuration));
	saved = configuration;
	CHECK(v92_pcm_profile_init(
		      NULL, V90_ANALOG_MODEM, G711_LAW_PCMU,
		      28000, 56000, 19) == V92_PROFILE_INVALID_ARGUMENT &&
	      v92_pcm_profile_init(
		      &configuration, (enum v90_modem_role)-1,
		      G711_LAW_PCMU, 28000, 56000, 19) ==
		      V92_PROFILE_INVALID_ARGUMENT &&
	      v92_pcm_profile_init(
		      &configuration, V90_ANALOG_MODEM,
		      (enum g711_law)-1, 28000, 56000, 19) ==
		      V92_PROFILE_INVALID_ARGUMENT &&
	      v92_pcm_profile_init(
		      &configuration, V90_ANALOG_MODEM, G711_LAW_PCMU,
		      56000, 28000, 19) == V92_PROFILE_INVALID_ARGUMENT &&
	      v92_pcm_profile_init(
		      &configuration, V90_ANALOG_MODEM, G711_LAW_PCMU,
		      28000, 56000, 0) == V92_PROFILE_INVALID_ARGUMENT &&
	      v92_pcm_profile_init(
		      &configuration, V90_ANALOG_MODEM, G711_LAW_PCMU,
		      28000, 56000, 20) == V92_PROFILE_INVALID_ARGUMENT &&
	      v92_pcm_profile_init(
		      &configuration, V90_ANALOG_MODEM, G711_LAW_PCMU,
		      1000, 2000, 19) == V92_PROFILE_UNSUPPORTED_RATE &&
	      memcmp(&configuration, &saved, sizeof(configuration)) == 0,
	      "rejected V.92 profile changed its destination");
	return true;
}

static bool test_result_names(void)
{
	static const char *const expected[] = {
		"ok",
		"invalid argument",
		"unsupported rate",
	};

	for (size_t index = 0; index < sizeof(expected) / sizeof(expected[0]);
	     ++index)
		CHECK(strcmp(
			      v92_profile_result_name(
				      (enum v92_profile_result)index),
			      expected[index]) == 0,
		      "result name %zu did not match", index);
	CHECK(strcmp(
		      v92_profile_result_name((enum v92_profile_result)-1),
		      "unknown") == 0,
	      "unknown result name did not match");
	return true;
}

int main(void)
{
	if (!test_generated_profiles() || !test_transactionality() ||
	    !test_result_names())
		return 1;
	return 0;
}
