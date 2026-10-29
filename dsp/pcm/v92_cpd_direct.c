// SPDX-License-Identifier: BSD-3-Clause

#include "v92_cpd_direct.h"

#include "integer_math.h"
#include "v92_cpd_gain.h"
#include "v92_cpd_moduli.h"
#include "v92_pcm_frame.h"

#include <limits.h>
#include <string.h>

enum {
	V92_CPD_DIRECT_40K_RATE_NUMBER = 13,
	V92_CPD_DIRECT_LINEAR_STEP = 256,
};

enum separation_rule {
	SEPARATION_NOMINAL,
	SEPARATION_CELL_MARGIN,
	SEPARATION_DISJOINT_MARGIN,
};

static bool direct_constellation(
	struct v92_cpd_constellation *constellation,
	enum g711_law law, uint8_t point_count)
{
	if (constellation == NULL || !g711_law_is_valid(law) ||
	    point_count == 0u ||
	    point_count > V92_CPD_MAX_CONSTELLATION_POINTS)
		return false;
	constellation->count = point_count;
	for (uint8_t point = 0u; point < point_count; ++point) {
		int16_t level;

		if (point_count <= V92_CPD_DIRECT_LINEAR_POINT_LIMIT) {
			level = (int16_t)((2u * point + 1u) *
				V92_CPD_DIRECT_LINEAR_STEP);
		} else if (!g711_ucode_sample(
				   law, (uint8_t)(point + 1u), true,
				   &level)) {
			return false;
		}
		if (level <= 0 ||
		    (point != 0u &&
		     level <= constellation->positive_linear[point - 1u]))
			return false;
		constellation->positive_linear[point] = level;
	}
	return true;
}

static bool direct_tables(
	struct v92_cpd_parameters *parameters, enum g711_law law,
	uint8_t data_rate_number, bool extend_e2u, bool direct_channel)
{
	unsigned int data_bits =
		pcm_upstream_frame_bits(data_rate_number);
	bool balanced = data_rate_number == 16u ||
		(direct_channel && data_rate_number >= 15u);

	if (parameters == NULL || !g711_law_is_valid(law) ||
	    data_bits == 0u)
		return false;
	*parameters = (struct v92_cpd_parameters){
		.four_times_gain_q0_16 = UINT16_MAX,
		.data_rate_number = data_rate_number,
		.trellis = data_rate_number == V92_CPD_DIRECT_40K_RATE_NUMBER ?
			V92_CPD_TRELLIS_32_STATE :
			V92_CPD_TRELLIS_16_STATE,
		.extend_e2u = extend_e2u,
		.modulus_present = true,
		.filters_present = true,
		.constellations_present = true,
	};
	parameters->filter[V92_CPD_PREFILTER_FEED_FORWARD].count =
		V92_CPD_DIRECT_FILTER_TAPS;
	parameters->filter[V92_CPD_PREFILTER_FEED_FORWARD]
		.coefficient[0] = INT16_MAX;
	/* Retain the level-neutral template and its 44k exception. Direct
	 * high-rate selection uses the same minimum balanced capacity as the
	 * channel designer, before measuring B1u. */
	if (balanced && !v92_cpd_balanced_moduli(parameters->modulus, data_rate_number))
		return false;
	for (size_t symbol = 0u;
	     symbol < V92_CPD_MODULUS_COUNT; ++symbol) {
		unsigned int exponent = data_bits / V92_CPD_MODULUS_COUNT;

		if (symbol < data_bits % V92_CPD_MODULUS_COUNT)
			exponent++;
		if (!balanced)
			parameters->modulus[symbol] = (uint8_t)(1u << exponent);
	}
	uint8_t point_count = v92_cpd_minimum_points(parameters->modulus);
	/* The 40k template has four representatives per modulus-32 value.
	 * The identity precoder uses only the inner half; direct selection
	 * omits the unreachable outer points before measuring B1u. */
	if (!direct_channel && data_rate_number == V92_CPD_DIRECT_40K_RATE_NUMBER)
		point_count = V92_CPD_DIRECT_MAX_POINTS;
	return direct_constellation(
		&parameters->constellation[0], law, point_count);
}

