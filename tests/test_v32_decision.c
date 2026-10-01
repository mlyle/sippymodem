// SPDX-License-Identifier: BSD-3-Clause

#include <decision.h>

#include <constellation.h>

#include "test.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

static const uint8_t RAW_TO_SYMBOL[16] = {
	15, 14, 13, 12, 1, 3, 0, 2,
	10, 8, 11, 9, 4, 5, 6, 7,
};

static uint64_t hash_u16(uint64_t hash, uint16_t value)
{
	hash ^= value & 0xffu;
	hash *= UINT64_C(1099511628211);
	hash ^= value >> 8;
	return hash * UINT64_C(1099511628211);
}

static struct v32_equalizer_observation observation(int16_t i, int16_t q)
{
	return (struct v32_equalizer_observation){
		.i = i,
		.q = q,
		.phase = 0x1234,
		.magnitude = 0x2345,
		.epoch = UINT64_C(0x1122334455667788),
		.lineage = UINT64_C(0x8877665544332211),
	};
}

static bool equalizer_decision_is_complete(
	const struct v32_equalizer_decision *decision,
	const struct v32_equalizer_observation *expected,
	bool expected_adaptation)
{
	return memcmp(&decision->observation, expected, sizeof(*expected)) == 0 &&
	       decision->ideal_phase <= INT16_MAX &&
	       decision->ideal_magnitude <= INT16_MAX &&
	       decision->adapt_on_error == expected_adaptation;
}

static struct v32_fixed_point raw_point_16(uint8_t index)
{
	static const int16_t I[16] = {
		-12288, -12288, -4096, -4096,
		-12288, -12288, -4096, -4096,
		4096, 4096, 12288, 12288,
		4096, 4096, 12288, 12288,
	};
	static const int16_t Q[16] = {
		12288, 4096, 12288, 4096,
		-4096, -12288, -4096, -12288,
		12288, 4096, 12288, 4096,
		-4096, -12288, -4096, -12288,
	};

	return (struct v32_fixed_point){ I[index], Q[index] };
}

static bool test_fixed_api_contracts(void)
{
	static const int16_t BASE[16][2] = {
		{ -4096, -4096 }, { -12288, -4096 },
		{ -4096, -12288 }, { -12288, -12288 },
		{ 4096, -4096 }, { 4096, -12288 },
		{ 12288, -4096 }, { 12288, -12288 },
		{ 4096, 4096 }, { 12288, 4096 },
		{ 4096, 12288 }, { 12288, 12288 },
		{ -4096, 4096 }, { -4096, 12288 },
		{ -12288, 4096 }, { -12288, 12288 },
	};
	struct v32_fixed_point point;
	const struct v32_fixed_point sentinel = { 123, -456 };

	for (uint8_t symbol = 0; symbol < 16; ++symbol) {
		CHECK(v32_constellation_fixed_point(symbol, &point) ==
			      V32_CONSTELLATION_OK &&
		      point.i == BASE[symbol][0] && point.q == BASE[symbol][1],
		      "fixed base point %u changed", symbol);
	}
	point = sentinel;
	CHECK(v32_constellation_fixed_point(16, &point) ==
		      V32_CONSTELLATION_INVALID_ARGUMENT &&
	      memcmp(&point, &sentinel, sizeof(point)) == 0 &&
	      v32_constellation_fixed_point(0, NULL) ==
		      V32_CONSTELLATION_INVALID_ARGUMENT,
	      "fixed base rejection was not transactional");
	point = sentinel;
	CHECK(v32_4800_fixed_point(4, &point) ==
		      V32_CONSTELLATION_INVALID_ARGUMENT &&
	      memcmp(&point, &sentinel, sizeof(point)) == 0 &&
	      v32_4800_fixed_point(0, NULL) ==
		      V32_CONSTELLATION_INVALID_ARGUMENT,
	      "fixed 4800 rejection was not transactional");
	point = sentinel;
	CHECK(v32_trellis_fixed_signal_point(2, 0, &point) ==
		      V32_CONSTELLATION_INVALID_ARGUMENT &&
	      memcmp(&point, &sentinel, sizeof(point)) == 0 &&
	      v32_trellis_fixed_signal_point(7, 0, &point) ==
		      V32_CONSTELLATION_INVALID_ARGUMENT &&
	      memcmp(&point, &sentinel, sizeof(point)) == 0 &&
	      v32_trellis_fixed_signal_point(3, 16, &point) ==
		      V32_CONSTELLATION_INVALID_ARGUMENT &&
	      memcmp(&point, &sentinel, sizeof(point)) == 0 &&
	      v32_trellis_fixed_signal_point(3, 0, NULL) ==
		      V32_CONSTELLATION_INVALID_ARGUMENT,
	      "fixed trellis rejection was not transactional");
	return true;
}

