// SPDX-License-Identifier: BSD-3-Clause

#include "v90_profile.h"
#include "v34_profile.h"
#include "v34_frame_schedule.h"
#include "pcm_power.h"
#include "wide_uint.h"

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

static void prepare_phase_levels(struct spaced_levels storage[V90_CP_FRAME_INTERVALS],
	const unsigned source[V90_CP_FRAME_INTERVALS], enum g711_law law,
	uint32_t distance, const struct pcm_constellation *available)
{
	for (unsigned slot = 0; slot < V90_CP_FRAME_INTERVALS; slot++)
		if (source[slot] == slot)
			prepare_levels(&storage[slot], law, distance,
				available ? available + slot : NULL);
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
 * Sweep its cap, then find the smallest common ceiling for the other phases. */
static unsigned asymmetric_families(unsigned root, uint64_t words,
	const struct spaced_levels *const levels[V90_CP_FRAME_INTERVALS],
	struct cardinality_family *families)
{
	unsigned caps[V90_CP_FRAME_INTERVALS];
	unsigned weak = 0, largest = 0, count = 0;
	uint64_t capacity = 1;

	for (unsigned slot = 0; slot < V90_CP_FRAME_INTERVALS; slot++) {
		caps[slot] = levels[slot]->size;
		if (caps[slot] < V90_INITIAL_CONSTELLATION_SIZE)
			return 0;
		capacity *= caps[slot];
		if (caps[slot] < root) {
			weak |= 1u << slot;
			if (caps[slot] > largest)
				largest = caps[slot];
		}
	}
	if (capacity < words || weak == 0)
		return 0;
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

static struct cardinality_family balanced_family(unsigned root)
{
	struct cardinality_family family = {
		.choices = (1u << V90_CP_FRAME_INTERVALS) - 1u,
	};
	for (unsigned slot = 0; slot < V90_CP_FRAME_INTERVALS; slot++) {
		family.low[slot] = (uint8_t)(root - 1u);
		family.high[slot] = (uint8_t)root;
	}
	return family;
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

static bool choose_cardinalities(const struct v90_pcm_mapping *mapping,
	uint32_t maximum_power, const struct spaced_levels *const levels[V90_CP_FRAME_INTERVALS],
	const struct cardinality_family *families, unsigned count,
	uint8_t best[V90_CP_FRAME_INTERVALS])
{
	bool fits = true;
	for (unsigned slot = 0; slot < V90_CP_FRAME_INTERVALS; slot++)
		fits &= mapping->modulus[slot] <= levels[slot]->size;
	if (fits && constellation_power(levels, mapping->modulus,
			mapping->modulus_bits) <= maximum_power) {
		memcpy(best, mapping->modulus, V90_CP_FRAME_INTERVALS);
		return true;
	}
	fits = false;
	uint32_t best_power = UINT32_MAX;
	for (unsigned family = 0; family < count; family++) {
		const struct cardinality_family *f = &families[family];
		unsigned choices = f->choices;
		bool feasible = true;
		/* At this distance, an unavailable low excludes the family;
		 * an unavailable high merely fixes that phase to its low. */
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
					memcpy(best, sizes, sizeof(sizes));
				}
			}
			/* Visit feasible branch subsets in the original ascending order. */
			pattern = (pattern - choices) & choices;
		} while (pattern != 0);
		if (family == 0 && fits)
			break;
	}
	return fits;
}

enum {
	MARGIN_UNITY = 32768,
	/* Two mirrored supports per positive gap, plus the sign boundary. */
	MARGIN_LINES_PER_PHASE = 2 * PCM_CONSTELLATION_UCODES - 1,
};

struct margin_line {
	int32_t room;
	int32_t approach;
};

static int32_t room_below(int point, int limit)
{
	return (limit - point) * MARGIN_UNITY +
		(limit >= 0 ? MARGIN_UNITY - 1 : 0);
}

static int32_t room_above(int point, int limit)
{
	return (point - limit) * MARGIN_UNITY +
		(limit <= 0 ? MARGIN_UNITY - 1 : 0);
}

/* Each support is the room before truncation at one exact codec boundary,
 * diminished by the movement toward that boundary from the worst preceding
 * symbol. Reflected signed neighbors have the same approach, so only the
 * tighter bound of each pair can affect the envelope. */
