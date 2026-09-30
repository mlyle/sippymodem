// SPDX-License-Identifier: BSD-3-Clause

#include <v90_profile.h>
#include <v90_info0.h>
#include <v90_jd.h>

#include "test.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
	ANALOG_PHASE2_NOMINAL_AMPLITUDE = 5320,
	DIGITAL_PHASE2_NOMINAL_AMPLITUDE = 5657,
	DIGITAL_PHASE2_NOMINAL_POWER_CODE = 6,
	DIGITAL_PHASE2_MAXIMUM_POWER_CODE = 23,
};

static uint32_t expected_bit_rate(uint8_t rate_index)
{
	return ((uint32_t)(PCM_DOWNSTREAM_BASE_FRAME_BITS + rate_index) *
		8000u + 3u) / 6u;
}

static bool test_downstream_rate_selection(void)
{
	CHECK(!pcm_downstream_rate_mask_is_usable(0) &&
	      pcm_downstream_rate_mask_is_usable(UINT32_C(1)) &&
	      pcm_downstream_rate_mask_is_usable(
		      PCM_DOWNSTREAM_RATE_CAPABILITY_MASK) &&
	      !pcm_downstream_rate_mask_is_usable(
		      UINT32_C(1) << PCM_DOWNSTREAM_MAX_DATA_RATE_NUMBER),
	      "configured downstream rate mask bounds changed");
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
		[G711_LAW_PCMU] = { 0xff, 0x84, 0x37, 0x68 },
		[G711_LAW_PCMA] = { 0xff, 0x84, 0x37, 0x78 },
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

static bool test_advertised_phase2_power(void)
{
	static const char *const names[] = { "B", "INFO", "L2", "L1" };
	static const double offsets_db[] = { 0.0, 0.0, 0.0, 6.0 };
	const unsigned int warmup = 800, count = 16000;

	for (int law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		struct pcm_call_configuration configuration;
		struct v34_phase2_tone_transmitter tone;
		struct v34_phase2_dpsk_transmitter info;
		struct v34_phase2_probe_transmitter probe;
		double energy[ARRAY_LENGTH(names)] = { 0 };
		uint32_t random = UINT32_C(0x90db012);

		CHECK(v90_profile_init(&configuration, V90_DIGITAL_MODEM,
			      law, 28000, 56000, NULL) == V90_PROFILE_OK &&
		      v34_phase2_tone_transmitter_init(&tone,
			      V34_CALLING_STATION,
			      configuration.phase2_nominal_amplitude) ==
			      V34_PHASE2_TONE_OK &&
		      v34_phase2_dpsk_transmitter_init(&info,
			      V34_CALLING_STATION,
			      configuration.phase2_nominal_amplitude) ==
			      V34_PHASE2_DPSK_OK &&
		      v34_phase2_probe_transmitter_init(&probe,
			      configuration.phase2_nominal_amplitude) ==
			      V34_PHASE2_PROBE_OK,
		      "digital Phase 2 power fixture did not initialize");
		for (unsigned int index = 0; index < warmup + count; ++index) {
			int16_t samples[ARRAY_LENGTH(names)];
			bool accepted;

			CHECK(v34_phase2_tone_transmitter_sample(
				      &tone, &samples[0]) == V34_PHASE2_TONE_OK &&
			      v34_phase2_dpsk_transmitter_sample(&info,
				      random >> 31, &samples[1], &accepted) ==
				      V34_PHASE2_DPSK_OK,
			      "digital Phase 2 tone/INFO generation failed");
			if (accepted)
				random = random * UINT32_C(1664525) +
					 UINT32_C(1013904223);
			/* Both probe levels use the same phase, without interleaving
			 * alternate samples of their 160-sample waveform. */
			struct v34_phase2_probe_transmitter l1 = probe;
			CHECK(v34_phase2_probe_transmitter_sample(&probe,
				      V34_PHASE2_PROBE_L2, &samples[2]) ==
				      V34_PHASE2_PROBE_OK &&
			      v34_phase2_probe_transmitter_sample(&l1,
				      V34_PHASE2_PROBE_L1, &samples[3]) ==
				      V34_PHASE2_PROBE_OK,
			      "digital Phase 2 probe generation failed");
			for (size_t signal = 0; signal < ARRAY_LENGTH(names); ++signal) {
				double sample = law == G711_LAW_PCMU ?
					g711_decode_pcmu(g711_encode_pcmu(samples[signal])) :
					g711_decode_pcma(g711_encode_pcma(samples[signal]));

				if (index >= warmup)
					energy[signal] += sample * sample;
			}
		}
		for (size_t signal = 0; signal < ARRAY_LENGTH(names); ++signal) {
			/* V.92 Table 6's -12 dBm0 reference is RMS 1000 in
			 * 14-bit mu-law units, or 500 in 13-bit A-law units:
			 * both become RMS 4000 in our 16-bit linear samples.
			 * V.90 8.2 requires nominal power except L1 (+6 dB).
			 * Allow 0.2 dB for G.711 quantization and finite INFO. */
			double measured = -12.0 + 10.0 * log10(
				energy[signal] / (count * 4000.0 * 4000.0));
			double advertised = -6.0 - configuration.local_info0.digital
				.nominal_transmit_power + offsets_db[signal];

			CHECK(isfinite(measured) && fabs(measured - advertised) < 0.2,
			      "law %d %s measured %.3f dBm0, advertised %.3f dBm0",
			      law, names[signal], measured, advertised);
		}
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
				      configuration.analog_phase4.generate_constellations ==
					      (role == V90_ANALOG_MODEM) &&
				      call.local_role == role && call.law == law &&
				      configuration.phase2_nominal_amplitude ==
					      (role == V90_DIGITAL_MODEM ?
					       DIGITAL_PHASE2_NOMINAL_AMPLITUDE :
					       ANALOG_PHASE2_NOMINAL_AMPLITUDE) &&
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
						      DIGITAL_PHASE2_NOMINAL_AMPLITUDE &&
						      tone->guard_amplitude == 0 :
					      tone->guard_amplitude ==
						      ANALOG_PHASE2_NOMINAL_AMPLITUDE,
				      "role %u law %u rate %u Phase 2 level changed",
				      role_index, law_index, bit_rate);
				if (role == V90_ANALOG_MODEM) {
					struct v90_pcm_mapping mapping;
					uint32_t training_power[2], data_power[2];

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
					      v90_pcm_mapping_power(&mapping, V90_PCM_MAPPING_CODEC,
						      law, &data_power[0], &data_power[1]) ==
						      V90_PCM_MAPPING_OK &&
					      v90_pcm_mapping_init(&mapping,
						      &configuration.analog_phase4
						       .training_parameters) == V90_PCM_MAPPING_OK &&
					      mapping.frame_bits == 24 && mapping.sign_bits == 6 &&
					      mapping.modulus_bits == 18 &&
					      v90_pcm_mapping_power(&mapping, V90_PCM_MAPPING_CODEC,
						      law, &training_power[0], &training_power[1]) ==
						      V90_PCM_MAPPING_OK &&
					      data_power[1] <= pow(10.0, 0.3) * training_power[0],
					      "analogue profile did not generate rate %u",
					      bit_rate);
					uint64_t training_capacity = 1;
					for (unsigned interval = 0; interval < V90_CP_FRAME_INTERVALS;
					     ++interval)
						training_capacity *= mapping.modulus[interval];
					CHECK(training_capacity == (UINT64_C(1) << mapping.modulus_bits),
					      "CPt left unused labels in its training constellations");
					CHECK(configuration.analog_phase4.training_parameters
						      .upstream_rate_mask == 0x1fffu &&
					      configuration.analog_phase4.data_parameters
						      .upstream_rate_mask == 0x1fffu &&
					      configuration.local_info1.analog.upstream_symbol_rate ==
						      V34_SYMBOL_RATE_3429,
					      "default upstream offer does not permit 33.6k");
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
					CHECK(configuration.digital_phase4.initial_mp.parameters.type0
						      .upstream_rate_capability_mask == 0x1fffu &&
					      configuration.digital_phase4.initial_mp.parameters.type0
						      .maximum_upstream_rate_2400_bps_units == 14,
					      "default MP omitted supported rates or changed its maximum");
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

static bool check_constellation_spacing(const struct v90_pcm_mapping *mapping,
	uint32_t limit, uint32_t *distance)
{
	int16_t previous = 0, level;
	uint32_t spacing = UINT32_MAX;
	struct v90_pcm_mapping narrower = *mapping;
	struct pcm_constellation lower_bound = { 0 };

	for (unsigned int code = 0; code < 128; ++code) {
		if (!(mapping->transmit[0].ucode_mask[code / 8u] & (1u << (code % 8u))))
			continue;
		CHECK(code <= 108 && g711_ucode_sample(mapping->law, code, true, &level) &&
		      level > previous, "selected level exceeds rail or aliases zero");
		uint32_t gap = previous == 0 ? 2u * level : (uint32_t)(level - previous);
		if (gap < spacing)
			spacing = gap;
		previous = level;
	}
	*distance = spacing;

	/* Independently bound every ordered level of any more widely spaced
	 * set. Its first positive level must exceed spacing/2; each next one
	 * must be more than spacing above its predecessor. All modulus-label
	 * power weights are nonnegative, so no set can beat this power bound. */
	unsigned int code = 0;
	previous = 0;
	for (unsigned int point = 0; point < mapping->modulus[0]; ++point) {
		for (; code <= 108; ++code) {
			CHECK(g711_ucode_sample(mapping->law, code, true, &level),
			      "spacing bound could not decode level");
			if ((point == 0 && 2u * level > spacing) ||
			    (point != 0 && (uint32_t)(level - previous) > spacing))
				break;
		}
		if (code > 108)
			return true; /* A larger distance cannot fit below the rail. */
		lower_bound.ucode_mask[code / 8u] |= (uint8_t)(1u << (code % 8u));
		previous = level;
		++code;
	}
	for (size_t slot = 0; slot < V90_CP_FRAME_INTERVALS; ++slot) {
		narrower.transmit[slot] = lower_bound;
		narrower.codec[slot] = lower_bound;
	}
	uint32_t floor, ceiling;
	CHECK(v90_pcm_mapping_power(&narrower, V90_PCM_MAPPING_CODEC, mapping->law,
				    &floor, &ceiling) == V90_PCM_MAPPING_OK && ceiling > limit,
	      "distance %u is not maximal at power %u (next bound %u)",
	      spacing, limit, ceiling);
	return true;
}

static bool check_training_constellation(struct v90_cp_parameters cp,
	uint32_t limit)
{
	struct v90_pcm_mapping mapping;
	struct pcm_constellation selected, expected = { 0 };
	bool found = false;

	CHECK(v90_pcm_mapping_init(&mapping, &cp) == V90_PCM_MAPPING_OK &&
	      v90_profile_select_constellation(&mapping, limit, &selected),
	      "training constellation selection failed");
	/* Independently enumerate the previous eight-point CPt family: evenly
	 * spaced companded codes, with the highest feasible upper rail. Data
	 * spacing must not also alter training or its scrambler decisions. */
	for (unsigned int last = 7; last <= 108; ++last) {
		unsigned int first = last > 31 ? 24 : last - 7;
		uint32_t floor, ceiling;

		cp.transmit_constellations[0] = (struct pcm_constellation){ 0 };
		for (unsigned int point = 0; point < 8; ++point) {
			unsigned int code = first + ((last - first) * point + 3u) / 7u;
			cp.transmit_constellations[0].ucode_mask[code / 8u] |=
				(uint8_t)(1u << (code % 8u));
		}
		CHECK(v90_pcm_mapping_init(&mapping, &cp) == V90_PCM_MAPPING_OK &&
		      v90_pcm_mapping_power(&mapping, V90_PCM_MAPPING_CODEC, cp.law,
					    &floor, &ceiling) == V90_PCM_MAPPING_OK,
		      "training constellation power fixture failed");
		if (ceiling <= limit) {
			expected = cp.transmit_constellations[0];
			found = true;
		}
	}
	CHECK(found && memcmp(&selected, &expected, sizeof(selected)) == 0,
	      "data spacing changed training law %u power %u", cp.law, limit);
	return true;
}

static bool test_constellation_power_selection(void)
{
	/* Independent normative Table 15 limits; include the entire advertised
	 * range, not only the default -12 dBm0 peer. Keep capacity for 56k while
	 * the negotiated K ranges through all 22 downstream rates. */
	static const uint32_t rms[] = {
		15124, 14276, 13480, 12724, 12012, 11340, 10708, 10108,
		9544, 9008, 8504, 8028, 7580, 7156, 6756, 6380,
		6020, 5684, 5368, 5068, 4784, 4516, 4264, 4024,
		3800, 3588, 3388, 3196, 3020, 2852, 2692, 2540,
	};

	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		struct pcm_call_configuration profile;
		uint32_t last_distance[V90_CP_RATE_INDEX_MAX + 1];

		for (size_t rate = 0; rate < ARRAY_LENGTH(last_distance); ++rate)
			last_distance[rate] = UINT32_MAX;

		CHECK(v90_profile_init(&profile, V90_ANALOG_MODEM, law, 28000, 56000, NULL) ==
			      V90_PROFILE_OK, "power selection profile failed");
		for (size_t code = 0; code < ARRAY_LENGTH(rms); ++code) {
			CHECK(check_training_constellation(profile.analog_phase4.training_parameters,
				      rms[code] * rms[code]), "training constellation changed");
			for (uint8_t rate = 1; rate <= V90_CP_RATE_INDEX_MAX; ++rate) {
				struct v90_cp_parameters cp = profile.analog_phase4.data_parameters;
				struct v90_pcm_mapping mapping, before;
				struct pcm_constellation selected = { 0 }, saved = selected;
				uint32_t lower, upper, limit = rms[code] * rms[code];

				cp.selected_rate_index = rate;
				CHECK(v90_pcm_mapping_init(&mapping, &cp) == V90_PCM_MAPPING_OK,
				      "rate %u mapping failed", rate);
				before = mapping;
				CHECK(v90_profile_select_constellation(&mapping, limit, &selected) &&
				      memcmp(&mapping, &before, sizeof(mapping)) == 0,
				      "law %u power %zu rate %u selection changed input", law, code, rate);
				cp.transmit_constellations[0] = selected;
				CHECK(v90_pcm_mapping_init(&mapping, &cp) == V90_PCM_MAPPING_OK &&
				      mapping.modulus[0] == before.modulus[0] &&
				      v90_pcm_mapping_power(&mapping, V90_PCM_MAPPING_CODEC, law,
							    &lower, &upper) == V90_PCM_MAPPING_OK && upper <= limit,
				      "law %u power %zu rate %u selection exceeded bound or lost capacity", law, code, rate);
				uint32_t distance;
				CHECK(check_constellation_spacing(&mapping, limit, &distance) &&
				      distance <= last_distance[rate],
				      "law %u power %zu rate %u spacing is not maximal/monotone", law, code, rate);
				last_distance[rate] = distance;
				selected = saved;
				CHECK(!v90_profile_select_constellation(NULL, limit, &selected) &&
				      !v90_profile_select_constellation(&mapping, limit, NULL) &&
				      !v90_profile_select_constellation(&mapping, 0, &selected) &&
				      !v90_profile_select_constellation(&mapping, 1, &selected) &&
				      memcmp(&selected, &saved, sizeof(selected)) == 0,
				      "rejected or impossible selection changed output");
				/* A valid different codec plane is not a direct-law profile. */
				unsigned int first = 0;
				while (!(mapping.codec[0].ucode_mask[first / 8u] & (1u << (first % 8u))))
					++first;
				mapping.codec[0].ucode_mask[first / 8u] &= (uint8_t)~(1u << (first % 8u));
				mapping.codec[0].ucode_mask[15] |= 0x80u;
				mapping.codec_plane_differs = true;
				CHECK(v90_pcm_mapping_is_valid(&mapping) &&
				      !v90_profile_select_constellation(&mapping, limit, &selected) &&
				      memcmp(&selected, &saved, sizeof(selected)) == 0,
				      "selection accepted a non-direct mapping");
				mapping.transmit[0] = mapping.codec[0];
				mapping.codec_plane_differs = false;
				CHECK(v90_pcm_mapping_is_valid(&mapping) &&
				      !v90_profile_select_constellation(&mapping, limit, &selected) &&
				      memcmp(&selected, &saved, sizeof(selected)) == 0,
				      "selection accepted unequal interval constellations");
			}
		}
	}
	return true;
}

static bool test_generated_data_capacity(void)
{
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
		for (unsigned redundancy = 0u; redundancy <= 3u; ++redundancy)
			for (uint8_t rate = 1u; rate <= V90_CP_RATE_INDEX_MAX; ++rate) {
				struct v90_cp_parameters cp = {
					.kind = V90_CP_DATA, .law = law, .selected_rate_index = rate,
					.sign_redundancy = redundancy, .constellation_count = 1u,
				};
				struct v90_pcm_mapping mapping;
				uint32_t floor, ceiling;
				CHECK(v90_profile_generate_data_constellation(law, rate, redundancy, 4024u * 4024u,
					&cp.transmit_constellations[0]) && v90_pcm_mapping_init(&mapping, &cp) == V90_PCM_MAPPING_OK &&
				      v90_pcm_mapping_power(&mapping, V90_PCM_MAPPING_CODEC, law, &floor, &ceiling) == V90_PCM_MAPPING_OK &&
				      ceiling <= 4024u * 4024u, "generated data capacity/power failed");
				uint64_t smaller_capacity = 1u;
				for (unsigned slot = 0u; slot < V90_CP_FRAME_INTERVALS; ++slot)
					smaller_capacity *= mapping.modulus[0] - 1u;
				CHECK(mapping.modulus[0] == 8u || smaller_capacity < (UINT64_C(1) << mapping.modulus_bits),
				      "generated map retained unnecessary capacity");
				struct pcm_constellation saved = cp.transmit_constellations[0];
				CHECK(!v90_profile_generate_data_constellation(law, 0u, redundancy, 100u, &cp.transmit_constellations[0]) &&
				      !v90_profile_generate_data_constellation(law, 23u, redundancy, 100u, &cp.transmit_constellations[0]) &&
				      !v90_profile_generate_data_constellation(law, rate, 4u, 100u, &cp.transmit_constellations[0]) &&
				      !v90_profile_generate_data_constellation(law, rate, redundancy, 0u, &cp.transmit_constellations[0]) &&
				      !v90_profile_generate_data_constellation((enum g711_law)99, rate, redundancy, 100u, &cp.transmit_constellations[0]) &&
				      !v90_profile_generate_data_constellation(law, rate, redundancy, 100u, NULL) &&
				      memcmp(&saved, &cp.transmit_constellations[0], sizeof(saved)) == 0,
				      "rejected data generation changed output");
			}
	return true;
}

static bool test_measured_data_capacity(void)
{
	struct pcm_constellation available[6] = { 0 };

	/* Disjoint interval sets require distinct rows. All eight points are
	 * necessary at the lowest rate, making the selected sets unambiguous. */
	for (unsigned interval = 0; interval < 6u; ++interval)
		for (unsigned code = 8u + interval % 3u; code < 40u; code += 4u)
			available[interval].ucode_mask[code / 8u] |= (uint8_t)(1u << (code % 8u));
	for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		struct v90_cp_parameters cp = {
			.kind = V90_CP_DATA, .law = law, .selected_rate_index = 1u,
		};
		struct v90_pcm_mapping mapping;
		uint32_t lower, upper;

		CHECK(v90_profile_generate_measured_constellations(law, 1u, 0u,
			4024u * 4024u, available, cp.transmit_constellations,
			cp.frame_constellation, &cp.constellation_count) &&
		      cp.constellation_count == 3u &&
		      v90_pcm_mapping_init(&mapping, &cp) == V90_PCM_MAPPING_OK &&
		      v90_pcm_mapping_power(&mapping, V90_PCM_MAPPING_CODEC, law, &lower, &upper) == V90_PCM_MAPPING_OK,
		      "measured interval selection failed");
		for (unsigned interval = 0; interval < 6u; ++interval)
			CHECK(memcmp(&mapping.transmit[interval], &available[interval], sizeof(available[0])) == 0 &&
			      cp.frame_constellation[interval] == interval % 3u,
			      "measured selection lost a level or duplicated a row");
		struct v90_cp_parameters saved = cp;
		CHECK(!v90_profile_generate_measured_constellations(law, V90_CP_RATE_INDEX_MAX,
			0u, 4024u * 4024u, available, cp.transmit_constellations,
			cp.frame_constellation, &cp.constellation_count) &&
		      !v90_profile_generate_measured_constellations(law, 1u, 0u,
			upper - 1u, available, cp.transmit_constellations,
			cp.frame_constellation, &cp.constellation_count) &&
		      memcmp(&cp, &saved, sizeof(cp)) == 0,
		      "insufficient measured capacity or exact power changed output");
		struct pcm_constellation incomplete[6];
		memcpy(incomplete, available, sizeof(incomplete));
		incomplete[5] = (struct pcm_constellation){ 0 };
		CHECK(!v90_profile_generate_measured_constellations(law, 1u, 0u,
			4024u * 4024u, incomplete, cp.transmit_constellations,
			cp.frame_constellation, &cp.constellation_count) &&
		      memcmp(&cp, &saved, sizeof(cp)) == 0,
		      "unusable final interval changed earlier rows");
		CHECK(v90_profile_generate_measured_constellations(law, 1u, 0u,
			upper, available, cp.transmit_constellations,
			cp.frame_constellation, &cp.constellation_count) &&
		      memcmp(&cp, &saved, sizeof(cp)) == 0,
		      "exact whole-frame power boundary rejected");

		struct pcm_call_configuration profile;
		CHECK(v90_profile_init(&profile, V90_ANALOG_MODEM, law, 28000, 56000, NULL) == V90_PROFILE_OK,
		      "measured training profile failed");
		cp = profile.analog_phase4.training_parameters;
		CHECK(v90_pcm_mapping_init(&mapping, &cp) == V90_PCM_MAPPING_OK &&
		      v90_profile_select_measured_constellations(&mapping, upper, available,
			cp.transmit_constellations, cp.frame_constellation, &cp.constellation_count) &&
		      v90_pcm_mapping_init(&mapping, &cp) == V90_PCM_MAPPING_OK &&
		      v90_pcm_mapping_power(&mapping, V90_PCM_MAPPING_CODEC, law, &lower, &upper) == V90_PCM_MAPPING_OK,
		      "measured training selection failed");
		for (unsigned interval = 0; interval < 6u; ++interval)
			CHECK(memcmp(&mapping.transmit[interval], &available[interval], sizeof(available[0])) == 0,
			      "training failed to preserve its interval cardinality");
	}
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
		{ "advertised Phase 2 power", test_advertised_phase2_power },
		{ "generated role and media profiles", test_generated_profiles },
		{ "generated constellation power selection", test_constellation_power_selection },
		{ "generated data capacity", test_generated_data_capacity },
		{ "measured data capacity", test_measured_data_capacity },
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
