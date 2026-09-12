// SPDX-License-Identifier: BSD-3-Clause

#include "v92_cpd_direct.h"

#include "integer_math.h"
#include "v92_cpd_gain.h"
#include "v92_pcm_frame.h"

#include <limits.h>
#include <string.h>

enum {
	V92_CPD_DIRECT_40K_RATE_NUMBER = 13,
	V92_CPD_DIRECT_LINEAR_STEP = 256,
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
	uint8_t data_rate_number, bool extend_e2u)
{
	unsigned int data_bits =
		v92_pcm_mapping_data_bits(data_rate_number);
	uint8_t point_count = 0u;

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
	/* Spread the 44k frame's 66 data bits evenly across the intervals.
	 * Nine modulus-54 and three modulus-27 trellis intervals have product
	 * 54^9 * 27^3 >= 2^66 (V.92 6.4.1); 52/26 would be too small. Each
	 * interval needs only 54 signed points, rather than concentrating 128
	 * in one interval. This improves quantizer separation for direct calls
	 * and leaves equivalent representatives for channel precoder feedback. */
	for (size_t symbol = 0u;
	     symbol < V92_CPD_MODULUS_COUNT; ++symbol) {
		unsigned int exponent = data_bits / V92_CPD_MODULUS_COUNT;
		uint8_t required;

		if (symbol < data_bits % V92_CPD_MODULUS_COUNT)
			exponent++;
		parameters->modulus[symbol] = (uint8_t)(1u << exponent);
		if (data_rate_number == 16u)
			parameters->modulus[symbol] = symbol % 4u == 3u ? 27u : 54u;
		required = symbol % 4u == 3u ?
			parameters->modulus[symbol] :
			(uint8_t)((parameters->modulus[symbol] + 1u) / 2u);
		if (point_count < required)
			point_count = required;
	}
	/* The 40 kbit/s selection uses the 32-state trellis. A 64-point
	 * positive half gives each modulus-32 value four representatives. */
	if (data_rate_number == V92_CPD_DIRECT_40K_RATE_NUMBER)
		point_count = V92_CPD_DIRECT_MAX_POINTS;
	return direct_constellation(
		&parameters->constellation[0], law, point_count);
}

static bool b1u_frequencies(
	const struct v92_cpd_parameters *parameters, enum g711_law law,
	uint16_t frequencies[2 * V92_CPD_DIRECT_MAX_POINTS])
{
	struct v92_pcm_mapping mapping;
	struct v92_pcm_frame_tx transmitter;
	uint8_t ones[V92_PCM_MAPPING_MAX_BITS];

	if (v92_pcm_mapping_init(&mapping, law, parameters) !=
			V92_PCM_MAPPING_OK ||
	    v92_pcm_frame_tx_init(&transmitter, &mapping) !=
			V92_PCM_FRAME_OK)
		return false;
	memset(frequencies, 0, 2u * V92_CPD_DIRECT_MAX_POINTS * sizeof(*frequencies));
	memset(ones, 1, mapping.data_bits_per_frame);
	for (unsigned int index = 0u;
	     index < V92_B1U_FRAMES; ++index) {
		struct v92_pcm_frame frame;

		if (v92_pcm_frame_encode(
			    &transmitter, ones, mapping.data_bits_per_frame,
			    &frame) != V92_PCM_FRAME_OK)
			return false;
		for (size_t symbol = 0u;
		     symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
			int index = frame.signed_index[symbol] + V92_CPD_DIRECT_MAX_POINTS;

			if (index < 0 || index >= 2 * V92_CPD_DIRECT_MAX_POINTS)
				return false;
			frequencies[index]++;
		}
	}
	return true;
}

static bool normalize_gain(
	struct v92_cpd_parameters *parameters,
	const uint16_t frequencies[2 * V92_CPD_DIRECT_MAX_POINTS])
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

		energy += (uint64_t)(v * v) * frequencies[eta + V92_CPD_DIRECT_MAX_POINTS];
	}
	parameters->four_times_gain_q0_16 =
		v92_cpd_gain_for_energy(energy, V92_B1U_FRAMES * V92_PCM_MAPPING_FRAME_SYMBOLS);
	return parameters->four_times_gain_q0_16 != 0u;
}

enum v92_cpd_result v92_cpd_direct_init(
	struct v92_cpd_parameters *parameters, enum g711_law law,
	uint8_t data_rate_number, bool extend_e2u)
{
	struct v92_cpd_parameters generated;
	uint16_t frequencies[2 * V92_CPD_DIRECT_MAX_POINTS];

	if (parameters == NULL)
		return V92_CPD_INVALID_ARGUMENT;
	if (!direct_tables(
		    &generated, law, data_rate_number, extend_e2u) ||
	    !b1u_frequencies(&generated, law, frequencies) ||
	    !normalize_gain(&generated, frequencies) ||
	    !v92_cpd_parameters_are_valid(&generated))
		return V92_CPD_INVALID_PARAMETERS;
	*parameters = generated;
	return V92_CPD_OK;
}

/* Identity-filter output followed by the analogue LU/G scaling and network
 * quantizer. Check both signs: integer rounding need not be symmetric. */