static size_t phase_margin_lines(enum g711_law law,
	const struct pcm_constellation *row, int previous_peak,
	struct margin_line lines[MARGIN_LINES_PER_PHASE])
{
	size_t count = pcm_constellation_count(row);
	if (count == 0)
		return 0;
	unsigned code = pcm_constellation_next(row, 0);
	int lower = g711_decode(law, g711_ucode_positive_octet(law, (uint8_t)code));
	if (lower <= 0)
		return 0;

	/* Fill by increasing approach: left edges in reverse Ucode order,
	 * the sign edge, then right edges in forward order. */
	size_t left = count - 1u, right = count;
	lines[left] = (struct margin_line){ room_below(-lower, -1), previous_peak + lower };
	for (code = pcm_constellation_next(row, code + 1u);
	     code < PCM_CONSTELLATION_UCODES;
	     code = pcm_constellation_next(row, code + 1u)) {
		int upper = g711_decode(law, g711_ucode_positive_octet(law, (uint8_t)code));
		/* Ties in the reconstructed magnitude go to the larger label. */
		int midpoint = (lower + upper + 1) / 2;
		uint8_t octet = g711_encode(law, (int16_t)midpoint);
		if (g711_decode(law, octet) < midpoint) {
			uint8_t ucode = octet ^ g711_ucode_positive_octet(law, 0);
			octet = g711_ucode_positive_octet(law, (uint8_t)(ucode + 1u));
		}
		struct g711_interval positive, negative;
		if (!g711_octet_interval(law, octet, &positive) ||
		    !g711_octet_interval(law, octet ^ 0x80u, &negative))
			return 0;
		int32_t left_room = room_below(lower, positive.lower - 1);
		int32_t left_mirror = room_above(-lower, negative.upper + 1);
		int32_t right_room = room_above(upper, positive.lower);
		int32_t right_mirror = room_below(-upper, negative.upper);
		lines[--left] = (struct margin_line){
			left_room < left_mirror ? left_room : left_mirror, previous_peak - lower,
		};
		lines[right++] = (struct margin_line){
			right_room < right_mirror ? right_room : right_mirror, previous_peak + upper,
		};
		lower = upper;
	}
	return right;
}

/* Each knot uses a difference of two nonnegative Q15 rooms and
 * a difference of two sample approaches. Both fit int32_t. */
struct tap_knot {
	int32_t numerator, denominator;
};

struct tap_segment {
	struct margin_line line;
	struct tap_knot start;
};

struct tap_phase {
	struct tap_segment segment[MARGIN_LINES_PER_PHASE];
	size_t count;
	int previous, older;
};

struct tap_plane {
	int64_t x, y, room;
};

struct tap_vertex {
	int64_t x, y, d;
	struct tap_plane edge;
};

/* Four box edges, two strip edges, six safe planes and five comparisons. */
enum { TAP_CELL_VERTICES = 4 + 2 + V90_CP_FRAME_INTERVALS + (V90_CP_FRAME_INTERVALS - 1) };

struct tap_cell {
	struct tap_vertex point[TAP_CELL_VERTICES];
	size_t count;
};

static bool tap_wide_negative(struct wide_uint value)
{
	return value.high >> 63 != 0;
}

static bool tap_knot_less(struct tap_knot a, struct tap_knot b)
{
	return (int64_t)a.numerator * b.denominator < (int64_t)b.numerator * a.denominator;
}

static void tap_add_line(struct tap_phase *phase, struct margin_line line)
{
	struct tap_knot crossing;

	while (phase->count != 0u) {
		const struct tap_segment *last = &phase->segment[phase->count - 1u];
		crossing.denominator = line.approach - last->line.approach;
		crossing.numerator = line.room - last->line.room;
		if (crossing.denominator == 0) {
			if (crossing.numerator >= 0)
				return;
		} else if (phase->count == 1u || tap_knot_less(last->start, crossing)) {
			break;
		}
		--phase->count;
	}
	if (phase->count == 0u)
		crossing = (struct tap_knot){ 0, 1 };
	phase->segment[phase->count++] = (struct tap_segment){ line, crossing };
}

/* With x = q1 + q2 and y = |q2|, all supports in one phase have
 * the same y slope. Their active support changes only at a knot in x. */