static bool normalize_gain(
	struct v92_cpd_parameters *parameters,
	const uint16_t frequencies[2 * V92_CPD_MAX_CONSTELLATION_POINTS])
{
	const struct v92_cpd_constellation *points = &parameters->constellation[0];
	uint64_t energy = 0u;

	/* For a monotone constellation and identity precoder, the chosen inner
	 * representative depends only on the coded index, not point spacing.
	 * Reuse exact signed B1u frequencies instead of encoding 48 frames for
	 * every search candidate on the live media thread. Energy is bounded by
	 * 576 signed 16-bit samples, comfortably inside uint64_t. */
	for (int eta = -(int)points->count; eta < points->count; ++eta) {
		int u = eta < 0 ? -points->positive_linear[-eta - 1] :
			points->positive_linear[eta];
		int64_t v = round_shift_s64_to_s32_saturating((int64_t)u * INT16_MAX, 15u);

		energy += (uint64_t)(v * v) * frequencies[eta + V92_CPD_MAX_CONSTELLATION_POINTS];
	}
	parameters->four_times_gain_q0_16 =
		v92_cpd_gain_for_energy(energy, V92_B1U_FRAMES * V92_PCM_MAPPING_FRAME_SYMBOLS);
	return parameters->four_times_gain_q0_16 != 0u;
}

static bool initialize_direct(
	struct v92_cpd_parameters *parameters,
	uint16_t frequencies[2 * V92_CPD_MAX_CONSTELLATION_POINTS],
	enum g711_law law, uint8_t rate, bool extend_e2u, bool direct_channel)
{
	struct v92_b1u_measurement measurement;

	if (!direct_tables(parameters, law, rate, extend_e2u, direct_channel) ||
	    v92_pcm_frame_measure_b1u(parameters, law, &measurement) != V92_PCM_FRAME_OK)
		return false;
	memcpy(frequencies, measurement.signed_frequencies,
		sizeof(measurement.signed_frequencies));
	parameters->four_times_gain_q0_16 =
		v92_cpd_gain_for_energy(measurement.prefilter_energy, V92_B1U_SYMBOLS);
	return parameters->four_times_gain_q0_16 != 0u &&
	       v92_cpd_parameters_are_valid(parameters);
}

enum v92_cpd_result v92_cpd_direct_init(
	struct v92_cpd_parameters *parameters, enum g711_law law,
	uint8_t data_rate_number, bool extend_e2u)
{
	struct v92_cpd_parameters generated;
	uint16_t frequencies[2 * V92_CPD_MAX_CONSTELLATION_POINTS];

	if (parameters == NULL)
		return V92_CPD_INVALID_ARGUMENT;
	if (!initialize_direct(&generated, frequencies, law,
		    data_rate_number, extend_e2u, false))
		return V92_CPD_INVALID_PARAMETERS;
	*parameters = generated;
	return V92_CPD_OK;
}

/* First integer pre-codec noise peak at which adjacent source levels can
 * share a reconstructed value. Decoded-code spacing alone overstates the
 * margin when one symbol sits near a quantizer boundary. */
static int input_margin(enum g711_law law, int lower, int upper)
{
	struct g711_interval cell;

	if (lower >= upper ||
	    !g711_quantization_interval(
		    law, (int16_t)(lower + (upper - lower) / 2), &cell))
		return 0;
	/* Any common cell reached before the inputs cross must contain their
	 * midpoint. Move both inputs just far enough to enter that cell; when
	 * an odd midpoint straddles a boundary they can instead cross there. */
	int margin = cell.lower - lower;

	if (upper - cell.upper > margin)
		margin = upper - cell.upper;
	if (margin < 0)
		margin = 0;
	int crossing = (upper - lower + 1) / 2;

	return margin < crossing ? margin : crossing;
}

/* Preserve the transmitter's identity-prefilter rounding before applying
 * F and LU. All three factors give a product below 2^46, retained in
 * int64_t until its final rounding. */