static bool test_fixed_geometry_and_small_slicers(void)
{
	uint64_t four_hash = UINT64_C(14695981039346656037);
	uint64_t uncoded_hash = UINT64_C(14695981039346656037);

	for (uint8_t raw = 0; raw < 4; ++raw) {
		uint8_t quadrant = (uint8_t)((raw + 3u) & 3u);
		struct v32_fixed_point point;
		struct v32_equalizer_observation sample;
		struct v32_quadrant_decision decision;

		CHECK(v32_4800_fixed_point(quadrant, &point) ==
			      V32_CONSTELLATION_OK,
		      "4800 fixed point %u did not generate", quadrant);
		sample = observation(point.i, point.q);
		CHECK(v32_decide_known_quadrant(
			      &sample, quadrant, true, &decision) ==
			      V32_DECISION_OK &&
		      decision.quadrant == quadrant &&
		      equalizer_decision_is_complete(
			      &decision.equalizer, &sample, true),
		      "known quadrant %u changed", quadrant);
		four_hash = hash_u16(four_hash, (uint16_t)point.i);
		four_hash = hash_u16(four_hash, (uint16_t)point.q);
		four_hash = hash_u16(
			four_hash, decision.equalizer.ideal_phase);
		four_hash = hash_u16(
			four_hash, decision.equalizer.ideal_magnitude);
	}
	CHECK(four_hash == UINT64_C(0xf2dd4557bd36ae36),
	      "four-point oracle digest was %016llx",
	      (unsigned long long)four_hash);

	for (uint8_t raw = 0; raw < 16; ++raw) {
		struct v32_fixed_point point = raw_point_16(raw);
		struct v32_equalizer_observation sample =
			observation(point.i, point.q);
		struct v32_uncoded_decision decision;

		CHECK(v32_decide_uncoded_16(
			      &sample, false, &decision) == V32_DECISION_OK &&
		      decision.symbol == RAW_TO_SYMBOL[raw] &&
		      equalizer_decision_is_complete(
			      &decision.equalizer, &sample, false),
		      "uncoded decision point %u changed", raw);
		int64_t magnitude = decision.equalizer.ideal_magnitude;
		int64_t radius_error = magnitude * magnitude -
			((int64_t)point.i * point.i + (int64_t)point.q * point.q);

		CHECK(magnitude > 0 && radius_error >= -2 * magnitude &&
		      radius_error <= 2 * magnitude,
		      "uncoded point %u did not retain its constellation radius", raw);
		uncoded_hash = hash_u16(uncoded_hash, (uint16_t)point.i);
		uncoded_hash = hash_u16(uncoded_hash, (uint16_t)point.q);
		uncoded_hash = hash_u16(
			uncoded_hash, decision.equalizer.ideal_phase);
		uncoded_hash = hash_u16(
			uncoded_hash, decision.equalizer.ideal_magnitude);
	}
	CHECK(uncoded_hash == UINT64_C(0x5fd76a6541cc1ab7),
	      "uncoded oracle digest was %016llx",
	      (unsigned long long)uncoded_hash);
	return true;
}

