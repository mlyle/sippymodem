// SPDX-License-Identifier: BSD-3-Clause

#include "v90_profile.h"
#include "v34_profile.h"
#include "v34_frame_schedule.h"
#include "pcm_power.h"

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
	struct v34_info0 info0 = v34_profile_info0();

	info0.symbol_rate_2800_supported = false;
	info0.maximum_symbol_rate_difference_steps = 2;
	return info0;
}

static struct v90_info0d digital_info0(enum g711_law law)
{
	return (struct v90_info0d){
		.common = v34_profile_info0(),
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
	struct v34_info1c info1 = {
		/* The probe spectrum does not provide a qualified offset estimate.
		 * V.90 Table 9 / V.92 Table 17 require -512, not unmeasured zero. */
		.common.frequency_offset_1_50_hz_units =
			V34_INFO1_FREQUENCY_OFFSET_UNAVAILABLE,
	};

	/* INFO1d advertises channel potential. The configured upstream bit-rate
	 * limit is applied later when MP selects the data-mode parameters. */
	for (size_t rate = 0; rate < V34_SYMBOL_RATE_COUNT; ++rate) {
		/* Use ordinary V.34 frame capacity, retaining the PCM protocol's
		 * exclusion of 2743 symbols/s from its conventional upstream. */
		uint8_t projected = rate == V34_SYMBOL_RATE_2743 ? 0 :
			v34_frame_geometry_maximum_rate((enum v34_symbol_rate)rate);

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
	if (!v34_symbol_rate_from_number(symbol_rate, &selected.symbol_rate) ||
	    !v90_upstream_symbol_rate_is_valid(selected.symbol_rate))
		return V90_PROFILE_UNSUPPORTED_RATE;
	maximum_units = v34_frame_geometry_maximum_rate(selected.symbol_rate);
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

		pcm_constellation_add(constellation, ucode);
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

static bool select_constellations(const struct v90_pcm_mapping *mapping,
	uint32_t maximum_power, const struct pcm_constellation *available,
	bool balance, struct pcm_constellation selected[V90_CP_FRAME_INTERVALS]);

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
	return select_constellations(mapping, maximum_power, available, false, selected) &&
		pcm_constellations_pack(selected, rows, map, count);
}

bool v90_profile_select_training_constellation(
	const struct v90_pcm_mapping *mapping, uint32_t maximum_power,
	struct pcm_constellation *constellation)
{
	struct v90_pcm_mapping candidate;
	uint8_t size;

	if (constellation == NULL || !v90_pcm_mapping_is_valid(mapping) ||
	    mapping->kind != V90_CP_TRAINING || mapping->codec_plane_differs || maximum_power == 0)
		return false;
	for (size_t slot = 1; slot < V90_CP_FRAME_INTERVALS; ++slot)
		if (memcmp(&mapping->transmit[0], &mapping->transmit[slot],
			   sizeof(mapping->transmit[0])) != 0)
			return false;

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

/* Prefix sums follow ascending linear levels; mapper labels descend.
 * At a fixed distance these are the componentwise lowest legal levels,
 * minimizing every nonnegative word-occurrence power weight. */
struct spaced_levels {
	uint64_t square[PCM_CONSTELLATION_UCODES + 1];
	uint8_t code[PCM_CONSTELLATION_UCODES];
	unsigned size;
};

static void prepare_levels(struct spaced_levels *levels, enum g711_law law,
	uint32_t distance, const struct pcm_constellation *available)
{
	uint32_t threshold = (distance + 1u) / 2u;
	levels->size = 0;
	levels->square[0] = 0;
	for (unsigned code = pcm_constellation_next(available, 0);
	     code <= V90_CONSTELLATION_LAST_UCODE;
	     code = pcm_constellation_next(available, code + 1u)) {
		int16_t value;
		value = g711_decode(law, g711_ucode_positive_octet(law, (uint8_t)code));
		if ((uint32_t)value < threshold)
			continue;
		levels->code[levels->size] = (uint8_t)code;
		levels->square[levels->size + 1] = levels->square[levels->size] +
			(uint32_t)((int32_t)value * value);
		levels->size++;
		threshold = (uint32_t)value + distance;
	}
}

static uint32_t constellation_power(const struct spaced_levels *const levels[6],
	const uint8_t sizes[6], unsigned bits)
{
	uint64_t remaining = (UINT64_C(1) << bits) - 1, stride = 1;
	struct pcm_power_sum sum = { 0 };
	uint32_t floor, ceiling;
	for (unsigned slot = 0; slot < V90_CP_FRAME_INTERVALS; slot++) {
		const struct spaced_levels *row = levels[slot];
		unsigned size = sizes[slot];
		pcm_power_accumulate_levels(&sum, bits, remaining, stride, size, row->square);
		stride *= size;
		remaining /= size;
	}
	pcm_power_bounds(&sum, bits, &floor, &ceiling);
	return ceiling;
}

struct cardinality_family {
	uint8_t low[V90_CP_FRAME_INTERVALS];
	uint8_t high[V90_CP_FRAME_INTERVALS];
	uint8_t choices;
	uint8_t minimum_high;
};

/* Capacity at these phase caps; six factors stay below 109^6. */
static uint64_t capped_capacity(const unsigned caps[V90_CP_FRAME_INTERVALS],
	unsigned weak, unsigned points, unsigned water)
{
	uint64_t capacity = 1;
	for (unsigned slot = 0; slot < V90_CP_FRAME_INTERVALS; ++slot) {
		unsigned size = weak & (1u << slot) ? points : water;
		capacity *= caps[slot] < size ? caps[slot] : size;
	}
	return capacity;
}

/* Fix every family's cardinalities before distance bisection. A phase with
 * fewer than the root's levels can trade capacity with less impaired phases.
 * Sweep its cap, then find the smallest common ceiling for the other phases.
 * Include the original root-1/root family first to preserve its tie order. */
static unsigned data_families(const struct v90_pcm_mapping *mapping,
	const struct pcm_constellation *available,
	const unsigned source[V90_CP_FRAME_INTERVALS],
	struct cardinality_family families[PCM_CONSTELLATION_UCODES])
{
	unsigned root = mapping->modulus[0], caps[V90_CP_FRAME_INTERVALS];
	unsigned weak = 0, largest = 0, count = 1;
	uint64_t words = UINT64_C(1) << mapping->modulus_bits, capacity = 1;

	families[0].choices = (1u << V90_CP_FRAME_INTERVALS) - 1u;
	for (unsigned slot = 0; slot < V90_CP_FRAME_INTERVALS; slot++) {
		families[0].low[slot] = (uint8_t)(root - 1u);
		families[0].high[slot] = (uint8_t)root;
	}
	if (available == NULL)
		return count;
	for (unsigned slot = 0; slot < V90_CP_FRAME_INTERVALS; slot++) {
		if (source[slot] != slot) {
			caps[slot] = caps[source[slot]];
		} else {
			struct spaced_levels levels;
			prepare_levels(&levels, mapping->law, 1, available + slot);
			caps[slot] = levels.size;
		}
		if (caps[slot] < V90_INITIAL_CONSTELLATION_SIZE)
			return 0;
		capacity *= caps[slot];
		if (caps[slot] < root) {
			weak |= 1u << slot;
			if (caps[slot] > largest)
				largest = caps[slot];
		}
	}
	if (capacity < words)
		return 0;
	if (weak == 0)
		return count;
	/* Increasing weak-phase caps cannot increase the least feasible ceiling
	 * for the other phases. Keep that ceiling across the ascending sweep;
	 * before any family fits, the full rail remains the upper bound. */
	unsigned water = V90_CONSTELLATION_LAST_UCODE + 1u;
	/* There are at most 102 added caps through the existing 109-code rail. */
	for (unsigned points = V90_INITIAL_CONSTELLATION_SIZE; points <= largest; points++) {
		if (capped_capacity(caps, weak, points, water) < words)
			continue;
		while (water > V90_INITIAL_CONSTELLATION_SIZE &&
		       capped_capacity(caps, weak, points, water - 1u) >= words)
			--water;
		struct cardinality_family *family = &families[count++];
		family->choices = 0;
		for (unsigned slot = 0; slot < V90_CP_FRAME_INTERVALS; slot++) {
			unsigned high = weak & (1u << slot) ? points : water;
			unsigned low = weak & (1u << slot) ? points :
				(water > V90_INITIAL_CONSTELLATION_SIZE ? water - 1u : water);
			family->low[slot] = (uint8_t)(caps[slot] < low ? caps[slot] : low);
			family->high[slot] = (uint8_t)(caps[slot] < high ? caps[slot] : high);
			if (family->low[slot] != family->high[slot])
				family->choices |= 1u << slot;
		}
	}
	return count;
}

/* All variable phases in a family share one low/high pair. Capacity
 * therefore depends only on the number of high choices, not their order.
 * Replace one remaining low factor at a time; each division is exact. */
static void prepare_capacity_counts(struct cardinality_family *families,
	unsigned count, uint64_t words)
{
	for (unsigned family = 0; family < count; ++family) {
		struct cardinality_family *f = &families[family];
		uint64_t capacity = 1;
		unsigned low = 1, high = 1, required = 0;
		unsigned choices = bit_count_u8((uint8_t)f->choices);
		for (unsigned slot = 0; slot < V90_CP_FRAME_INTERVALS; ++slot) {
			capacity *= f->low[slot];
			if (f->choices & (1u << slot)) {
				low = f->low[slot];
				high = f->high[slot];
			}
		}
		while (capacity < words && required < choices) {
			capacity = capacity / low * high;
			++required;
		}
		f->minimum_high = capacity >= words ? required : choices + 1u;
	}
}

/* A fixed size vector's feasibility is monotone in distance. A union of
 * fixed families therefore permits bisection, sharing one greedy level
 * prefix per phase and distance. Preserve the original moduli at equal distance;
 * otherwise prefer the lowest exact power ceiling, then family/pattern order.
 * This is a bounded family, not optimization over arbitrary size vectors. */
static bool select_constellations(const struct v90_pcm_mapping *mapping,
	uint32_t maximum_power, const struct pcm_constellation *available,
	bool balance, struct pcm_constellation selected[V90_CP_FRAME_INTERVALS])
{
	unsigned root = mapping->modulus[0], largest = root;
	uint64_t capacity = 1, words = UINT64_C(1) << mapping->modulus_bits;
	struct cardinality_family families[PCM_CONSTELLATION_UCODES];
	unsigned count = 0;
	bool found = false;
	int16_t peak;
	unsigned source[V90_CP_FRAME_INTERVALS];
	struct spaced_levels storage[V90_CP_FRAME_INTERVALS];
	const struct spaced_levels *levels[V90_CP_FRAME_INTERVALS];

	pcm_constellation_sources(available, source);
	for (unsigned slot = 0; slot < V90_CP_FRAME_INTERVALS; slot++) {
		levels[slot] = &storage[source[slot]];
		if (mapping->modulus[slot] > largest)
			largest = mapping->modulus[slot];
		capacity *= root;
	}
	/* Only generated common-row maps may trade capacity between phases.
	 * Peer-owned maps retain every interval's independent cardinality. */
	if (balance && root != V90_INITIAL_CONSTELLATION_SIZE) {
		count = data_families(mapping, available, source, families);
		if (count == 1 && capacity == words)
			count = 0;
	}
	if (!g711_ucode_sample(mapping->law, V90_CONSTELLATION_LAST_UCODE, true, &peak))
		return false;
	prepare_capacity_counts(families, count, words);
	uint32_t low = 1, high = 2u * (uint32_t)peak / (2u * largest - 1u);
	while (low <= high) {
		uint32_t spacing = low + (high - low) / 2u, best_power = UINT32_MAX;
		uint8_t best[V90_CP_FRAME_INTERVALS] = { 0 };
		bool fits = true;
		for (unsigned slot = 0; slot < V90_CP_FRAME_INTERVALS; slot++)
			if (source[slot] == slot)
				prepare_levels(&storage[slot], mapping->law, spacing,
					available ? available + slot : NULL);
		/* The original cardinalities win at equal distance. Both searches
		 * use the same componentwise-minimal levels and exact power sum. */
		for (unsigned slot = 0; slot < V90_CP_FRAME_INTERVALS; slot++)
			fits &= mapping->modulus[slot] <= levels[slot]->size;
		fits = fits && constellation_power(levels, mapping->modulus,
			mapping->modulus_bits) <= maximum_power;
		if (fits)
			memcpy(best, mapping->modulus, sizeof(best));
		unsigned family_count = fits ? 0 : count;
		for (unsigned family = 0; family < family_count; family++) {
			const struct cardinality_family *f = &families[family];
			unsigned choices = f->choices;
			bool feasible = true;
			/* At this distance, an unavailable low excludes the family;
			 * an unavailable high merely fixes that phase to its low.
			 * Every remaining high has the same capacity ratio, so the
			 * precomputed minimum count still applies to these choices. */
			for (unsigned slot = 0; slot < V90_CP_FRAME_INTERVALS; slot++) {
				feasible &= f->low[slot] <= levels[slot]->size;
				if (f->high[slot] > levels[slot]->size)
					choices &= ~(1u << slot);
			}
			if (!feasible || bit_count_u8((uint8_t)choices) < f->minimum_high)
				continue;
			unsigned pattern = 0;
			do {
				if (bit_count_u8((uint8_t)pattern) >= f->minimum_high) {
					uint8_t sizes[V90_CP_FRAME_INTERVALS];
					for (unsigned slot = 0; slot < V90_CP_FRAME_INTERVALS; slot++)
						sizes[slot] = pattern & (1u << slot) ? f->high[slot] : f->low[slot];
					uint32_t power = constellation_power(levels, sizes, mapping->modulus_bits);
					if (power <= maximum_power && power < best_power) {
						fits = true;
						best_power = power;
						memcpy(best, sizes, sizeof(best));
					}
				}
				/* Visit feasible branch subsets in the original ascending order. */
				pattern = (pattern - choices) & choices;
			} while (pattern != 0);
			if (family == 0 && fits)
				break;
		}
		if (fits) {
			for (unsigned slot = 0; slot < V90_CP_FRAME_INTERVALS; slot++) {
				selected[slot] = (struct pcm_constellation){ 0 };
				for (unsigned point = 0; point < best[slot]; point++)
					pcm_constellation_add(&selected[slot], levels[slot]->code[point]);
			}
			found = true;
			low = spacing + 1u;
		} else {
			high = spacing - 1u;
		}
	}
	return found;
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
		select_constellations(&mapping, maximum_power, available, true, selected) &&
		pcm_constellations_pack(selected, rows, map, count);
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
