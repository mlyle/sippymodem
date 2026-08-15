// SPDX-License-Identifier: BSD-3-Clause

#include "v90_profile.h"

#include <stddef.h>
#include <stdint.h>

enum {
	V90_PHASE2_NOMINAL_AMPLITUDE = 12000,
	V90_PHASE3_NOMINAL_AMPLITUDE = 6000,
	V90_UPSTREAM_RATE_MASK = 0x1555,
	V90_DATA_RATE_BASE = 20,
	V90_SIGN_REDUNDANCY = 1,
	V90_CONSTELLATION_FIRST_UCODE = 24,
	V90_CONSTELLATION_LAST_UCODE = 108,
	V90_INITIAL_CONSTELLATION_SIZE = 8,
};

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

uint32_t v90_profile_downstream_bit_rate(uint8_t rate_index)
{
	uint32_t frame_bits;

	if (rate_index == 0 ||
	    rate_index > V90_PROFILE_DOWNSTREAM_RATE_COUNT)
		return 0;
	frame_bits = V90_DATA_RATE_BASE + rate_index;
	return (frame_bits * 8000u + V90_CP_FRAME_INTERVALS / 2u) /
	       V90_CP_FRAME_INTERVALS;
}

enum v90_profile_result v90_profile_select_downstream(
	struct v90_downstream_selection *selection,
	unsigned int minimum_rate, unsigned int maximum_rate)
{
	struct v90_downstream_selection selected = { 0 };

	if (selection == NULL || minimum_rate > maximum_rate)
		return V90_PROFILE_INVALID_ARGUMENT;
	for (uint8_t index = 1;
	     index <= V90_PROFILE_DOWNSTREAM_RATE_COUNT; ++index) {
		uint32_t bit_rate = v90_profile_downstream_bit_rate(index);

		if (bit_rate < minimum_rate || bit_rate > maximum_rate)
			continue;
		selected.enabled_rate_mask |= UINT32_C(1) << (index - 1u);
		selected.selected_rate_index = index;
		selected.selected_bit_rate = bit_rate;
	}
	if (selected.enabled_rate_mask == 0)
		return V90_PROFILE_UNSUPPORTED_RATE;
	*selection = selected;
	return V90_PROFILE_OK;
}

static struct v90_jd_parameters jd_parameters(uint32_t rate_mask)
{
	return (struct v90_jd_parameters){
		.data_rate_capability_mask = rate_mask,
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

static uint8_t data_constellation_size(uint8_t rate_index)
{
	uint8_t sign_bits =
		V90_CP_FRAME_INTERVALS - V90_SIGN_REDUNDANCY;
	uint8_t modulus_bits =
		(uint8_t)(V90_DATA_RATE_BASE + rate_index -
			  sign_bits);
	uint64_t required = UINT64_C(1) << modulus_bits;

	for (uint8_t size = V90_INITIAL_CONSTELLATION_SIZE;
	     size < PCM_CONSTELLATION_UCODES; ++size) {
		uint64_t capacity = 1;

		for (size_t interval = 0;
		     interval < V90_CP_FRAME_INTERVALS; ++interval)
			capacity *= size;
		if (capacity >= required)
			return size;
	}
	return PCM_CONSTELLATION_UCODES;
}

static void generate_constellation(
	struct pcm_constellation *constellation, uint8_t size)
{
	unsigned int span = V90_CONSTELLATION_LAST_UCODE -
			    V90_CONSTELLATION_FIRST_UCODE;
	unsigned int divisor = size - 1u;

	for (unsigned int ordinal = 0; ordinal < size; ++ordinal) {
		uint8_t ucode = (uint8_t)(
			V90_CONSTELLATION_FIRST_UCODE +
			(span * ordinal + divisor / 2u) / divisor);

		add_ucode(constellation, ucode);
	}
}

static struct v90_cp_parameters cp_parameters(
	enum g711_law law, enum v90_cp_kind kind,
	uint8_t data_rate_index)
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
		.selected_rate_index = kind == V90_CP_TRAINING ?
			13 : data_rate_index,
		.sign_redundancy = V90_SIGN_REDUNDANCY,
		.spectral_lookahead = 2,
		.constellation_count = 1,
	};

	generate_constellation(
		&parameters.transmit_constellations[0],
		kind == V90_CP_TRAINING ? V90_INITIAL_CONSTELLATION_SIZE :
			data_constellation_size(data_rate_index));
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
	struct pcm_call_configuration *configuration,
	enum v90_modem_role role, enum g711_law law,
	unsigned int minimum_rate, unsigned int maximum_rate)
{
	struct v90_downstream_selection downstream;
	struct pcm_call_configuration selected = {
		.local_role = role,
		.law = law,
		.phase2_nominal_amplitude = V90_PHASE2_NOMINAL_AMPLITUDE,
	};

	if (configuration == NULL || !pcm_modem_role_is_valid(role) ||
	    !g711_law_is_valid(law) || minimum_rate > maximum_rate)
		return V90_PROFILE_INVALID_ARGUMENT;
	if (v90_profile_select_downstream(
		    &downstream, minimum_rate, maximum_rate) != V90_PROFILE_OK)
		return V90_PROFILE_UNSUPPORTED_RATE;
	selected.enabled_downstream_rate_mask =
		downstream.enabled_rate_mask;
	if (role == V90_ANALOG_MODEM) {
		selected.local_info0.analog = analog_info0();
		selected.local_info1.analog = analog_info1();
		selected.phase3.analog.nominal_peak_amplitude =
			V90_PHASE3_NOMINAL_AMPLITUDE;
		selected.analog_phase4.training_parameters =
			cp_parameters(
				law, V90_CP_TRAINING,
				downstream.selected_rate_index);
		selected.analog_phase4.data_parameters =
			cp_parameters(
				law, V90_CP_DATA,
				downstream.selected_rate_index);
	} else {
		selected.local_info0.digital = digital_info0(law);
		selected.local_info1.digital = digital_info1();
		selected.phase3.digital.receive =
			(struct v90_phase3_analog_rx_configuration){
				.s_gain_adaptation = 0.15,
				.pp_equalizer_adaptation = 0.2,
				.tracking_equalizer_adaptation = 0.05,
			};
		selected.phase3.digital.jd =
			jd_parameters(downstream.enabled_rate_mask);
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