static bool test_nearest_ties(void)
{
	static const int16_t samples[] = {
		INT16_MIN, -12288, -8193, -8192, -8191, -4096, -1, 0,
		1, 4096, 8191, 8192, 8193, 12288, INT16_MAX,
	};
	struct v32_equalizer_observation sample = observation(-8192, 4096);
	struct v32_quadrant_decision four;
	struct v32_uncoded_decision uncoded;

	CHECK(v32_decide_4_point(&sample, false, &four) ==
		      V32_DECISION_OK && four.quadrant == 3,
	      "four-point tie did not retain the first decision point");
	/* An independent distance search over the fixed geometry checks all
	 * decision edges and intersections, and the full signed input bounds. */
	for (size_t i = 0; i < ARRAY_LENGTH(samples); ++i) {
		for (size_t q = 0; q < ARRAY_LENGTH(samples); ++q) {
			uint64_t best_distance = UINT64_MAX;
			uint8_t expected = 0;

			for (uint8_t raw = 0; raw < 16; ++raw) {
				struct v32_fixed_point point = raw_point_16(raw);
				int64_t di = (int64_t)samples[i] - point.i;
				int64_t dq = (int64_t)samples[q] - point.q;
				uint64_t distance = (uint64_t)(di * di + dq * dq);

				if (distance < best_distance) {
					best_distance = distance;
					expected = RAW_TO_SYMBOL[raw];
				}
			}
			sample = observation(samples[i], samples[q]);
			CHECK(v32_decide_uncoded_16(&sample, true, &uncoded) ==
				      V32_DECISION_OK && uncoded.symbol == expected,
			      "uncoded nearest point changed at %d/%d",
			      samples[i], samples[q]);
		}
	}
	return true;
}

static bool test_small_contracts(void)
{
	struct v32_equalizer_observation sample = observation(4096, -14336);
	struct v32_equalizer_observation invalid = sample;
	struct v32_quadrant_decision quadrant;
	struct v32_quadrant_decision quadrant_unchanged;
	struct v32_uncoded_decision uncoded;
	struct v32_uncoded_decision uncoded_unchanged;
	union {
		struct v32_equalizer_observation observation;
		struct v32_quadrant_decision decision;
	} quadrant_overlap;
	union {
		struct v32_equalizer_observation observation;
		struct v32_uncoded_decision decision;
	} uncoded_overlap;

	memset(&quadrant, 0xa5, sizeof(quadrant));
	quadrant_unchanged = quadrant;
	CHECK(v32_decide_known_quadrant(&sample, 4, false, &quadrant) ==
		      V32_DECISION_INVALID_ARGUMENT &&
	      memcmp(&quadrant, &quadrant_unchanged, sizeof(quadrant)) == 0 &&
	      v32_decide_known_quadrant(&sample, 0, false, NULL) ==
		      V32_DECISION_INVALID_ARGUMENT &&
	      v32_decide_4_point(NULL, false, &quadrant) ==
		      V32_DECISION_INVALID_ARGUMENT &&
	      memcmp(&quadrant, &quadrant_unchanged, sizeof(quadrant)) == 0,
	      "small-slicer invalid arguments were accepted");
	memset(&uncoded, 0x3c, sizeof(uncoded));
	uncoded_unchanged = uncoded;
	CHECK(v32_decide_uncoded_16(NULL, false, &uncoded) ==
		      V32_DECISION_INVALID_ARGUMENT &&
	      memcmp(&uncoded, &uncoded_unchanged, sizeof(uncoded)) == 0,
	      "uncoded invalid argument mutated output");
	invalid.phase = UINT16_C(0x8000);
	CHECK(v32_decide_uncoded_16(&invalid, false, &uncoded) ==
		      V32_DECISION_INVALID_ARGUMENT &&
	      memcmp(&uncoded, &uncoded_unchanged, sizeof(uncoded)) == 0,
	      "out-of-domain observation phase was accepted");
	invalid = sample;
	invalid.magnitude = UINT16_C(0x8000);
	CHECK(v32_decide_4_point(&invalid, false, &quadrant) ==
		      V32_DECISION_INVALID_ARGUMENT &&
	      memcmp(&quadrant, &quadrant_unchanged, sizeof(quadrant)) == 0,
	      "out-of-domain observation magnitude was accepted");
	quadrant_overlap.observation = sample;
	CHECK(v32_decide_known_quadrant(
		      &quadrant_overlap.observation, 0, false,
		      &quadrant_overlap.decision) == V32_DECISION_INVALID_ARGUMENT &&
	      memcmp(&quadrant_overlap.observation, &sample, sizeof(sample)) == 0,
	      "known-quadrant overlapping output mutated input");
	quadrant_overlap.observation = sample;
	CHECK(v32_decide_4_point(
		      &quadrant_overlap.observation, false,
		      &quadrant_overlap.decision) == V32_DECISION_INVALID_ARGUMENT &&
	      memcmp(&quadrant_overlap.observation, &sample, sizeof(sample)) == 0,
	      "four-point overlapping output mutated input");
	uncoded_overlap.observation = sample;
	CHECK(v32_decide_uncoded_16(
		      &uncoded_overlap.observation, false,
		      &uncoded_overlap.decision) == V32_DECISION_INVALID_ARGUMENT &&
	      memcmp(&uncoded_overlap.observation, &sample, sizeof(sample)) == 0,
	      "uncoded overlapping output mutated input");
	return true;
}