static int level_separation(
	const struct v92_cpd_parameters *cpd, enum g711_law law, int16_t lu)
{
	const struct v92_cpd_constellation *points = &cpd->constellation[0];
	int previous = 0, minimum = INT16_MAX;

	for (int eta = -(int)points->count; eta < points->count; ++eta) {
		int u = eta < 0 ? -points->positive_linear[-eta - 1] :
			points->positive_linear[eta];
		int32_t v = round_shift_s64_to_s32_saturating(
			(int64_t)u * INT16_MAX, 15u);
		int32_t scaled = round_shift_s64_to_s32_saturating(
			(int64_t)v * cpd->four_times_gain_q0_16 * lu, 18u);
		int sample;

		if (scaled <= INT16_MIN || scaled >= INT16_MAX)
			return 0;
		sample = law == G711_LAW_PCMA ?
			g711_decode_pcma(g711_encode_pcma((int16_t)scaled)) :
			g711_decode_pcmu(g711_encode_pcmu((int16_t)scaled));
		if (eta != -(int)points->count && sample - previous < minimum)
			minimum = sample - previous;
		previous = sample;
	}
	return minimum;
}

static int direct_separation(
	const struct v92_cpd_parameters *cpd, enum g711_law law, int16_t lu)
{
	if (cpd->data_rate_number != 15u && cpd->data_rate_number != 17u)
		return level_separation(cpd, law, lu);
	/* TRN1u supplies a quantized LU. The balanced direct alphabet must
	 * remain injective at every scalar consistent with that observation,
	 * not only its decoded centre. Adjacent points can otherwise collapse
	 * inside the cell even though the selected nominal spacing is large. */
	struct g711_interval interval;
	int minimum = INT16_MAX;

	if (!g711_quantization_interval(law, lu, &interval) || interval.lower <= 0)
		return 0;
	for (int level = interval.lower; level <= interval.upper; ++level) {
		int separation = level_separation(cpd, law, (int16_t)level);

		if (separation == 0)
			return 0;
		if (separation < minimum)
			minimum = separation;
	}
	return minimum;
}

enum v92_cpd_result v92_cpd_direct_select(
	struct v92_cpd_parameters *parameters, enum g711_law law,
	uint8_t data_rate_number, int16_t upstream_level, bool extend_e2u)
{
	struct v92_cpd_parameters candidate, best;
	uint16_t frequencies[2 * V92_CPD_DIRECT_MAX_POINTS];
	unsigned int count;
	int best_separation = 0;

	if (parameters == NULL)
		return V92_CPD_INVALID_ARGUMENT;
	if (upstream_level <= 0 ||
	    v92_cpd_direct_init(&candidate, law, data_rate_number, extend_e2u) != V92_CPD_OK)
		return V92_CPD_INVALID_PARAMETERS;
	/* Use 48/24 and 62/31 moduli for the 64- and 68-bit direct frames.
	 * These are the smallest uniform signed alphabets satisfying
	 * (2N)^9 * N^3 >= 2^K (V.92 6.4.1-6.4.2). Keep this direct
	 * allocation out of the channel designer's level-neutral template. */
	if (data_rate_number == 15u || data_rate_number == 17u) {
		uint8_t points = data_rate_number == 15u ? 24u : 31u;

		for (size_t symbol = 0u; symbol < V92_CPD_MODULUS_COUNT; ++symbol)
			candidate.modulus[symbol] = symbol % 4u == 3u ? points : 2u * points;
		memset(&candidate.constellation[0], 0, sizeof(candidate.constellation[0]));
		if (!direct_constellation(&candidate.constellation[0], law, points) ||
		    !b1u_frequencies(&candidate, law, frequencies) ||
		    !normalize_gain(&candidate, frequencies))
			return V92_CPD_INVALID_PARAMETERS;
	}
	/* The identity precoder always selects the inner representatives. At
	 * 40k its outer 32 positive points are unreachable; do not let unused
	 * points constrain placement or add competing receive branches. */
	if (data_rate_number == V92_CPD_DIRECT_40K_RATE_NUMBER) {
		candidate.constellation[0].count = 32u;
		memset(candidate.constellation[0].positive_linear + 32u, 0,
			(V92_CPD_MAX_CONSTELLATION_POINTS - 32u) * sizeof(int16_t));
	}
	if (direct_separation(&candidate, law, upstream_level) > 0) {
		*parameters = candidate;
		return V92_CPD_OK;
	}
	if (!b1u_frequencies(&candidate, law, frequencies))
		return V92_CPD_INVALID_PARAMETERS;
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
		separation = direct_separation(&candidate, law, upstream_level);
		if (separation > best_separation) {
			best_separation = separation;
			best = candidate;
		}
	}
	if (best_separation == 0 && (data_rate_number == 15u || data_rate_number == 17u)) {
		/* Contiguous codec windows can have no placement that survives the
		 * whole LU cell. Try a bounded family of linear/cubic grids:
		 * a(x) = 30000 * (x + s*x^3)/(1+s), x=(2i+1)/(2N-1), s=0..16.
		 * The linear term keeps spacing near zero; curvature increases the
		 * outer gaps. Integer rounding makes generation deterministic, and
		 * all intermediates fit uint64_t for N<=64. Reuse B1u frequencies
		 * because monotone spacing does not change inner representatives. */
		uint64_t width = 2u * count - 1u;

		for (unsigned shape = 0u; shape <= 16u; ++shape) {
			uint64_t denominator = (shape + 1u) * width * width * width;

			for (unsigned point = 0u; point < count; ++point) {
				uint64_t x = 2u * point + 1u;
				uint64_t numerator = 30000u * x * (width * width + shape * x * x);

				candidate.constellation[0].positive_linear[point] =
					(int16_t)((numerator + denominator / 2u) / denominator);
			}
			if (!normalize_gain(&candidate, frequencies))
				return V92_CPD_INVALID_PARAMETERS;
			int separation = direct_separation(&candidate, law, upstream_level);

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
