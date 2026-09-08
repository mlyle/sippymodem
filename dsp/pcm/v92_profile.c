// SPDX-License-Identifier: BSD-3-Clause

#include "v92_profile.h"

#include "pcm_rate.h"
#include "v90_profile.h"
#include "v92_pcm_dil.h"
#include "v92_pcm_phase3_trn1u.h"

#include <stdint.h>

enum {
	V92_TRAINING_UCODE = 91,
	V92_SU_MINIMUM_MAGNITUDE = 256,
	V92_CONSTELLATION_FIRST_UCODE = 24,
	V92_CONSTELLATION_LAST_UCODE = 108,
	V92_INITIAL_CONSTELLATION_SIZE = 8,
	V92_CPT_SELECTED_RATE_INDEX = 9,
	V92_SIGN_REDUNDANCY = 1,
};

static const uint8_t v92_cpt_ucodes[] = { 76, 68, 57, 35 };

static void add_ucode(
	struct pcm_constellation *constellation, uint8_t ucode)
{
	constellation->ucode_mask[ucode / 8u] |=
		(uint8_t)(1u << (ucode % 8u));
}

static void generate_constellation(
	struct pcm_constellation *constellation, uint8_t size)
{
	unsigned int span = V92_CONSTELLATION_LAST_UCODE -
			    V92_CONSTELLATION_FIRST_UCODE;
	unsigned int divisor = size - 1u;

	for (unsigned int ordinal = 0; ordinal < size; ++ordinal) {
		uint8_t ucode = (uint8_t)(
			V92_CONSTELLATION_FIRST_UCODE +
			(span * ordinal + divisor / 2u) / divisor);

		add_ucode(constellation, ucode);
	}
}

static uint8_t data_constellation_size(uint8_t rate_number)
{
	uint8_t sign_bits = V92_CPU_FRAME_INTERVALS - V92_SIGN_REDUNDANCY;
	uint8_t modulus_bits = (uint8_t)(
		PCM_DOWNSTREAM_BASE_FRAME_BITS + rate_number - sign_bits);
	uint64_t required = UINT64_C(1) << modulus_bits;

	for (uint8_t size = V92_INITIAL_CONSTELLATION_SIZE;
	     size < PCM_CONSTELLATION_UCODES; ++size) {
		uint64_t capacity = 1;

		for (size_t interval = 0;
		     interval < V92_CPU_FRAME_INTERVALS; ++interval)
			capacity *= size;
		if (capacity >= required)
			return size;
	}
	return PCM_CONSTELLATION_UCODES;
}

static void generate_cpt_constellation(
	struct pcm_constellation *constellation)
{
	/* CPt is a training map, independent of the later CPu data rate. Four
	 * levels in each of six intervals carry exactly twelve modulus bits;
	 * with Sr=1 this selects the compact 22667 bit/s training profile. */
	for (size_t index = 0;
	     index < sizeof(v92_cpt_ucodes) / sizeof(v92_cpt_ucodes[0]);
	     ++index)
		add_ucode(constellation, v92_cpt_ucodes[index]);
}

static struct v92_pcm_info1a analog_info1(void)
{
	return (struct v92_pcm_info1a){
		.filter_sections = V92_PCM_FILTER_P1_Z2,
		.maximum_total_coefficients = 256,
		.maximum_section_coefficients = 128,
		.training_ucode = V92_TRAINING_UCODE,
	};
}

static struct v92_cpt_parameters cpt_parameters(enum g711_law law)
{
	struct v92_cpt_parameters parameters = {
		.law = law,
		.trn1d_ratio_q3_13 = UINT16_C(0x2000),
		.selected_rate_index = V92_CPT_SELECTED_RATE_INDEX,
		.sign_redundancy = V92_SIGN_REDUNDANCY,
		.spectral_lookahead = 2,
		.constellation_count = 1,
	};

	generate_cpt_constellation(&parameters.transmit_constellations[0]);
	return parameters;
}

static struct v92_cpu_parameters cpu_parameters(
	enum g711_law law, uint8_t downstream_rate_number)
{
	struct v92_cpu_parameters parameters = {
		.law = law,
		.trn1d_ratio_q3_13 = UINT16_C(0x2000),
		.data_rate_number = downstream_rate_number,
		.sign_redundancy = V92_SIGN_REDUNDANCY,
		.spectral_lookahead_frames = 2,
		.constellation_count = 1,
	};

	generate_constellation(
		&parameters.transmit_constellations[0],
		data_constellation_size(downstream_rate_number));
	return parameters;
}

