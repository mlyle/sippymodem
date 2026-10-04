// SPDX-License-Identifier: BSD-3-Clause

#include "v90_profile.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	/* Preserve the analogue tone A/guard level across retraining. */
	V90_PHASE2_ANALOG_NOMINAL_AMPLITUDE = 5320,
	/* V.90 Table 7: -6 minus this code is -12 dBm0. Derive the digital
	 * waveform amplitude from the same code below. */
	V90_PHASE2_DIGITAL_NOMINAL_POWER_CODE = 6,
	V90_PHASE2_DIGITAL_MAXIMUM_POWER_CODE = 23,
	V90_PHASE3_NOMINAL_AMPLITUDE = 6000,
	/* Depths zero and one are mandatory at the digital modem; deeper
	 * spectral shaping is optional. Advertise the mandatory depth even
	 * though the generated conventional CP profile is unshaped. */
	V90_SPECTRAL_LOOKAHEAD = 1,
	V90_CONSTELLATION_FIRST_UCODE = 24,
	V90_CONSTELLATION_LAST_UCODE = 108,
	V90_INITIAL_CONSTELLATION_SIZE = 8,
	/* Eight levels in each of six intervals carry K=18 modulus bits.
	 * With S=6 unshaped signs, Table 14's CPt rate number is 24-8. */
	V90_TRAINING_RATE_INDEX = 16,
};

struct v90_upstream_selection {
	uint16_t rate_mask;
	uint8_t maximum_rate_2400_bps_units;
	enum v34_symbol_rate symbol_rate;
	bool suppress_optional_info1d_3429;
};

static struct v34_info0 analog_info0(void)
{
	return (struct v34_info0){
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
	struct v34_info1c info1 = {
		/* The probe spectrum does not provide a qualified offset estimate.
		 * V.90 Table 9 / V.92 Table 17 require -512, not unmeasured zero. */
		.common.frequency_offset_1_50_hz_units =
			V34_INFO1_FREQUENCY_OFFSET_UNAVAILABLE,
	};

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
		     rate > (size_t)upstream->symbol_rate))
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
	const struct v90_upstream_policy automatic = { 0 };
	struct v90_upstream_selection selected = { 0 };
	unsigned int minimum;
	unsigned int maximum;
	unsigned int symbol_rate;
	unsigned int maximum_units;

	if (selection == NULL)
		return V90_PROFILE_INVALID_ARGUMENT;
	if (policy == NULL)
		policy = &automatic;
	selected.suppress_optional_info1d_3429 =
		policy->suppress_optional_info1d_3429;
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
	/* V.90 Tables 14/16: capabilities use 2400-bit/s steps. Automatic
	 * selection includes 31.2/33.6k, subject to the peer's INFO1d and MP. */
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

static uint8_t data_constellation_size(uint8_t rate_index, uint8_t sign_redundancy)
{
	uint8_t sign_bits =
		V90_CP_FRAME_INTERVALS - sign_redundancy;
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
	struct pcm_constellation *constellation, uint8_t size, uint8_t last)
{
	unsigned int first = last + 1u - size;
	if (first > V90_CONSTELLATION_FIRST_UCODE)
		first = V90_CONSTELLATION_FIRST_UCODE;
	unsigned int span = last - first;
	unsigned int divisor = size - 1u;

	*constellation = (struct pcm_constellation){ 0 };
	for (unsigned int ordinal = 0; ordinal < size; ++ordinal) {
		uint8_t ucode = (uint8_t)(
			first +
			(span * ordinal + divisor / 2u) / divisor);

		add_ucode(constellation, ucode);
	}
}

bool v90_profile_generate_data_template(uint8_t rate_index,
	uint8_t sign_redundancy, struct pcm_constellation *constellation)
{
	struct pcm_constellation generated = { 0 };

	if (constellation == NULL || rate_index < PCM_DOWNSTREAM_MIN_DATA_RATE_NUMBER ||
	    rate_index > PCM_DOWNSTREAM_MAX_DATA_RATE_NUMBER || sign_redundancy > 3u)
		return false;
	generate_constellation(&generated,
		data_constellation_size(rate_index, sign_redundancy),
		V90_CONSTELLATION_LAST_UCODE);
	*constellation = generated;
	return true;
}

static bool spaced_constellation(struct pcm_constellation *constellation,
	enum g711_law law, uint8_t size, uint32_t spacing,
	const struct pcm_constellation *available)
{
	uint32_t threshold = (spacing + 1u) / 2u;

	*constellation = (struct pcm_constellation){ 0 };
	/* The lowest available level at each step minimizes power for this
	 * distance. Include the innermost positive/negative pair, which evenly
	 * spaced companded codes would leave particularly crowded. */
	for (unsigned int code = 0; code <= V90_CONSTELLATION_LAST_UCODE; ++code) {
		int16_t level;

		if (!g711_ucode_sample(law, (uint8_t)code, true, &level))
			return false;
		if ((available != NULL && !pcm_constellation_contains(available, (uint8_t)code)) ||
		    (uint32_t)level < threshold)
			continue;
		add_ucode(constellation, (uint8_t)code);
		if (--size == 0)
			return true;
		threshold = (uint32_t)level + spacing;
	}
	return false;
}