static uint64_t tcm_map_hash(uint8_t data_bits)
{
	uint8_t count = (uint8_t)(1u << (data_bits + 1u));
	uint64_t hash = UINT64_C(14695981039346656037);

	for (uint8_t symbol = 0; symbol < count; ++symbol) {
		struct v32_fixed_point point;
		struct v32_equalizer_observation sample;
		struct v32_tcm_decision decision;

		if (v32_trellis_fixed_signal_point(
			    data_bits, symbol, &point) != V32_CONSTELLATION_OK)
			return 0;
		sample = observation(point.i, point.q);
		if (v32_decide_tcm(
			    &sample, data_bits, true, &decision) != V32_DECISION_OK ||
		    !equalizer_decision_is_complete(
			    &decision.equalizer, &sample, true))
			return 0;
		hash = hash_u16(hash, (uint16_t)point.i);
		hash = hash_u16(hash, (uint16_t)point.q);
		hash = hash_u16(hash, decision.feedback_point);
		hash = hash_u16(hash, decision.equalizer.ideal_phase);
		hash = hash_u16(hash, decision.equalizer.ideal_magnitude);
	}
	return hash;
}

static void add_grid_value(
	int16_t values[128], size_t *count, int32_t value)
{
	if (value < INT16_MIN || value > INT16_MAX)
		return;
	for (size_t index = 0; index < *count; ++index)
		if (values[index] == (int16_t)value)
			return;
	values[(*count)++] = (int16_t)value;
}

static size_t build_boundary_grid(int16_t values[128])
{
	static const int32_t BOUNDARIES[] = {
		0x0000, 0x0800, 0x1000, 0x169f, 0x16a0, 0x16a1,
		0x2000, 0x21f1, 0x2799, 0x2d40, 0x2d41, 0x2d42,
		0x32e9, 0x3890, 0x3891, 0x3892, 0x3893, 0x3e39,
		0x3e3a,
	};
	size_t count = 0;

	add_grid_value(values, &count, INT16_MIN);
	add_grid_value(values, &count, INT16_MAX);
	for (size_t index = 0; index < ARRAY_LENGTH(BOUNDARIES); ++index) {
		for (int sign = -1; sign <= 1; sign += 2) {
			int32_t base = BOUNDARIES[index] * sign;

			for (int delta = -1; delta <= 1; ++delta)
				add_grid_value(values, &count, base + delta);
		}
	}
	return count;
}