static int64_t point_gain(const struct v92_cpd_parameters *cpd, int eta)
{
	const struct v92_cpd_constellation *points = &cpd->constellation[0];
	int u = eta < 0 ? -points->positive_linear[-eta - 1] :
		points->positive_linear[eta];
	int32_t v = round_shift_s64_to_s32_saturating((int64_t)u * INT16_MAX, 15u);

	return (int64_t)v * cpd->four_times_gain_q0_16;
}

static int scale_at_level(int64_t point, int lu)
{
	return round_shift_s64_to_s32_saturating(point * lu, 18u);
}

static int scaled_point(const struct v92_cpd_parameters *cpd, int eta, int lu)
{
	return scale_at_level(point_gain(cpd, eta), lu);
}

/* Both signs matter: G.711's signed input cells are not symmetric.
 * For an LU interval, monotonicity bounds each symbol by its endpoints.
 * Reject a candidate as soon as it cannot beat the retained margin. */
static int level_separation(
	const struct v92_cpd_parameters *cpd, enum g711_law law,
	int lower_lu, int upper_lu, bool input_noise, int required_margin)
{
	int count = cpd->constellation[0].count;
	int previous = 0, minimum = INT16_MAX;

	for (int eta = -count; eta < count; ++eta) {
		int lower = scaled_point(cpd, eta, eta < 0 ? upper_lu : lower_lu);
		int upper = scaled_point(cpd, eta, eta < 0 ? lower_lu : upper_lu);

		if (lower <= INT16_MIN || upper >= INT16_MAX)
			return 0;
		if (eta != -count) {
			int gap = input_noise ?
				input_margin(law, previous, lower) :
				g711_quantize(law, (int16_t)lower) -
					g711_quantize(law, (int16_t)previous);

			if (gap <= required_margin)
				return 0;
			if (gap < minimum)
				minimum = gap;
		}
		previous = upper;
	}
	return minimum;
}

/* Minimum peak input noise needed for both points to enter one codec bin. */
static int bin_margin(
	int64_t left, int64_t right, int lu, const struct g711_interval *bin)
{
	int distance = bin->lower - scale_at_level(left, lu);
	int other = scale_at_level(right, lu) - bin->upper;

	if (other > distance)
		distance = other;
	return distance < 0 ? 0 : distance;
}

static int pair_input_margin(
	enum g711_law law, int64_t left, int64_t right, int lower_lu, int upper_lu)
{
	int a = scale_at_level(left, lower_lu);
	int b = scale_at_level(right, lower_lu);

	/* Across zero both distances to any fixed bin grow with LU. */
	if (left <= 0 && right >= 0)
		return input_margin(law, a, b);
	int first = a + (b - a) / 2;

	a = scale_at_level(left, upper_lu);
	b = scale_at_level(right, upper_lu);
	int last = a + (b - a) / 2;

	if (first > last) {
		int swap = first;

		first = last;
		last = swap;
	}
	int minimum = INT16_MAX;

	/* An optimal shared bin contains the integer midpoint. Its range is
	 * monotone for same-sign points, so only these bins need inspection. */
	for (int midpoint = first; midpoint <= last;) {
		struct g711_interval bin;

		(void)g711_quantization_interval(law, (int16_t)midpoint, &bin);
		int low = lower_lu, high = upper_lu + 1;
		int center = bin.lower + bin.upper;

		/* One bin-entry distance decreases while the other grows. Their
		 * maximum is minimized at their crossing or its predecessor. */
		while (low < high) {
			int lu = low + (high - low) / 2;
			int sum = scale_at_level(left, lu) + scale_at_level(right, lu);

			if (left >= 0 ? sum >= center : sum <= center)
				high = lu;
			else
				low = lu + 1;
		}
		int candidate = low > upper_lu ? upper_lu : low;
		int margin = bin_margin(left, right, candidate, &bin);

		if (margin < minimum)
			minimum = margin;
		if (low > lower_lu) {
			margin = bin_margin(left, right, low - 1, &bin);
			if (margin < minimum)
				minimum = margin;
		}
		if (bin.upper >= last)
			break;
		midpoint = bin.upper + 1;
	}
	return minimum;
}

