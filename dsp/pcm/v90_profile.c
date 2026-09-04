// SPDX-License-Identifier: BSD-3-Clause

#include "v90_profile.h"

#include <stddef.h>
#include <stdint.h>

enum {
	/* Keep both roles at the calibrated PCM line level.  Tone A combines its
	 * carrier and guard at their specified relative levels; giving it a QAM
	 * peak level can overdrive a peer whose data-mode gain survives retrain. */
	V90_PHASE2_NOMINAL_AMPLITUDE = 5320,
	/* V.90 Table 7 encodes nominal power as -6 dBm0 minus this value. */
	V90_PHASE2_DIGITAL_NOMINAL_POWER_CODE = 4,
	V90_PHASE2_DIGITAL_MAXIMUM_POWER_CODE = 23,
	V90_PHASE3_NOMINAL_AMPLITUDE = 6000,
	V90_DEFAULT_UPSTREAM_RATE_MASK = 0x1555,
	V90_DEFAULT_UPSTREAM_MAXIMUM_RATE_UNITS = 12,
	V90_SIGN_REDUNDANCY = 1,
	/* Depths zero and one are mandatory at the digital modem; deeper
	 * spectral shaping is optional.  Use the deepest mandatory profile in
	 * both directions so Jd and the subsequent CP mapping agree. */
	V90_SPECTRAL_LOOKAHEAD = 1,
	V90_CONSTELLATION_FIRST_UCODE = 24,
	V90_CONSTELLATION_LAST_UCODE = 108,
	V90_INITIAL_CONSTELLATION_SIZE = 8,
};

struct v90_upstream_selection {
	uint16_t rate_mask;
	uint8_t maximum_rate_2400_bps_units;
	enum v34_symbol_rate symbol_rate;
	bool custom;
	bool suppress_optional_info1d_3429;
};

static struct v90_info0 analog_info0(void)
{
	return (struct v90_info0){
		.symbol_rate_2743_supported = true,
		.symbol_rate_3429_supported = true,
		.low_carrier_3000_supported = true,
		.high_carrier_3000_supported = true,
		.low_carrier_3200_supported = true,
		.high_carrier_3200_supported = true,
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
			.symbol_rate_2743_supported = true,
			.symbol_rate_2800_supported = true,
			.symbol_rate_3429_supported = true,
			.low_carrier_3000_supported = true,
			.high_carrier_3000_supported = true,
			.low_carrier_3200_supported = true,
			.high_carrier_3200_supported = true,
			.symbol_rate_3429_transmit_permitted = true,
			.transmit_power_reduction_supported = true,
			.constellation_1664_supported = true,
		},
		.nominal_transmit_power =
			V90_PHASE2_DIGITAL_NOMINAL_POWER_CODE,
		.maximum_transmit_power =
			V90_PHASE2_DIGITAL_MAXIMUM_POWER_CODE,
		.measure_power_at_codec_output = true,
		.pcm_law = law == G711_LAW_PCMA ?
				   V90_PCM_A_LAW : V90_PCM_MU_LAW,
		.upstream_symbol_rate_3429_supported = true,
	};
}

static struct v34_info1c digital_info1(
	const struct v90_upstream_selection *upstream)
{
	static const uint8_t projected_rates[V34_SYMBOL_RATE_COUNT] = {
		[V34_SYMBOL_RATE_2400] = 9,
		[V34_SYMBOL_RATE_2800] = 11,
		[V34_SYMBOL_RATE_3000] = 12,
		[V34_SYMBOL_RATE_3200] = 13,
		[V34_SYMBOL_RATE_3429] = 14,
	};
	struct v34_info1c info1 = { 0 };

	/* INFO1d advertises channel potential. The configured upstream bit-rate
	 * limit is applied later when MP selects the data-mode parameters. */
	for (size_t rate = 0; rate < V34_SYMBOL_RATE_COUNT; ++rate) {
		uint8_t projected = projected_rates[rate];

		/* V.90 5.2 requires the digital modem to support both 3000 and
		 * 3200 symbols/s.  The configured symbol-rate ceiling selects the
		 * analogue modem's INFO1a preference; it must not withdraw the
		 * mandatory 3200 fallback from digital INFO1d. */
		if (rate == V34_SYMBOL_RATE_3429 &&
		    (upstream->suppress_optional_info1d_3429 ||
		     (upstream->custom && rate > (size_t)upstream->symbol_rate)))
			projected = 0;
		info1.answer_transmit[rate] =
			(struct v34_info1_probe_result){
				.high_carrier =
					rate == V34_SYMBOL_RATE_2800 ||
					rate == V34_SYMBOL_RATE_3000 ||
					rate == V34_SYMBOL_RATE_3200,
				.preemphasis_filter_index = projected != 0 ? 6 : 0,
				.projected_data_rate_2400_bps_units = projected,
			};
	}
	return info1;
}