static bool tap_phases(enum g711_law law,
	const struct pcm_constellation selected[V90_CP_FRAME_INTERVALS],
	struct tap_phase phase[V90_CP_FRAME_INTERVALS])
{
	int peak[V90_CP_FRAME_INTERVALS];
	struct margin_line lines[MARGIN_LINES_PER_PHASE];
	for (unsigned i = 0; i < V90_CP_FRAME_INTERVALS; ++i) {
		uint8_t code;
		if (!pcm_constellation_descending_select(&selected[i], 0, &code))
			return false;
		peak[i] = g711_decode(law, g711_ucode_positive_octet(law, code));
	}
	for (unsigned i = 0; i < V90_CP_FRAME_INTERVALS; ++i) {
		int previous = peak[(i + V90_CP_FRAME_INTERVALS - 1u) % V90_CP_FRAME_INTERVALS];
		phase[i].previous = previous;
		phase[i].older = peak[(i + V90_CP_FRAME_INTERVALS - 2u) % V90_CP_FRAME_INTERVALS];
		phase[i].count = 0;
		size_t count = phase_margin_lines(law, &selected[i], previous, lines);
		if (count == 0u)
			return false;
		for (size_t j = 0; j < count; ++j)
			tap_add_line(&phase[i], lines[j]);
	}
	return true;
}

static struct tap_plane tap_at(const struct tap_phase *phase, size_t index, int sign)
{
	struct margin_line line = phase->segment[index].line;
	return (struct tap_plane){
		-line.approach, sign * phase->previous - phase->older, line.room,
	};
}

static struct tap_plane tap_subtract(struct tap_plane a, struct tap_plane b)
{
	return (struct tap_plane){ a.x - b.x, a.y - b.y, a.room - b.room };
}

/* Retain each incoming edge's supporting line. Intersections stay as
 * determinants of two original lines, even after repeated clipping.
 * Line slopes are bounded by 2^17 and offsets by 2^31. Coordinates fit
 * int64_t. Evaluating a plane at those coordinates needs wider products. */
static struct tap_vertex tap_intersection(struct tap_plane edge, struct tap_plane cut)
{
	struct tap_vertex p = {
		edge.y * cut.room - edge.room * cut.y,
		edge.room * cut.x - edge.x * cut.room,
		edge.x * cut.y - edge.y * cut.x,
		edge,
	};
	if (p.d < 0) {
		p.d = -p.d;
		p.x = -p.x;
		p.y = -p.y;
	}
	return p;
}

static int tap_side(struct tap_plane p, struct tap_vertex v)
{
	struct wide_uint sum, product;
	sum = wide_product_s64(p.x, v.x);
	product = wide_product_s64(p.y, v.y);
	(void)wide_add(sum, product, &sum);
	product = wide_product_s64(p.room, v.d);
	(void)wide_add(sum, product, &sum);
	if (tap_wide_negative(sum))
		return -1;
	return sum.low != 0u || sum.high != 0u;
}

static bool tap_push(struct tap_cell *cell, struct tap_vertex p)
{
	if (cell->count == TAP_CELL_VERTICES || p.d == 0)
		return false;
	cell->point[cell->count++] = p;
	return true;
}

/* Walk the convex polygon in order. Vertices on the cut belong to
 * both halves; only a strict crossing creates a new vertex. */
static bool tap_clip(struct tap_cell *cell, struct tap_plane cut)
{
	if (cell->count == 0)
		return true;
	if (cut.x == 0 && cut.y == 0) {
		if (cut.room < 0)
			cell->count = 0;
		return true;
	}
	struct tap_cell clipped;
	clipped.count = 0;
	int before = tap_side(cut, cell->point[cell->count - 1u]);
	for (size_t i = 0; i < cell->count; ++i) {
		struct tap_vertex v = cell->point[i];
		int side = tap_side(cut, v);
		if ((before < 0 && side > 0) || (before > 0 && side < 0)) {
			struct tap_vertex crossing = tap_intersection(v.edge, cut);
			if (before < 0)
				crossing.edge = cut;
			if (!tap_push(&clipped, crossing))
				return false;
		}
		if (side >= 0) {
			if (side == 0 && before < 0)
				v.edge = cut;
			if (!tap_push(&clipped, v))
				return false;
		}
		before = side;
	}
	memcpy(cell->point, clipped.point, clipped.count * sizeof(*cell->point));
	cell->count = clipped.count;
	return true;
}

