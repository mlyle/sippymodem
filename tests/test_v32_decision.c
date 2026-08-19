// SPDX-License-Identifier: BSD-3-Clause

#include <decision.h>

#include <constellation.h>

#include "test.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

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
	static const uint8_t RAW_TO_SYMBOL[16] = {
		15, 14, 13, 12, 1, 3, 0, 2,
		10, 8, 11, 9, 4, 5, 6, 7,
	};
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
		uncoded_hash = hash_u16(uncoded_hash, (uint16_t)point.i);
		uncoded_hash = hash_u16(uncoded_hash, (uint16_t)point.q);
		uncoded_hash = hash_u16(
			uncoded_hash, decision.equalizer.ideal_phase);
		uncoded_hash = hash_u16(
			uncoded_hash, decision.equalizer.ideal_magnitude);
	}
	CHECK(uncoded_hash == UINT64_C(0xa40013ae2f6bfbdb),
	      "uncoded oracle digest was %016llx",
	      (unsigned long long)uncoded_hash);
	return true;
}

static bool test_nearest_ties(void)
{
	struct v32_equalizer_observation sample = observation(-8192, 4096);
	struct v32_quadrant_decision four;
	struct v32_uncoded_decision uncoded;

	CHECK(v32_decide_4_point(&sample, false, &four) ==
		      V32_DECISION_OK && four.quadrant == 3,
	      "four-point tie did not retain the first decision point");
	sample = observation(-12288, 8192);
	CHECK(v32_decide_uncoded_16(&sample, false, &uncoded) ==
		      V32_DECISION_OK && uncoded.symbol == 15,
	      "uncoded tie did not retain the first decision point");
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

int main(void)
{
	if (!test_fixed_api_contracts() ||
	    !test_fixed_geometry_and_small_slicers() ||
	    !test_nearest_ties() || !test_small_contracts())
		return 1;
	puts("V.32 decision tests passed");
	return 0;
}
