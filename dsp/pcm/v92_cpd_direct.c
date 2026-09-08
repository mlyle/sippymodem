// SPDX-License-Identifier: BSD-3-Clause

#include "v92_cpd_direct.h"

#include "v92_pcm_frame.h"

#include <limits.h>
#include <string.h>

enum {
	V92_CPD_DIRECT_40K_RATE_NUMBER = 13,
	V92_CPD_DIRECT_LINEAR_STEP = 256,
	V92_CPD_DIRECT_GAIN_TARGET_SHIFT = 36,
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
	for (size_t symbol = 0u;
	     symbol < V92_CPD_MODULUS_COUNT; ++symbol) {
		unsigned int exponent = data_bits / V92_CPD_MODULUS_COUNT;
		uint8_t required;

		if (symbol < data_bits % V92_CPD_MODULUS_COUNT)
			exponent++;
		parameters->modulus[symbol] = (uint8_t)(1u << exponent);
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

static bool add_energy(uint64_t *energy, int32_t value)
{
	uint64_t magnitude = value < 0 ?
		(uint64_t)(-(int64_t)value) : (uint64_t)value;
	uint64_t square;

	if (energy == NULL ||
	    (magnitude != 0u && magnitude > UINT64_MAX / magnitude))
		return false;
	square = magnitude * magnitude;
	if (*energy > UINT64_MAX - square)
		return false;
	*energy += square;
	return true;
}

static uint16_t nearest_gain(
	uint64_t prefilter_energy, size_t symbol_count)
{
	__uint128_t target;
	uint32_t low = 0u;
	uint32_t high = UINT16_MAX;

	if (prefilter_energy == 0u || symbol_count == 0u)
		return 0u;
	/* CPd carries F=4G in Q0.16, so G=F/2^18. Select the
	 * representable F whose mean squared B1u prefilter output is nearest
	 * unity. */
	target = (__uint128_t)symbol_count <<
		V92_CPD_DIRECT_GAIN_TARGET_SHIFT;
	while (low < high) {
		uint32_t middle = low + (high - low + 1u) / 2u;
		__uint128_t scaled =
			(__uint128_t)middle * middle * prefilter_energy;

		if (scaled <= target)
			low = middle;
		else
			high = middle - 1u;
	}
	if (low == 0u)
		return 1u;
	if (low < UINT16_MAX) {
		uint32_t upper = low + 1u;
		__uint128_t below = target -
			(__uint128_t)low * low * prefilter_energy;
		__uint128_t above =
			(__uint128_t)upper * upper * prefilter_energy - target;

		if (above < below)
			low = upper;
	}
	return (uint16_t)low;
}

static bool normalize_gain(
	struct v92_cpd_parameters *parameters, enum g711_law law)
{
	struct v92_pcm_mapping mapping;
	struct v92_pcm_frame_tx transmitter;
	uint8_t ones[V92_PCM_MAPPING_MAX_BITS];
	uint64_t energy = 0u;
	size_t symbol_count = 0u;

	if (v92_pcm_mapping_init(&mapping, law, parameters) !=
			V92_PCM_MAPPING_OK ||
	    v92_pcm_frame_tx_init(&transmitter, &mapping) !=
			V92_PCM_FRAME_OK)
		return false;
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
			if (!add_energy(&energy, frame.v[symbol]))
				return false;
			symbol_count++;
		}
	}
	parameters->four_times_gain_q0_16 =
		nearest_gain(energy, symbol_count);
	return parameters->four_times_gain_q0_16 != 0u;
}

enum v92_cpd_result v92_cpd_direct_init(
	struct v92_cpd_parameters *parameters, enum g711_law law,
	uint8_t data_rate_number, bool extend_e2u)
{
	struct v92_cpd_parameters generated;

	if (parameters == NULL)
		return V92_CPD_INVALID_ARGUMENT;
	if (!direct_tables(
		    &generated, law, data_rate_number, extend_e2u) ||
	    !normalize_gain(&generated, law) ||
	    !v92_cpd_parameters_are_valid(&generated))
		return V92_CPD_INVALID_PARAMETERS;
	*parameters = generated;
	return V92_CPD_OK;
}