static size_t tap_search(const struct tap_phase *phase, int64_t numerator, int64_t denominator)
{
	size_t low = 0, high = phase->count;
	while (low + 1u < high) {
		size_t mid = (low + high) / 2u;
		struct tap_knot knot = phase->segment[mid].start;
		if (tap_wide_negative(wide_subtract(wide_product_s64(numerator, knot.denominator),
			wide_product_s64(knot.numerator, denominator))))
			high = mid;
		else
			low = mid;
	}
	return low;
}

static int32_t tap_axis_limit(int32_t limit, int64_t slope, int64_t room)
{
	if (slope <= 0)
		return limit;
	int64_t crossing = (room + slope - 1) / slope;
	return crossing < limit ? (int32_t)crossing : limit;
}

/* Crop the tap box to the range in which the original map can
 * have nonnegative reserve, then walk the strips between its knots. */
static bool tap_compare_sector(
	const struct tap_phase original[V90_CP_FRAME_INTERVALS],
	const struct tap_phase selected[V90_CP_FRAME_INTERVALS], int sign,
	unsigned *strips_left)
{
	int32_t first = MARGIN_UNITY, second = MARGIN_UNITY;
	size_t active[V90_CP_FRAME_INTERVALS];
	for (unsigned i = 0; i < V90_CP_FRAME_INTERVALS; ++i) {
		for (size_t j = 0; j < original[i].count; ++j) {
			struct tap_plane p = tap_at(&original[i], j, sign);
			int64_t first_approach = -p.x, second_approach = -(sign * p.x + p.y);
			/* The other nonnegative coefficient can only use more room. */
			if (second_approach >= 0)
				first = tap_axis_limit(first, first_approach, p.room);
			if (first_approach >= 0)
				second = tap_axis_limit(second, second_approach, p.room);
		}
	}
	struct tap_cell box = { {
		{ 0, 0, 1, { 1, -sign, 0 } },
		{ first, 0, 1, { 0, 1, 0 } },
		{ first + sign * second, second, 1, { -1, sign, first } },
		{ sign * second, second, 1, { 0, -1, second } },
	}, 4 };
	struct tap_knot left = { sign < 0 ? -second : 0, 1 };
	const struct tap_knot end = { first + (sign > 0 ? second : 0), 1 };
	for (unsigned i = 0; i < V90_CP_FRAME_INTERVALS; ++i)
		active[i] = tap_search(&original[i], left.numerator, left.denominator);
	while (*strips_left != 0u) {
		--*strips_left;
		struct tap_knot right = end;
		struct tap_plane plane[V90_CP_FRAME_INTERVALS];
		for (unsigned i = 0; i < V90_CP_FRAME_INTERVALS; ++i) {
			plane[i] = tap_at(&original[i], active[i], sign);
			if (active[i] + 1u < original[i].count &&
			    tap_knot_less(original[i].segment[active[i] + 1u].start, right))
				right = original[i].segment[active[i] + 1u].start;
		}
		struct tap_cell region = box;
		if (!tap_clip(&region, (struct tap_plane){ left.denominator, 0, -left.numerator }) ||
		    !tap_clip(&region, (struct tap_plane){ -right.denominator, 0, right.numerator }))
			return false;
		for (unsigned i = 0; i < V90_CP_FRAME_INTERVALS && region.count != 0u; ++i)
			if (!tap_clip(&region, plane[i]))
				return false;
		/* Clip each cell where one baseline plane is the minimum.
		 * The candidate reserve is concave, so vertices suffice. */
		for (unsigned i = 0; i < V90_CP_FRAME_INTERVALS && region.count != 0u; ++i) {
			struct tap_cell cell = region;
			for (unsigned j = 0; j < V90_CP_FRAME_INTERVALS && cell.count != 0u; ++j)
				if (i != j && !tap_clip(&cell, tap_subtract(plane[j], plane[i])))
					return false;
			for (size_t j = 0; j < cell.count; ++j) {
				const struct tap_vertex vertex = cell.point[j];
				for (unsigned k = 0; k < V90_CP_FRAME_INTERVALS; ++k) {
					size_t at = tap_search(&selected[k], vertex.x, vertex.d);
					struct tap_plane candidate = tap_at(&selected[k], at, sign);
					if (tap_side(tap_subtract(candidate, plane[i]), vertex) < 0)
						return false;
				}
			}
		}
		if (!tap_knot_less(right, end))
			return true;
		left = right;
		for (unsigned i = 0; i < V90_CP_FRAME_INTERVALS; ++i) {
			if (active[i] + 1u < original[i].count &&
			    !tap_knot_less(left, original[i].segment[active[i] + 1u].start))
				++active[i];
		}
	}
	return false;
}

