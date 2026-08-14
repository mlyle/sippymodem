// SPDX-License-Identifier: BSD-3-Clause

#include "v90_profile.h"

#include <stddef.h>
#include <stdint.h>

enum {
	V90_PHASE2_NOMINAL_AMPLITUDE = 12000,
	V90_PHASE3_NOMINAL_AMPLITUDE = 6000,
	V90_UPSTREAM_RATE_MASK = 0x1555,
};

static bool role_is_valid(enum v90_modem_role role)
{
	return role == V90_ANALOG_MODEM || role == V90_DIGITAL_MODEM;
}

static struct v90_info0 analog_info0(void)
{
	return (struct v90_info0){
		.symbol_rate_2743_supported = true,
		.symbol_rate_3429_supported = true,
		.low_carrier_3000_supported = true,
		.high_carrier_3000_supported = true,
		.symbol_rate_3429_transmit_permitted = true,
		.transmit_power_reduction_supported = true,
		.maximum_symbol_rate_difference_steps = 2,
		.constellation_1664_supported = true,
	};
}

static struct v90_info0d digital_info0(enum g711_law law)
{
	return (struct v90_info0d){
		.common = {
			.symbol_rate_2800_supported = true,
			.symbol_rate_3429_supported = true,
			.low_carrier_3000_supported = true,
			.high_carrier_3000_supported = true,
			.symbol_rate_3429_transmit_permitted = true,
			.maximum_symbol_rate_difference_steps = 4,
			.cme_modem = true,
			.constellation_1664_supported = true,
		},
		.nominal_transmit_power = 9,
		.maximum_transmit_power = 18,
		.measure_power_at_codec_output = true,
		.pcm_law = law == G711_LAW_PCMA ?
				   V90_PCM_A_LAW : V90_PCM_MU_LAW,
		.upstream_symbol_rate_3429_supported = true,
	};
}

static struct v34_info1c digital_info1(void)
{
	struct v34_info1c info1 = {
		.common = {
			.minimum_peer_power_reduction_db = 2,
			.additional_peer_power_reduction_db = 1,
			.local_md_length_35_ms_units = 7,
			.frequency_offset_1_50_hz_units =
				V34_INFO1_FREQUENCY_OFFSET_UNAVAILABLE,
		},
	};

	for (size_t rate = 0; rate < V34_SYMBOL_RATE_COUNT; ++rate) {
		info1.answer_transmit[rate] =
			(struct v34_info1_probe_result){
				.high_carrier = (rate & 1u) != 0,
				.preemphasis_filter_index = (uint8_t)(rate + 2u),
				.projected_data_rate_2400_bps_units =
					(uint8_t)(rate + 6u),
			};
	}
	info1.answer_transmit[V34_SYMBOL_RATE_3000].high_carrier = true;
	return info1;
}

static struct v90_info1a analog_info1(void)
{
	return (struct v90_info1a){
		.local_md_length_35_ms_units = 5,
		.training_ucode = 91,
		.upstream_symbol_rate = V34_SYMBOL_RATE_3000,
		.frequency_offset_1_50_hz_units =
			V34_INFO1_FREQUENCY_OFFSET_UNAVAILABLE,
	};
}

static struct v90_jd_parameters jd_parameters(void)
{
	return (struct v90_jd_parameters){
		.data_rate_capability_mask = V90_JD_RATE_CAPABILITY_MASK,
		.training_constellation = V34_CONSTELLATION_16_POINT,
		.rate_renegotiation_constellation = V34_CONSTELLATION_4_POINT,
		.maximum_spectral_shaping_lookahead = 2,
	};
}

static void add_ucode(
	struct pcm_constellation *constellation, uint8_t ucode)
{
	constellation->ucode_mask[ucode / 8u] |=
		(uint8_t)(1u << (ucode % 8u));
}

static struct v90_cp_parameters cp_parameters(
	enum g711_law law, enum v90_cp_kind kind)
{
	struct v90_cp_parameters parameters = {
		.kind = kind,
		.law = law,
		.upstream_rate_mask = V90_UPSTREAM_RATE_MASK,
		.trn1d_ratio_q3_13 = 0x2000u,
		.shaping_a1_q1_6 = -16,
		.shaping_a2_q1_6 = 8,
		.shaping_b1_q1_6 = -1,
		.shaping_b2_q1_6 = 31,
		.selected_rate_index = kind == V90_CP_TRAINING ? 13 : 1,
		.sign_redundancy = 1,
		.spectral_lookahead = 2,
		.constellation_count = 1,
	};

	for (uint8_t index = 0; index < 8; ++index)
		add_ucode(
			&parameters.transmit_constellations[0],
			(uint8_t)(24u + 12u * index));
	return parameters;
}

static struct v90_mp_record initial_mp(void)
{
	return (struct v90_mp_record){
		.type = V90_MP_RECORD_TYPE0,
		.parameters.type0 = {
			.maximum_upstream_rate_2400_bps_units = 12,
			.upstream_trellis = V34_TRELLIS_32_STATE,
			.upstream_nonlinear_encoding = true,
			.upstream_expanded_shaping = true,
			.upstream_rate_capability_mask =
				V90_UPSTREAM_RATE_MASK,
		},
	};
}

enum v90_profile_result v90_profile_init(
	struct v90_call_configuration *configuration,
	enum v90_modem_role role, enum g711_law law,
	unsigned int minimum_rate, unsigned int maximum_rate)
{
	struct v90_call_configuration selected = {
		.local_role = role,
		.law = law,
		.phase2_nominal_amplitude = V90_PHASE2_NOMINAL_AMPLITUDE,
	};

	if (configuration == NULL || !role_is_valid(role) ||
	    !g711_law_is_valid(law) || minimum_rate > maximum_rate)
		return V90_PROFILE_INVALID_ARGUMENT;
	if (minimum_rate > V90_PROFILE_DOWNSTREAM_BIT_RATE ||
	    maximum_rate < V90_PROFILE_DOWNSTREAM_BIT_RATE)
		return V90_PROFILE_UNSUPPORTED_RATE;
	if (role == V90_ANALOG_MODEM) {
		selected.local_info0.analog = analog_info0();
		selected.local_info1.analog = analog_info1();
		selected.phase3.analog.nominal_peak_amplitude =
			V90_PHASE3_NOMINAL_AMPLITUDE;
		selected.analog_phase4.training_parameters =
			cp_parameters(law, V90_CP_TRAINING);
		selected.analog_phase4.data_parameters =
			cp_parameters(law, V90_CP_DATA);
	} else {
		selected.local_info0.digital = digital_info0(law);
		selected.local_info1.digital = digital_info1();
		selected.phase3.digital.receive =
			(struct v90_phase3_analog_rx_configuration){
				.s_gain_adaptation = 0.15,
				.pp_equalizer_adaptation = 0.2,
				.tracking_equalizer_adaptation = 0.05,
			};
		selected.phase3.digital.jd = jd_parameters();
		selected.digital_phase4.initial_mp = initial_mp();
	}
	*configuration = selected;
	return V90_PROFILE_OK;
}

const char *v90_profile_result_name(enum v90_profile_result result)
{
	switch (result) {
	case V90_PROFILE_OK:
		return "ok";
	case V90_PROFILE_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_PROFILE_UNSUPPORTED_RATE:
		return "unsupported rate";
	}
	return "unknown";
}