static uint32_t random_word(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static uint64_t tcm_quantizer_hash(uint8_t data_bits)
{
	int16_t grid[128];
	size_t grid_count = build_boundary_grid(grid);
	uint32_t random = UINT32_C(0x17c032a5) ^
		(UINT32_C(0x9e3779b9) * (uint32_t)(data_bits - 2u));
	uint64_t hash = UINT64_C(14695981039346656037);
	uint8_t signal_points = (uint8_t)(1u << (data_bits + 1u));

	for (uint8_t symbol = 0; symbol < signal_points; ++symbol) {
		struct v32_fixed_point point;
		struct v32_equalizer_observation sample;
		struct v32_tcm_decision decision;

		if (v32_trellis_fixed_signal_point(
			    data_bits, symbol, &point) != V32_CONSTELLATION_OK)
			return 0;
		sample = observation(point.i, point.q);
		if (v32_decide_tcm(
			    &sample, data_bits, false, &decision) != V32_DECISION_OK)
			return 0;
		hash = hash_u16(hash, decision.feedback_point);
		hash = hash_u16(hash, decision.equalizer.ideal_phase);
		hash = hash_u16(hash, decision.equalizer.ideal_magnitude);
	}
	for (size_t i = 0; i < grid_count; ++i) {
		for (size_t q = 0; q < grid_count; ++q) {
			struct v32_equalizer_observation sample =
				observation(grid[i], grid[q]);
			struct v32_tcm_decision decision;

			if (v32_decide_tcm(
				    &sample, data_bits, false, &decision) !=
			    V32_DECISION_OK)
				return 0;
			hash = hash_u16(hash, decision.feedback_point);
			hash = hash_u16(hash, decision.equalizer.ideal_phase);
			hash = hash_u16(hash, decision.equalizer.ideal_magnitude);
		}
	}
	for (unsigned int index = 0; index < 4096; ++index) {
		int16_t i = (int16_t)(random_word(&random) >> 16);
		int16_t q = (int16_t)(random_word(&random) >> 16);
		struct v32_equalizer_observation sample = observation(i, q);
		struct v32_tcm_decision decision;

		if (v32_decide_tcm(
			    &sample, data_bits, false, &decision) != V32_DECISION_OK)
			return 0;
		hash = hash_u16(hash, decision.feedback_point);
		hash = hash_u16(hash, decision.equalizer.ideal_phase);
		hash = hash_u16(hash, decision.equalizer.ideal_magnitude);
	}
	return hash;
}

static bool test_tcm_oracles(void)
{
	static const struct {
		uint8_t data_bits;
		int16_t i;
		int16_t q;
		uint8_t feedback;
		uint16_t phase;
		uint16_t magnitude;
	} AXES[] = {
		{ 4, 0, 4096, 2, 0x1fff, 0x1000 },
		{ 4, -4096, 0, 10, 0x4000, 0x1000 },
		{ 4, 0, -4096, 18, 0x6000, 0x1000 },
		{ 4, 4096, 0, 26, 0x0000, 0x1000 },
		{ 6, 0, 2048, 9, 0x1fff, 0x0800 },
		{ 6, 0, -2048, 73, 0x6000, 0x0800 },
	};
	static const uint64_t MAP_HASH[4] = {
		UINT64_C(0x2fec165460e63253), UINT64_C(0xfbe796323edfdaf1),
		UINT64_C(0x126df5315cab590d), UINT64_C(0xe73440af3eab6abe),
	};
	static const uint64_t QUANTIZER_HASH[4] = {
		UINT64_C(0xaea89273641920e8), UINT64_C(0x403c3c8f234acf1f),
		UINT64_C(0xdd514ab2ca200940), UINT64_C(0xfa7093209973c2ad),
	};

	for (size_t index = 0; index < ARRAY_LENGTH(AXES); ++index) {
		struct v32_equalizer_observation sample =
			observation(AXES[index].i, AXES[index].q);
		struct v32_tcm_decision decision;

		CHECK(v32_decide_tcm(
			      &sample, AXES[index].data_bits, false, &decision) ==
			      V32_DECISION_OK &&
		      decision.feedback_point == AXES[index].feedback &&
		      decision.equalizer.ideal_phase == AXES[index].phase &&
		      decision.equalizer.ideal_magnitude ==
			      AXES[index].magnitude,
		      "TCM axis case %zu changed", index);
	}
	for (uint8_t data_bits = 3; data_bits <= 6; ++data_bits) {
		uint64_t map = tcm_map_hash(data_bits);
		uint64_t quantizer = tcm_quantizer_hash(data_bits);

		CHECK(map == MAP_HASH[data_bits - 3u],
		      "%u-bit TCM map digest was %016llx", data_bits,
		      (unsigned long long)map);
		CHECK(quantizer == QUANTIZER_HASH[data_bits - 3u],
		      "%u-bit TCM quantizer digest was %016llx", data_bits,
		      (unsigned long long)quantizer);
	}
	return true;
}

static bool test_tcm_contracts(void)
{
	struct v32_equalizer_observation sample = observation(4096, -14336);
	struct v32_equalizer_observation invalid = sample;
	struct v32_tcm_decision tcm;
	struct v32_tcm_decision unchanged;
	union {
		struct v32_equalizer_observation observation;
		struct v32_tcm_decision decision;
	} overlap;

	memset(&tcm, 0x5a, sizeof(tcm));
	unchanged = tcm;
	CHECK(v32_decide_tcm(NULL, 6, false, &tcm) ==
		      V32_DECISION_INVALID_ARGUMENT &&
	      memcmp(&tcm, &unchanged, sizeof(tcm)) == 0 &&
	      v32_decide_tcm(&sample, 2, false, &tcm) ==
		      V32_DECISION_INVALID_ARGUMENT &&
	      memcmp(&tcm, &unchanged, sizeof(tcm)) == 0 &&
	      v32_decide_tcm(&sample, 7, false, &tcm) ==
		      V32_DECISION_INVALID_ARGUMENT &&
	      memcmp(&tcm, &unchanged, sizeof(tcm)) == 0 &&
	      v32_decide_tcm(&sample, 6, false, NULL) ==
		      V32_DECISION_INVALID_ARGUMENT,
	      "TCM null/rate rejection was not transactional");
	invalid.epoch = 0;
	CHECK(v32_decide_tcm(&invalid, 6, false, &tcm) ==
		      V32_DECISION_INVALID_ARGUMENT &&
	      memcmp(&tcm, &unchanged, sizeof(tcm)) == 0,
	      "zero-epoch observation was accepted");
	overlap.observation = sample;
	CHECK(v32_decide_tcm(
		      &overlap.observation, 6, false, &overlap.decision) ==
		      V32_DECISION_INVALID_ARGUMENT &&
	      memcmp(&overlap.observation, &sample, sizeof(sample)) == 0,
	      "overlapping TCM output mutated the observation");
	CHECK(v32_decide_tcm(&sample, 6, true, &tcm) == V32_DECISION_OK &&
		      tcm.feedback_point == 77 &&
		      tcm.equalizer.ideal_magnitude == 0x3a3e &&
		      equalizer_decision_is_complete(
			      &tcm.equalizer, &sample, true),
	      "128-point parity target changed");
	return true;
}

int main(void)
{
	if (!test_fixed_api_contracts() ||
	    !test_fixed_geometry_and_small_slicers() ||
	    !test_nearest_ties() || !test_small_contracts() ||
	    !test_tcm_oracles() || !test_tcm_contracts())
		return 1;
	puts("V.32 decision tests passed");
	return 0;
}
