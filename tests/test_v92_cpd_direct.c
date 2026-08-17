// SPDX-License-Identifier: BSD-3-Clause

#include <v92_cpd_direct.h>
#include <v92_pcm_frame.h>

#include "test.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

enum {
	DIRECT_B1U_FRAMES = 48,
	DIRECT_B1U_SYMBOLS =
		DIRECT_B1U_FRAMES * V92_PCM_MAPPING_FRAME_SYMBOLS,
};

static uint8_t expected_point_count(uint8_t rate)
{
	unsigned int bits = v92_pcm_mapping_data_bits(rate);
	uint8_t count = 0u;

	for (size_t symbol = 0u;
	     symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
		unsigned int exponent = bits / V92_PCM_MAPPING_FRAME_SYMBOLS;
		uint8_t modulus;
		uint8_t required;

		if (symbol < bits % V92_PCM_MAPPING_FRAME_SYMBOLS)
			exponent++;
		modulus = (uint8_t)(1u << exponent);
		required = symbol % 4u == 3u ? modulus :
			(uint8_t)((modulus + 1u) / 2u);
		if (count < required)
			count = required;
	}
	return rate == 13u ? 64u : count;
}

static bool direct_sections_are_correct(
	const struct v92_cpd_parameters *cpd, enum g711_law law,
	uint8_t rate)
{
	uint8_t point_count = expected_point_count(rate);
	__uint128_t product = 1u;
	__uint128_t expected =
		(__uint128_t)1u << v92_pcm_mapping_data_bits(rate);

	CHECK(cpd->modulus_present && cpd->filters_present &&
	      cpd->constellations_present &&
	      cpd->filter[V92_CPD_PRECODER_FEED_FORWARD].count == 0u &&
	      cpd->filter[V92_CPD_PRECODER_FEEDBACK].count == 0u &&
	      cpd->filter[V92_CPD_PREFILTER_FEED_FORWARD].count == 4u &&
	      cpd->filter[V92_CPD_PREFILTER_FEED_FORWARD]
		      .coefficient[0] == INT16_MAX &&
	      cpd->filter[V92_CPD_PREFILTER_FEED_FORWARD]
		      .coefficient[1] == 0 &&
	      cpd->filter[V92_CPD_PREFILTER_FEED_FORWARD]
		      .coefficient[2] == 0 &&
	      cpd->filter[V92_CPD_PREFILTER_FEED_FORWARD]
		      .coefficient[3] == 0 &&
	      cpd->filter[V92_CPD_PREFILTER_FEEDBACK].count == 0u,
	      "rate %u law %u did not use the four-tap identity prefilter",
	      rate, law);
	for (size_t symbol = 0u;
	     symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
		unsigned int exponent =
			v92_pcm_mapping_data_bits(rate) /
			V92_PCM_MAPPING_FRAME_SYMBOLS;

		if (symbol < v92_pcm_mapping_data_bits(rate) %
				     V92_PCM_MAPPING_FRAME_SYMBOLS)
			exponent++;
		CHECK(cpd->modulus[symbol] == (uint8_t)(1u << exponent),
		      "rate %u law %u symbol %zu modulus changed",
		      rate, law, symbol);
		product *= cpd->modulus[symbol];
	}
	CHECK(product == expected,
	      "rate %u law %u modulus product changed", rate, law);
	CHECK(cpd->constellation[0].count == point_count,
	      "rate %u law %u generated %u rather than %u points",
	      rate, law, cpd->constellation[0].count, point_count);
	for (uint8_t point = 0u; point < point_count; ++point) {
		int16_t expected_level;

		if (point_count <= 16u) {
			expected_level = (int16_t)((2u * point + 1u) * 256u);
		} else {
			CHECK(g711_ucode_sample(
				      law, (uint8_t)(point + 1u), true,
				      &expected_level),
			      "rate %u law %u point %u could not be generated",
			      rate, law, point);
		}
		CHECK(cpd->constellation[0].positive_linear[point] ==
			      expected_level,
		      "rate %u law %u point %u changed", rate, law, point);
	}
	for (size_t phase = 0u; phase < V92_CPD_FRAME_INTERVALS; ++phase)
		CHECK(cpd->frame_constellation[phase] == 0u,
		      "rate %u law %u phase %zu changed constellation",
		      rate, law, phase);
	for (size_t index = 1u;
	     index < V92_CPD_MAX_CONSTELLATIONS; ++index)
		CHECK(cpd->constellation[index].count == 0u,
		      "rate %u law %u populated constellation %zu",
		      rate, law, index);
	return true;
}