static void configure_analog(
	struct pcm_call_configuration *selected,
	const struct pcm_call_configuration *v90,
	const struct pcm_downstream_rate_selection *downstream,
	uint32_t upstream_rate_mask)
{
	struct v92_pcm_info1a info1 = analog_info1();

	selected->local_info0.v92_analog = (struct v92_info0a){
		.v90 = v90->local_info0.analog,
		.v92_capable = true,
	};
	selected->local_info1.pcm_upstream_enabled = upstream_rate_mask != 0u;
	if (!selected->local_info1.pcm_upstream_enabled)
		return;
	selected->local_info1.v92_analog = info1;
	selected->phase3.v92_analog =
		(struct v92_phase3_analog_station_configuration){
			.local_info1a = info1,
			.cpt = cpt_parameters(selected->law),
			.upstream_rate_mask = upstream_rate_mask,
			.initial_trn1u_symbols =
				V92_PROFILE_INITIAL_TRN1U_SYMBOLS,
			.law = selected->law,
			.upstream_lu_ucode = V92_TRAINING_UCODE,
		};
	selected->v92_analog_phase4.suvu = (struct v92_suvu_parameters){
		.prefilter_level_db_q2_2 = V92_SUVU_NO_MEASUREMENT_Q2_2,
		.wait_for_cpu = true,
	};
	selected->v92_analog_phase4.cpu = cpu_parameters(
		selected->law, downstream->selected_data_rate_number);
}

static void configure_digital(
	struct pcm_call_configuration *selected,
	const struct pcm_call_configuration *v90,
	uint8_t maximum_upstream_data_rate_number)
{
	struct v34_info1c v34 = v90->local_info1.digital;

	v34.answer_transmit[V34_SYMBOL_RATE_3429].high_carrier = false;
	selected->local_info0.v92_digital = (struct v92_info0d){
		.v90 = v90->local_info0.digital,
		.v92_capable = true,
	};
	selected->local_info1.v92_digital = (struct v92_info1d){
		.v34 = v34,
		.pcm_upstream_supported = maximum_upstream_data_rate_number != 0u,
	};
	if (!selected->local_info1.v92_digital.pcm_upstream_supported)
		return;
	selected->phase3.v92_digital =
		(struct pcm_call_v92_digital_phase3_configuration){
			.jd = {
				.data_rate_capability_mask =
					selected->enabled_downstream_rate_mask,
				.maximum_spectral_shaping_lookahead = 3,
			},
			.jp = {
				.training_constellation =
					V92_CONSTELLATION_4_POINT,
				.rate_renegotiation_constellation =
					V92_CONSTELLATION_4_POINT,
			},
			.equalizer_adaptation = 0.05,
			.su_minimum_magnitude = V92_SU_MINIMUM_MAGNITUDE,
		};
	selected->v92_digital_phase4.upstream_selection = PCM_CALL_V92_UPSTREAM_DIRECT;
	selected->v92_digital_phase4.direct_maximum_data_rate_number =
		maximum_upstream_data_rate_number;
}

enum v92_profile_result v92_profile_init(
	struct pcm_call_configuration *configuration,
	enum v90_modem_role role, enum g711_law law,
	unsigned int minimum_downstream_rate,
	unsigned int maximum_downstream_rate,
	uint8_t maximum_upstream_data_rate_number,
	const struct v90_upstream_policy *conventional_upstream)
{
	struct pcm_call_configuration v90;
	struct pcm_call_configuration selected = { 0 };
	struct pcm_downstream_rate_selection downstream;
	uint32_t upstream_rate_mask = 0u;

	if (configuration == NULL || !pcm_modem_role_is_valid(role) ||
	    !g711_law_is_valid(law) ||
	    minimum_downstream_rate > maximum_downstream_rate ||
	    maximum_upstream_data_rate_number >
		    V92_PROFILE_MAX_UPSTREAM_DATA_RATE_NUMBER)
		return V92_PROFILE_INVALID_ARGUMENT;
	if (!pcm_downstream_rates_select(
		    &downstream, minimum_downstream_rate,
		    maximum_downstream_rate) ||
	    (maximum_upstream_data_rate_number != 0u &&
	     !v92_pcm_dil_rate_mask_through(
		    maximum_upstream_data_rate_number,
		    &upstream_rate_mask)) ||
	    v90_profile_init(
		    &v90, role, law, minimum_downstream_rate,
		    maximum_downstream_rate, conventional_upstream) != V90_PROFILE_OK)
		return V92_PROFILE_UNSUPPORTED_RATE;
	/* PCM upstream adds a capability to the conventional path. Retain the
	 * latter's policy for INFO1 selection and subsequent full retrains. */
	selected = v90;
	selected.protocol = PCM_PROTOCOL_V92;
	if (role == V90_ANALOG_MODEM)
		configure_analog(
			&selected, &v90, &downstream, upstream_rate_mask);
	else
		configure_digital(
			&selected, &v90,
			maximum_upstream_data_rate_number);
	*configuration = selected;
	return V92_PROFILE_OK;
}

const char *v92_profile_result_name(enum v92_profile_result result)
{
	switch (result) {
	case V92_PROFILE_OK:
		return "ok";
	case V92_PROFILE_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_PROFILE_UNSUPPORTED_RATE:
		return "unsupported rate";
	}
	return "unknown";
}