static bool preserves_joint_margin(enum g711_law law,
	const struct pcm_constellation original[V90_CP_FRAME_INTERVALS],
	const struct pcm_constellation selected[V90_CP_FRAME_INTERVALS])
{
	/* Bound synchronous training work. A map that cannot be certified
	 * within this budget falls back to the first map at the same rate. */
	unsigned strips_left = 64;
	struct tap_phase before[V90_CP_FRAME_INTERVALS];
	struct tap_phase after[V90_CP_FRAME_INTERVALS];
	return tap_phases(law, original, before) && tap_phases(law, selected, after) &&
		tap_compare_sector(before, after, 1, &strips_left) &&
		tap_compare_sector(before, after, -1, &strips_left);
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
	bool found = false, distinct_masks = false, expanded = false;
	int16_t peak;
	unsigned source[V90_CP_FRAME_INTERVALS];
	struct spaced_levels storage[V90_CP_FRAME_INTERVALS];
	const struct spaced_levels *levels[V90_CP_FRAME_INTERVALS];
	struct pcm_constellation original[V90_CP_FRAME_INTERVALS];

	pcm_constellation_sources(available, source);
	for (unsigned slot = 0; slot < V90_CP_FRAME_INTERVALS; slot++) {
		levels[slot] = &storage[source[slot]];
		distinct_masks |= source[slot] != 0;
		if (mapping->modulus[slot] > largest)
			largest = mapping->modulus[slot];
		capacity *= root;
	}
	/* Only generated common-row maps may trade capacity between phases.
	 * Peer-owned maps retain every interval's independent cardinality. */
	bool refine = balance && root != V90_INITIAL_CONSTELLATION_SIZE && distinct_masks;
	if (balance && root != V90_INITIAL_CONSTELLATION_SIZE) {
		families[0] = balanced_family(root);
		count = 1;
		if (available) {
			prepare_phase_levels(storage, source, mapping->law, 1, available);
			count += asymmetric_families(root, words, levels, families + 1);
		}
		if (count == 1 && capacity == words)
			count = 0;
	}
	if (!g711_ucode_sample(mapping->law, V90_CONSTELLATION_LAST_UCODE, true, &peak))
		return false;
	prepare_capacity_counts(families, count, words);
	uint32_t maximum_spacing = 2u * (uint32_t)peak / (2u * largest - 1u);
	uint32_t low = 1, high = maximum_spacing;
	while (low <= high) {
		uint32_t spacing = low + (high - low) / 2u;
		uint8_t best[V90_CP_FRAME_INTERVALS] = { 0 };
		prepare_phase_levels(storage, source, mapping->law, spacing, available);
		if (choose_cardinalities(mapping, maximum_power, levels, families, count, best)) {
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
		/* A mask may qualify many tightly clustered levels. At the first
		 * rejected distance, freeze caps from the spaced levels for a
		 * second search. The new search must improve the previous map. */
		if (refine && found && low > high && low <= maximum_spacing) {
			refine = false;
			prepare_phase_levels(storage, source, mapping->law, low, available);
			count = 1 + asymmetric_families(root, words, levels, families + 1);
			prepare_capacity_counts(families, count, words);
			if (count > 1 &&
			    choose_cardinalities(mapping, maximum_power, levels, families, count, best)) {
				memcpy(original, selected, sizeof(original));
				high = maximum_spacing;
				expanded = true;
			}
		}
	}
	if (expanded && !preserves_joint_margin(mapping->law, original, selected))
		memcpy(selected, original, sizeof(original));
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