static __uint128_t distance(__uint128_t left, __uint128_t right)
{
	return left >= right ? left - right : right - left;
}

static bool gain_is_nearest(
	const struct v92_cpd_parameters *cpd, enum g711_law law)
{
	struct v92_pcm_mapping mapping;
	struct v92_pcm_frame_tx transmitter;
	uint8_t ones[V92_PCM_MAPPING_MAX_BITS];
	uint64_t energy = 0u;
	uint32_t gain = cpd->four_times_gain_q0_16;
	__uint128_t target = (__uint128_t)DIRECT_B1U_SYMBOLS << 36u;
	__uint128_t selected;

	CHECK(v92_pcm_mapping_init(&mapping, law, cpd) ==
		      V92_PCM_MAPPING_OK &&
	      v92_pcm_frame_tx_init(&transmitter, &mapping) ==
		      V92_PCM_FRAME_OK,
	      "direct mapping did not initialize for gain verification");
	memset(ones, 1, mapping.data_bits_per_frame);
	for (unsigned int index = 0u;
	     index < DIRECT_B1U_FRAMES; ++index) {
		struct v92_pcm_frame frame;

		CHECK(v92_pcm_frame_encode(
			      &transmitter, ones,
			      mapping.data_bits_per_frame,
			      &frame) == V92_PCM_FRAME_OK,
		      "B1u frame %u did not encode", index);
		for (size_t symbol = 0u;
		     symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
			uint64_t magnitude = frame.v[symbol] < 0 ?
				(uint64_t)(-(int64_t)frame.v[symbol]) :
				(uint64_t)frame.v[symbol];

			CHECK(magnitude == 0u ||
			      magnitude <= UINT64_MAX / magnitude,
			      "B1u energy overflowed");
			CHECK(energy <= UINT64_MAX - magnitude * magnitude,
			      "B1u accumulated energy overflowed");
			energy += magnitude * magnitude;
		}
	}
	selected = (__uint128_t)gain * gain * energy;
	if (gain > 1u)
		CHECK(distance(selected, target) <=
			      distance(
				      (__uint128_t)(gain - 1u) *
					      (gain - 1u) * energy,
				      target),
		      "gain %u was farther than its lower neighbor", gain);
	if (gain < UINT16_MAX)
		CHECK(distance(selected, target) <=
			      distance(
				      (__uint128_t)(gain + 1u) *
					      (gain + 1u) * energy,
				      target),
		      "gain %u was farther than its upper neighbor", gain);
	return true;
}