static struct v90_info1a analog_info1(
	const struct v90_upstream_selection *upstream)
{
	return (struct v90_info1a){
		.local_md_length_35_ms_units = 0,
		.training_ucode = 91,
		.upstream_symbol_rate = upstream->symbol_rate,
		.frequency_offset_1_50_hz_units =
			V34_INFO1_FREQUENCY_OFFSET_UNAVAILABLE,
	};
}

static bool upstream_symbol_rate(
	unsigned int symbols_per_second, enum v34_symbol_rate *rate)
{
	switch (symbols_per_second) {
	case 3000:
		*rate = V34_SYMBOL_RATE_3000;
		return true;
	case 3200:
		*rate = V34_SYMBOL_RATE_3200;
		return true;
	case 3429:
		*rate = V34_SYMBOL_RATE_3429;
		return true;
	default:
		return false;
	}
}

static enum v90_profile_result select_upstream(
	const struct v90_upstream_policy *policy,
	struct v90_upstream_selection *selection)
{
	struct v90_upstream_selection selected = {
		.rate_mask = V90_DEFAULT_UPSTREAM_RATE_MASK,
		.maximum_rate_2400_bps_units =
			V90_DEFAULT_UPSTREAM_MAXIMUM_RATE_UNITS,
		.symbol_rate = V34_SYMBOL_RATE_3000,
	};
	unsigned int minimum;
	unsigned int maximum;
	unsigned int symbol_rate;
	unsigned int maximum_units;

	if (selection == NULL)
		return V90_PROFILE_INVALID_ARGUMENT;
	if (policy == NULL) {
		*selection = selected;
		return V90_PROFILE_OK;
	}
	selected.suppress_optional_info1d_3429 =
		policy->suppress_optional_info1d_3429;
	if (policy->minimum_bit_rate == 0 &&
	     policy->maximum_bit_rate == 0 &&
	     policy->maximum_symbol_rate == 0) {
		*selection = selected;
		return V90_PROFILE_OK;
	}
	minimum = policy->minimum_bit_rate == 0 ?
			  4800u : policy->minimum_bit_rate;
	maximum = policy->maximum_bit_rate == 0 ?
			  33600u : policy->maximum_bit_rate;
	if (minimum > maximum || minimum < 4800u || maximum > 33600u)
		return V90_PROFILE_UNSUPPORTED_RATE;
	symbol_rate = policy->maximum_symbol_rate;
	if (symbol_rate == 0)
		symbol_rate = maximum <= 28800u ? 3000u :
			      maximum <= 31200u ? 3200u : 3429u;
	if (!upstream_symbol_rate(symbol_rate, &selected.symbol_rate))
		return V90_PROFILE_UNSUPPORTED_RATE;
	maximum_units = selected.symbol_rate == V34_SYMBOL_RATE_3000 ? 12u :
			selected.symbol_rate == V34_SYMBOL_RATE_3200 ? 13u : 14u;
	selected.rate_mask = 0;
	selected.maximum_rate_2400_bps_units = 0;
	selected.custom = true;
	for (unsigned int units = V90_MP_MINIMUM_RATE_2400_BPS_UNITS;
	     units <= maximum_units; ++units) {
		unsigned int bit_rate = units * 2400u;

		if (bit_rate < minimum || bit_rate > maximum)
			continue;
		selected.rate_mask |= (uint16_t)(
			1u << (units - V90_MP_MINIMUM_RATE_2400_BPS_UNITS));
		selected.maximum_rate_2400_bps_units = (uint8_t)units;
	}
	if (selected.rate_mask == 0)
		return V90_PROFILE_UNSUPPORTED_RATE;
	*selection = selected;
	return V90_PROFILE_OK;
}