static int cell_separation(
	const struct v92_cpd_parameters *cpd, enum g711_law law,
	const struct g711_interval *levels, int required_margin)
{
	int count = cpd->constellation[0].count;
	int64_t previous = point_gain(cpd, -count);

	/* Monotonicity bounds every source point by the outer endpoints. Keep
	 * the same strict clipping limits as the per-level scorer. Together with
	 * the already validated law, this makes every following codec-bin query
	 * valid without another per-pair validation layer. */
	if (scale_at_level(previous, levels->upper) <= INT16_MIN ||
	    scale_at_level(point_gain(cpd, count - 1), levels->upper) >= INT16_MAX)
		return 0;
	int minimum = INT16_MAX;

	for (int eta = -count + 1; eta < count; ++eta) {
		int64_t next = point_gain(cpd, eta);
		int separation = pair_input_margin(
			law, previous, next, levels->lower, levels->upper);

		if (separation <= required_margin)
			return 0;
		if (separation < minimum)
			minimum = separation;
		previous = next;
	}
	return minimum;
}

static int direct_separation(
	const struct v92_cpd_parameters *cpd, enum g711_law law, int16_t lu,
	enum separation_rule rule, int required_margin)
{
	if (rule == SEPARATION_NOMINAL)
		return level_separation(cpd, law, lu, lu, false, required_margin);
	/* TRN1u supplies a quantized LU. Every balanced alphabet must stay
	 * injective at each level in that cell. Prefer disjoint symbol ranges
	 * at 46.7k so the receiver need not resolve LU to distinguish them.
	 * At 48k, maximize per-level noise margin instead: B1u refines LU,
	 * while separating entire cells can sacrifice most of that margin. */
	struct g711_interval interval;

	if (!g711_quantization_interval(law, lu, &interval) || interval.lower <= 0)
		return 0;
	if (rule == SEPARATION_DISJOINT_MARGIN)
		return level_separation(
			cpd, law, interval.lower, interval.upper, true, required_margin);
	/* A minimum over the LU cell cannot exceed its value at the
	 * reported level. Reject candidates that cannot improve the retained
	 * margin before enumerating the exact codec-bin crossings. */
	if (!level_separation(cpd, law, lu, lu, true, required_margin))
		return 0;
	return cell_separation(cpd, law, &interval, required_margin);
}

static enum v92_cpd_result select_mapping(
	struct v92_cpd_parameters *parameters, enum g711_law law,
	int16_t upstream_level, const struct v92_cpd_parameters *initial,
	const uint16_t frequencies[2 * V92_CPD_MAX_CONSTELLATION_POINTS],
	enum separation_rule rule)
{
	struct v92_cpd_parameters candidate = *initial, best;
	uint8_t data_rate_number = initial->data_rate_number;
	unsigned int count;
	int best_separation = 0;
	bool optimize_margin = rule == SEPARATION_CELL_MARGIN;