/* Called only for a validated direct-law mapping with equal cardinalities. */
static bool select_spaced_constellations(const struct v90_pcm_mapping *mapping,
	uint32_t maximum_power, const struct pcm_constellation *available,
	struct pcm_constellation constellations[V90_CP_FRAME_INTERVALS])
{
	struct v90_pcm_mapping candidate = *mapping;
	struct pcm_constellation best[V90_CP_FRAME_INTERVALS] = { 0 };
	uint8_t size = mapping->modulus[0];
	int16_t peak;
	bool found = false;

	if (!g711_ucode_sample(mapping->law, V90_CONSTELLATION_LAST_UCODE, true, &peak))
		return false;
	uint32_t low = 1, high = 2u * (uint32_t)peak / (2u * size - 1u);

	/* At fixed K and cardinality, each ordered level has a nonnegative
	 * exact 8.5.2 power weight. The greedy set minimizes every level for
	 * a given distance, so feasibility is monotone and can be bisected. */
	while (low <= high) {
		uint32_t spacing = low + (high - low) / 2u;
		uint32_t floor, ceiling;

		bool fits = true;
		for (size_t slot = 0; slot < V90_CP_FRAME_INTERVALS; ++slot) {
			fits = spaced_constellation(&candidate.transmit[slot], mapping->law,
				size, spacing, available ? &available[slot] : NULL) && fits;
			candidate.codec[slot] = candidate.transmit[slot];
		}
		if (!fits) {
			high = spacing - 1u;
			continue;
		}
		if (v90_pcm_mapping_power(&candidate, V90_PCM_MAPPING_CODEC,
					 mapping->law, &floor, &ceiling) != V90_PCM_MAPPING_OK)
			return false;
		if (ceiling <= maximum_power) {
			memcpy(best, candidate.transmit, sizeof(best));
			found = true;
			low = spacing + 1u;
		} else
			high = spacing - 1u;
	}
	if (found)
		memcpy(constellations, best, sizeof(best));
	return found;
}

bool v90_profile_select_measured_constellations(
	const struct v90_pcm_mapping *mapping, uint32_t maximum_power,
	const struct pcm_constellation available[V90_CP_FRAME_INTERVALS],
	struct pcm_constellation rows[V90_CP_MAX_CONSTELLATIONS],
	uint8_t map[V90_CP_FRAME_INTERVALS], uint8_t *count)
{
	struct pcm_constellation selected[V90_CP_FRAME_INTERVALS];

	if (rows == NULL || map == NULL || count == NULL ||
	    !v90_pcm_mapping_is_valid(mapping) || mapping->codec_plane_differs ||
	    maximum_power == 0)
		return false;
	for (unsigned interval = 1; interval < V90_CP_FRAME_INTERVALS; ++interval)
		if (mapping->modulus[interval] != mapping->modulus[0])
			return false;
	return select_spaced_constellations(mapping, maximum_power, available, selected) &&
		pcm_constellations_pack(selected, rows, map, count);
}

bool v90_profile_select_constellation(
	const struct v90_pcm_mapping *mapping, uint32_t maximum_power,
	struct pcm_constellation *constellation)
{
	struct v90_pcm_mapping candidate;
	uint8_t size;

	if (constellation == NULL || !v90_pcm_mapping_is_valid(mapping) ||
	    mapping->codec_plane_differs || maximum_power == 0)
		return false;
	for (size_t slot = 1; slot < V90_CP_FRAME_INTERVALS; ++slot)
		if (memcmp(&mapping->transmit[0], &mapping->transmit[slot],
			   sizeof(mapping->transmit[0])) != 0)
			return false;
	if (mapping->kind == V90_CP_DATA) {
		struct pcm_constellation selected[V90_CP_FRAME_INTERVALS];
		if (!select_spaced_constellations(mapping, maximum_power, NULL, selected))
			return false;
		*constellation = selected[0];
		return true;
	}
	size = mapping->modulus[0];
	candidate = *mapping;
	/* Preserve distinct labels, reducing the highest level until the exact
	 * 8.5.2 average fits. A lower K can make the highest labels more likely,
	 * so start afresh on every negotiated rate rather than retain a cap. */
	for (int last = V90_CONSTELLATION_LAST_UCODE; last >= size - 1; --last) {
		uint32_t floor, ceiling;

		generate_constellation(&candidate.transmit[0], size, (uint8_t)last);
		for (size_t slot = 0; slot < V90_CP_FRAME_INTERVALS; ++slot) {
			candidate.transmit[slot] = candidate.transmit[0];
			candidate.codec[slot] = candidate.transmit[0];
		}
		if (v90_pcm_mapping_power(&candidate, V90_PCM_MAPPING_CODEC,
					 mapping->law, &floor, &ceiling) != V90_PCM_MAPPING_OK)
			return false;
		if (ceiling <= maximum_power) {
			*constellation = candidate.transmit[0];
			return true;
		}
	}
	return false;
}