static struct v90_jd_parameters jd_parameters(uint32_t rate_mask)
{
	return (struct v90_jd_parameters){
		.data_rate_capability_mask = rate_mask,
		/* Four-point CP signaling is slower but leaves substantially more
		 * acquisition margin and does not constrain the selected data rate. */
		.training_constellation = V34_CONSTELLATION_4_POINT,
		.rate_renegotiation_constellation = V34_CONSTELLATION_4_POINT,
		.maximum_spectral_shaping_lookahead = V90_SPECTRAL_LOOKAHEAD,
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
		(uint8_t)(PCM_DOWNSTREAM_BASE_FRAME_BITS + rate_index -
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
	uint8_t data_rate_index, uint16_t upstream_rate_mask)
{
	struct v90_cp_parameters parameters = {
		.kind = kind,
		.law = law,
		.upstream_rate_mask = upstream_rate_mask,
		.trn1d_ratio_q3_13 = 0x2000u,
		.shaping_a1_q1_6 = kind == V90_CP_TRAINING ? 0 : -16,
		.shaping_a2_q1_6 = kind == V90_CP_TRAINING ? 0 : 8,
		.shaping_b1_q1_6 = kind == V90_CP_TRAINING ? 0 : -1,
		.shaping_b2_q1_6 = kind == V90_CP_TRAINING ? 0 : 31,
		.selected_rate_index = kind == V90_CP_TRAINING ?
			13 : data_rate_index,
		/* Keep CPt unshaped so its fixed MP'/Ed boundary has no delayed
		 * spectral frame to carry into the data mapping. */
		.sign_redundancy = kind == V90_CP_TRAINING ?
			0 : V90_SIGN_REDUNDANCY,
		.spectral_lookahead = kind == V90_CP_TRAINING ?
			0 : V90_SPECTRAL_LOOKAHEAD,
		.constellation_count = 1,
	};

	generate_constellation(
		&parameters.transmit_constellations[0],
		kind == V90_CP_TRAINING ? V90_INITIAL_CONSTELLATION_SIZE :
			data_constellation_size(data_rate_index));
	return parameters;
}

static struct v90_mp_record initial_mp(
	const struct v90_upstream_selection *upstream)
{
	return (struct v90_mp_record){
		.type = V90_MP_RECORD_TYPE0,
		.parameters.type0 = {
			.maximum_upstream_rate_2400_bps_units =
				upstream->maximum_rate_2400_bps_units,
			/* Keep the mandatory upstream range on the linear, 16-state
			 * baseline used by ordinary peers.  The denser optional rates
			 * need nonlinear projection to retain decision margin under
			 * returned echo and bounded line offsets. */
			.upstream_trellis = V34_TRELLIS_16_STATE,
			.upstream_nonlinear_encoding =
				upstream->maximum_rate_2400_bps_units >
				V90_MP_MANDATORY_MAXIMUM_RATE_2400_BPS_UNITS,
			.upstream_expanded_shaping = true,
			.upstream_rate_capability_mask = upstream->rate_mask,
		},
	};
}

enum v90_profile_result v90_profile_init(
	struct pcm_call_configuration *configuration,
	enum v90_modem_role role, enum g711_law law,
	unsigned int minimum_rate, unsigned int maximum_rate,
	const struct v90_upstream_policy *upstream_policy)
{
	struct pcm_downstream_rate_selection downstream;
	struct v90_upstream_selection upstream;
	struct pcm_call_configuration selected = {
		.local_role = role,
		.law = law,
		.phase2_nominal_amplitude = V90_PHASE2_NOMINAL_AMPLITUDE,
	};

	if (configuration == NULL || !pcm_modem_role_is_valid(role) ||
	    !g711_law_is_valid(law) || minimum_rate > maximum_rate)
		return V90_PROFILE_INVALID_ARGUMENT;
	if (select_upstream(upstream_policy, &upstream) != V90_PROFILE_OK)
		return V90_PROFILE_UNSUPPORTED_RATE;
	if (!pcm_downstream_rates_select(
		    &downstream, minimum_rate, maximum_rate))
		return V90_PROFILE_UNSUPPORTED_RATE;
	selected.enabled_downstream_rate_mask =
		downstream.enabled_mask;
	if (role == V90_ANALOG_MODEM) {
		selected.local_info0.analog = analog_info0();
		selected.local_info1.analog = analog_info1(&upstream);
		selected.phase3.analog.nominal_peak_amplitude =
			V90_PHASE3_NOMINAL_AMPLITUDE;
		selected.phase3.analog.dil_profile =
			V90_DIL_PROFILE_CONVENTIONAL;
		selected.analog_phase4.training_parameters =
			cp_parameters(
				law, V90_CP_TRAINING,
				downstream.selected_data_rate_number,
				upstream.rate_mask);
		selected.analog_phase4.data_parameters =
			cp_parameters(
				law, V90_CP_DATA,
				downstream.selected_data_rate_number,
				upstream.rate_mask);
	} else {
		selected.local_info0.digital = digital_info0(law);
		selected.local_info1.digital = digital_info1(&upstream);
		selected.phase3.digital.receive =
			(struct v90_phase3_analog_rx_configuration){
				.s_gain_adaptation = 0.15,
				.pp_equalizer_adaptation = 0.2,
				.tracking_equalizer_adaptation = 0.05,
			};
		selected.phase3.digital.jd =
			jd_parameters(downstream.enabled_mask);
		selected.digital_phase4.initial_mp = initial_mp(&upstream);
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