	/* Full-cell selection maximizes pre-codec noise margin within the
	 * bounded family. Nominal and disjoint selection retain their first
	 * usable template. Decoded-code spacing alone cannot bound input noise. */
	if (data_rate_number != 19u) {
		int separation = direct_separation(&candidate, law, upstream_level, rule, 0);

		if (separation > 0) {
			if (!optimize_margin) {
				*parameters = candidate;
				return V92_CPD_OK;
			}
			best = candidate;
			best_separation = separation;
		}
	}
	/* V.92 Table 30 permits arbitrary positive linear points. Search a
	 * bounded set of contiguous G.711 windows, including quarter-code
	 * offsets; renormalize the actual B1u waveform for each candidate.
	 * This is a direct-channel selector, not a filtered-channel designer.
	 * If no injective map exists in this family, let policy try a lower rate. */
	count = candidate.constellation[0].count;
	for (unsigned int slot = law == G711_LAW_PCMU ? 4u : 0u;
	     slot / 4u + count <= 128u; ++slot) {
		unsigned int start = slot / 4u, fraction = slot % 4u;
		int separation;

		if (fraction != 0u && start + count == 128u)
			continue;
		for (unsigned int point = 0u; point < count; ++point) {
			int16_t low, high;

			if (!g711_ucode_sample(law, (uint8_t)(start + point), true, &low))
				return V92_CPD_INVALID_PARAMETERS;
			if (fraction != 0u) {
				if (!g711_ucode_sample(law, (uint8_t)(start + point + 1u), true, &high))
					return V92_CPD_INVALID_PARAMETERS;
				low += (int16_t)((high - low) * fraction / 4u);
			}
			candidate.constellation[0].positive_linear[point] = low;
		}
		if (!normalize_gain(&candidate, frequencies))
			return V92_CPD_INVALID_PARAMETERS;
		separation = direct_separation(
			&candidate, law, upstream_level, rule, best_separation);
		if (separation > best_separation) {
			best_separation = separation;
			best = candidate;
		}
	}
	if (optimize_margin || (best_separation == 0 &&
	    data_rate_number >= 15u && data_rate_number <= 19u)) {
		/* Contiguous codec windows can have no placement that survives the
		 * whole LU cell. Try a bounded family of linear/cubic grids:
		 * a(x) = 30000 * (x + s*x^3)/(1+s), x=(2i+1)/(2N-1), s=0..16.
		 * The linear term keeps spacing near zero; curvature increases the
		 * outer gaps. At 40k and 48k, use quarter steps and compare against codec
		 * windows too: integer shapes can miss useful source separation.
		 * Integer rounding makes generation deterministic; all intermediates
		 * fit uint64_t for N<=64. Reuse B1u frequencies
		 * because monotone spacing does not change inner representatives. */
		uint64_t width = 2u * count - 1u;
		unsigned resolution = (optimize_margin && data_rate_number == 13u) ||
			data_rate_number == 19u ? 4u : 1u;

		for (unsigned shape = 0u; shape <= 16u * resolution; ++shape) {
			uint64_t denominator = (shape + resolution) * width * width * width;

			for (unsigned point = 0u; point < count; ++point) {
				uint64_t x = 2u * point + 1u;
				uint64_t numerator = 30000u * x *
					(resolution * width * width + shape * x * x);

				candidate.constellation[0].positive_linear[point] =
					(int16_t)((numerator + denominator / 2u) / denominator);
			}
			if (!normalize_gain(&candidate, frequencies))
				return V92_CPD_INVALID_PARAMETERS;
			int separation = direct_separation(
				&candidate, law, upstream_level, rule, best_separation);

			if (separation > best_separation) {
				best_separation = separation;
				best = candidate;
			}
		}
	}
	if (best_separation == 0)
		return V92_CPD_INVALID_PARAMETERS;
	*parameters = best;
	return V92_CPD_OK;
}

enum v92_cpd_result v92_cpd_direct_select(
	struct v92_cpd_parameters *parameters, enum g711_law law,
	uint8_t data_rate_number, int16_t upstream_level, bool extend_e2u)
{
	struct v92_cpd_parameters initial;
	uint16_t frequencies[2 * V92_CPD_MAX_CONSTELLATION_POINTS];

	if (parameters == NULL)
		return V92_CPD_INVALID_ARGUMENT;
	if (upstream_level <= 0 ||
	    !initialize_direct(&initial, frequencies, law,
		    data_rate_number, extend_e2u, true))
		return V92_CPD_INVALID_PARAMETERS;
	/* Prefer a full-cell noise margin where the bounded family can provide
	 * one. A missing scalar bound does not prove the coded channel unusable;
	 * retain the existing nominal selector as the fallback. Both searches
	 * reuse this call's exact B1u frequencies and independent candidates. */
	if (data_rate_number == 13u || data_rate_number == 14u) {
		enum v92_cpd_result result = select_mapping(
			parameters, law, upstream_level, &initial, frequencies,
			SEPARATION_CELL_MARGIN);

		if (result != V92_CPD_INVALID_PARAMETERS)
			return result;
	}
	if (data_rate_number == 18u) {
		enum v92_cpd_result result = select_mapping(
			parameters, law, upstream_level, &initial, frequencies,
			SEPARATION_DISJOINT_MARGIN);

		if (result != V92_CPD_INVALID_PARAMETERS)
			return result;
	}
	enum separation_rule rule = data_rate_number >= 15u ?
		SEPARATION_CELL_MARGIN : SEPARATION_NOMINAL;

	return select_mapping(
		parameters, law, upstream_level, &initial, frequencies, rule);
}