static bool test_rate_and_law_matrix(void)
{
	for (unsigned int law = G711_LAW_PCMU;
	     law <= G711_LAW_PCMA; ++law) {
		for (uint8_t rate = 1u;
		     rate <= V92_CPD_MAX_DATA_RATE_NUMBER; ++rate) {
			struct v92_cpd_parameters cpd;
			struct v92_cpd_parameters decoded;
			uint8_t frame[V92_CPD_MAX_FRAME_BYTES];
			size_t frame_bits = 0u;

			CHECK(v92_cpd_direct_init(
				      &cpd, (enum g711_law)law, rate,
				      (rate & 1u) != 0u) == V92_CPD_OK &&
			      v92_cpd_parameters_are_valid(&cpd),
			      "rate %u law %u did not generate", rate, law);
			CHECK(cpd.data_rate_number == rate &&
			      cpd.extend_e2u == ((rate & 1u) != 0u) &&
			      !cpd.acknowledges_cpu &&
			      cpd.trellis ==
				      (rate == 13u ?
					       V92_CPD_TRELLIS_32_STATE :
					       V92_CPD_TRELLIS_16_STATE),
			      "rate %u law %u fixed selection changed",
			      rate, law);
			CHECK(direct_sections_are_correct(
				      &cpd, (enum g711_law)law, rate) &&
			      gain_is_nearest(&cpd, (enum g711_law)law),
			      "rate %u law %u direct mapping changed",
			      rate, law);
			memset(frame, 0xa5, sizeof(frame));
			CHECK(v92_cpd_encode(
				      &cpd, 17u, frame, sizeof(frame),
				      &frame_bits) == V92_CPD_OK &&
			      v92_cpd_decode(
				      frame, sizeof(frame), frame_bits, 17u,
				      &decoded) == V92_CPD_OK &&
			      memcmp(&decoded, &cpd, sizeof(cpd)) == 0,
			      "rate %u law %u CPd did not round trip",
			      rate, law);
		}
	}
	return true;
}

static bool test_known_boundaries(void)
{
	struct v92_cpd_parameters cpd;

	CHECK(v92_cpd_direct_init(
		      &cpd, G711_LAW_PCMU, 1u, false) == V92_CPD_OK &&
	      cpd.four_times_gain_q0_16 == 165u,
	      "DRN1 gain was %u rather than 165", cpd.four_times_gain_q0_16);
	CHECK(v92_cpd_direct_init(
		      &cpd, G711_LAW_PCMA, 4u, true) == V92_CPD_OK &&
	      cpd.four_times_gain_q0_16 == 108u &&
	      v92_cpd_frame_bits(&cpd, 17u) == 680u,
	      "DRN4 direct CPd changed gain or wire extent");
	CHECK(v92_cpd_direct_init(
		      &cpd, G711_LAW_PCMU, 13u, false) == V92_CPD_OK &&
	      cpd.trellis == V92_CPD_TRELLIS_32_STATE &&
	      cpd.constellation[0].count == 64u,
	      "DRN13 did not select its expanded 32-state mapping");
	return true;
}

static bool test_transactionality(void)
{
	struct v92_cpd_parameters cpd;
	struct v92_cpd_parameters before;

	memset(&cpd, 0x5a, sizeof(cpd));
	before = cpd;
	CHECK(v92_cpd_direct_init(
		      &cpd, G711_LAW_PCMU, 0u, false) ==
		      V92_CPD_INVALID_PARAMETERS &&
	      memcmp(&cpd, &before, sizeof(cpd)) == 0,
	      "cleardown changed the rejected output");
	CHECK(v92_cpd_direct_init(
		      &cpd, G711_LAW_PCMU,
		      V92_CPD_MAX_DATA_RATE_NUMBER + 1u, false) ==
		      V92_CPD_INVALID_PARAMETERS &&
	      memcmp(&cpd, &before, sizeof(cpd)) == 0,
	      "unsupported rate changed the rejected output");
	CHECK(v92_cpd_direct_init(
		      &cpd, (enum g711_law)99, 1u, false) ==
		      V92_CPD_INVALID_PARAMETERS &&
	      memcmp(&cpd, &before, sizeof(cpd)) == 0 &&
	      v92_cpd_direct_init(
		      NULL, G711_LAW_PCMU, 1u, false) ==
		      V92_CPD_INVALID_ARGUMENT,
	      "invalid arguments were accepted");
	return true;
}

int main(void)
{
	if (!test_rate_and_law_matrix() || !test_known_boundaries() ||
	    !test_transactionality())
		return 1;
	return 0;
}