bool v90_profile_generate_measured_constellations(enum g711_law law,
	uint8_t rate_index, uint8_t sign_redundancy, uint32_t maximum_power,
	const struct pcm_constellation available[V90_CP_FRAME_INTERVALS],
	struct pcm_constellation rows[V90_CP_MAX_CONSTELLATIONS],
	uint8_t map[V90_CP_FRAME_INTERVALS], uint8_t *count)
{
	struct v90_cp_parameters parameters = {
		.kind = V90_CP_DATA, .law = law,
		.selected_rate_index = rate_index,
		.sign_redundancy = sign_redundancy, .constellation_count = 1u,
	};
	struct v90_pcm_mapping mapping;
	struct pcm_constellation selected[V90_CP_FRAME_INTERVALS];

	if (rows == NULL || map == NULL || count == NULL || maximum_power == 0 ||
	    !g711_law_is_valid(law) || rate_index == 0u ||
	    rate_index > V90_CP_RATE_INDEX_MAX || sign_redundancy > 3u)
		return false;
	/* Regenerate capacity at the selected K. Retaining a former high-rate
	 * map defeats the noise margin that a lower rate is meant to provide.
	 * Only K and S affect the exact average-power weights, not the shaping
	 * coefficients or lookahead used by the caller's spectral encoder. */
	generate_constellation(&parameters.transmit_constellations[0],
		data_constellation_size(rate_index, sign_redundancy), V90_CONSTELLATION_LAST_UCODE);
	return v90_pcm_mapping_init(&mapping, &parameters) == V90_PCM_MAPPING_OK &&
		select_spaced_constellations(&mapping, maximum_power, available, selected) &&
		pcm_constellations_pack(selected, rows, map, count);
}

bool v90_profile_generate_data_constellation(enum g711_law law,
	uint8_t rate_index, uint8_t sign_redundancy, uint32_t maximum_power,
	struct pcm_constellation *constellation)
{
	struct pcm_constellation rows[V90_CP_MAX_CONSTELLATIONS];
	uint8_t map[V90_CP_FRAME_INTERVALS], count;

	if (constellation == NULL || !v90_profile_generate_measured_constellations(law,
		    rate_index, sign_redundancy, maximum_power, NULL, rows, map, &count))
		return false;
	*constellation = rows[0];
	return true;
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
		.selected_rate_index = kind == V90_CP_TRAINING ?
			V90_TRAINING_RATE_INDEX : data_rate_index,
		/* Keep the conventional training and data sign coding identical.
		 * Some peers apply CP shaping before MP/Ed ends, discarding a
		 * training sign bit when CPt and CP have different redundancy. */
		.constellation_count = 1,
	};

	if (kind == V90_CP_TRAINING)
		generate_constellation(&parameters.transmit_constellations[0],
			V90_INITIAL_CONSTELLATION_SIZE, V90_CONSTELLATION_LAST_UCODE);
	else
		v90_profile_generate_data_template(data_rate_index,
			parameters.sign_redundancy, &parameters.transmit_constellations[0]);
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
			/* Use the interoperable linear, Type-0 baseline at every rate.
			 * Nonlinear projection remains available when an explicit MP
			 * record selects it, but is not needed to reach 33.6 kbit/s. */
			.upstream_trellis = V34_TRELLIS_16_STATE,
			.upstream_nonlinear_encoding = false,
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
		/* V.92 Table 6's -12 dBm0 reference has RMS 4000 in our
		 * 16-bit G.711 scale (1000 in mu-law's 14-bit scale). Convert
		 * that RMS reference to a peak sinusoid at the advertised level.
		 * INFO and L2 preserve this power; L1 is 6 dB higher. */
		.phase2_nominal_amplitude = role == V90_DIGITAL_MODEM ?
			(int16_t)lround(4000.0 * sqrt(2.0) * pow(10.0,
				(6.0 - V90_PHASE2_DIGITAL_NOMINAL_POWER_CODE) / 20.0)) :
			V90_PHASE2_ANALOG_NOMINAL_AMPLITUDE,
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
		selected.analog_phase4.generate_constellations = true;
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
