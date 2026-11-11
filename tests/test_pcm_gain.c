// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>
#include <pcm_gain.h>

#include "test.h"

#include <limits.h>
#include <math.h>
#include <string.h>

static int16_t host_sample(int16_t wire, double gain)
{
	double value = round(wire * gain);
	return (int16_t)fmax(INT16_MIN, fmin(INT16_MAX, value));
}

static bool check_estimate(const struct pcm_gain *gain, double actual)
{
	struct pcm_gain saved = *gain;
	double estimate = -1.;

	CHECK(pcm_gain_is_valid(gain) && gain->lower <= actual && actual <= gain->upper,
	      "gain %.17g outside [%.17g,%.17g]", actual, gain->lower, gain->upper);
	CHECK(pcm_gain_estimate(gain, &estimate) &&
	      estimate >= gain->lower && estimate <= gain->upper,
	      "no estimate inside the surviving interval");
	CHECK(memcmp(gain, &saved, sizeof(saved)) == 0,
	      "estimation changed accumulated state");
	return true;
}

static bool test_input_space(void)
{
	static const double scales[] = {
		.00002, .02, .125, .75, 1., 4., 16.39453125, 512.,
	};

	for (size_t i = 0; i < ARRAY_LENGTH(scales); i++) {
		struct pcm_gain gain;
		pcm_gain_init(&gain);
		for (int32_t wire = INT16_MIN; wire <= INT16_MAX; wire++) {
			int16_t sample = host_sample((int16_t)wire, scales[i]);
			CHECK(pcm_gain_contains(&gain, (int16_t)wire, sample) &&
			      pcm_gain_observe(&gain, (int16_t)wire, sample),
			      "gain %.17g rejected linear sample %d", scales[i], (int)wire);
		}
		CHECK(check_estimate(&gain, scales[i]), "linear gain %zu", i);
		/* Interleave two independent law-specific streams. Include both
		 * PCMU zero octets, using their decoded level, not a synthetic -1. */
		struct pcm_gain codec[2];
		pcm_gain_init(codec);
		pcm_gain_init(codec + 1);
		for (unsigned code = 0; code <= UINT8_MAX; code++) {
			int16_t wire[2] = {
				g711_decode_pcmu((uint8_t)code),
				g711_decode_pcma((uint8_t)code),
			};
			for (unsigned law = 0; law < 2; law++) {
				CHECK(pcm_gain_observe(codec + law, wire[law],
					      host_sample(wire[law], scales[i])),
				      "gain %.17g law %u code %u", scales[i], law, code);
			}
		}
		CHECK(check_estimate(codec, scales[i]) &&
		      check_estimate(codec + 1, scales[i]), "codec gain %zu", i);
	}
	return true;
}

static bool test_clipping_and_ties(void)
{
	struct pcm_gain gain;
	pcm_gain_init(&gain);
	double estimate = -1.;

	CHECK(pcm_gain_observe(&gain, 100, INT16_MAX) &&
	      gain.lower == (double)327.665 && isinf(gain.upper) &&
	      gain.energy == 0. && gain.correlation == 0., "positive clipping");
	CHECK(pcm_gain_observe(&gain, -100, INT16_MIN) &&
	      gain.lower == (double)327.675 && isinf(gain.upper) &&
	      gain.energy == 0. && gain.correlation == 0., "negative clipping");
	CHECK(!pcm_gain_estimate(&gain, &estimate) && estimate == -1.,
	      "all-clipped input invented an estimate");
	CHECK(pcm_gain_observe(&gain, 1, 328) &&
	      pcm_gain_estimate(&gain, &estimate) && estimate == 328.,
	      "uncensored evidence did not resolve clipping");

	pcm_gain_init(&gain);
	CHECK(pcm_gain_observe(&gain, 2, 1) && gain.lower == .25 && gain.upper == .75 &&
	      pcm_gain_observe(&gain, 2, 2) && gain.lower == .75 && gain.upper == .75,
	      "half-way ties were not conservatively included");
	CHECK(pcm_gain_estimate(&gain, &estimate) && estimate == .75,
	      "point interval estimate");
	/* The outer bounds intersect even though one chosen tie rule cannot
	 * generate both observations: do not treat estimation as admission. */
	CHECK(host_sample(2, estimate) == 2, "tie fixture no longer distinguishes bounds");
	return true;
}

static bool test_frozen_bounds(void)
{
	const struct pcm_gain gain = {
		.lower = .5, .upper = 1.5, .correlation = 7500., .energy = 10000.,
	};
	struct pcm_gain saved = gain;
	static const struct { int16_t wire, sample; bool compatible; } pairs[] = {
		{ 2, 1, true }, { 2, 3, true }, { 2, 0, false }, { 2, 4, false },
		{ -2, -1, true }, { -2, -3, true }, { -2, 0, false }, { -2, -4, false },
		{ 0, 0, true }, { 0, 1, false }, { 0, -1, false },
		{ INT16_MAX, INT16_MAX, true }, { INT16_MIN, INT16_MIN, true },
		{ INT16_MAX, INT16_MIN, false }, { INT16_MIN, INT16_MAX, false },
	};
	for (size_t i = 0; i < ARRAY_LENGTH(pairs); ++i) {
		CHECK(pcm_gain_contains(&gain, pairs[i].wire, pairs[i].sample) ==
		      pairs[i].compatible && memcmp(&gain, &saved, sizeof(gain)) == 0,
		      "held-out pair %zu refitted or escaped the frozen bounds", i);
	}
	const struct pcm_gain tie = { .lower = .75, .upper = .75 };
	CHECK(pcm_gain_contains(&tie, 2, 1) && pcm_gain_contains(&tie, 2, 2) &&
	      !pcm_gain_contains(&tie, 2, 0), "frozen half-way ties changed");
	return true;
}

static bool test_rejections(void)
{
	struct pcm_gain gain;
	pcm_gain_init(&gain);
	double estimate = -1.;
	CHECK(!pcm_gain_estimate(&gain, &estimate) && estimate == -1.,
	      "empty training invented an estimate");
	CHECK(pcm_gain_observe(&gain, 0, 0) &&
	      !pcm_gain_estimate(&gain, &estimate), "silence calibrated a gain");
	CHECK(pcm_gain_observe(&gain, 10, 0) &&
	      !pcm_gain_estimate(&gain, &estimate), "zero correlation calibrated a gain");
	pcm_gain_init(&gain);
	CHECK(pcm_gain_observe(&gain, 100, 75), "valid fixture");
	struct pcm_gain saved = gain;
	static const int16_t bad[][2] = { { 0, 1 }, { 100, -75 }, { 100, 200 } };
	for (size_t i = 0; i < ARRAY_LENGTH(bad); i++) {
		CHECK(!pcm_gain_observe(&gain, bad[i][0], bad[i][1]) &&
		      memcmp(&gain, &saved, sizeof(gain)) == 0,
		      "rejected pair %zu changed state", i);
	}
	CHECK(!pcm_gain_is_valid(NULL) && !pcm_gain_observe(NULL, 0, 0) &&
	      !pcm_gain_contains(NULL, 0, 0) &&
	      !pcm_gain_estimate(NULL, &estimate) && estimate == -1. &&
	      !pcm_gain_estimate(&gain, NULL), "NULL arguments");
	CHECK(!pcm_gain_estimate(&gain, &gain.lower) &&
	      !pcm_gain_estimate(&gain, &gain.energy) &&
	      memcmp(&gain, &saved, sizeof(gain)) == 0, "aliased estimate mutated state");
	const struct pcm_gain invalid[] = {
		{ 0 }, { NAN, 1., 1., 1. }, { 0., NAN, 1., 1. },
		{ 0., 1., NAN, 1. }, { 0., 1., 1., NAN },
		{ -1., 1., 1., 1. }, { 2., 1., 1., 1. },
		{ 0., 1., -1., 1. }, { 0., 1., 1., -1. },
		{ INFINITY, INFINITY, 1., 1. }, { 0., 1., INFINITY, 1. },
		{ 0., 1., 1., INFINITY },
	};
	for (size_t i = 0; i < ARRAY_LENGTH(invalid); i++) {
		gain = invalid[i];
		saved = gain;
		CHECK(!pcm_gain_is_valid(&gain) && !pcm_gain_observe(&gain, 1, 1) &&
		      !pcm_gain_contains(&gain, 1, 1) &&
		      !pcm_gain_estimate(&gain, &estimate) && estimate == -1. &&
		      memcmp(&gain, &saved, sizeof(gain)) == 0,
		      "invalid state %zu was accepted or changed", i);
	}
	pcm_gain_init(&gain);
	CHECK(pcm_gain_is_valid(&gain) && gain.lower == 0. && isinf(gain.upper) && gain.upper > 0. &&
	      gain.correlation == 0. && gain.energy == 0., "reset retained training");
	pcm_gain_init(NULL);
	return true;
}

static bool test_estimate_clamping(void)
{
	/* Repeated low-level rounding can bias the point estimate outside a
	 * later, narrower interval. Both the bounds and all observations count. */
	for (unsigned upper = 0; upper < 2; upper++) {
		struct pcm_gain gain;
		int16_t wire = upper ? 1 : 2;
		pcm_gain_init(&gain);
		for (unsigned i = 0; i < 1000; i++)
			CHECK(pcm_gain_observe(&gain, wire, 1), "low-level observation");
		CHECK(pcm_gain_observe(&gain, 100, 75), "narrow interval");
		double estimate;
		CHECK(pcm_gain_estimate(&gain, &estimate) &&
		      estimate == (upper ? gain.upper : gain.lower), "estimate escaped bounds");
		CHECK(gain.energy == 10000. + 1000. * wire * wire &&
		      gain.correlation == 7500. + 1000. * wire,
		      "bounds discarded accumulated least-squares evidence");
	}
	return true;
}

static bool test_observed_cells(void)
{
	static const double scales[] = { .125, .5, .75, 1., 4., 16.39453125 };
	for (unsigned law = 0; law < 2; ++law) {
		struct g711_interval cells[256];
		for (size_t i = 0; i < ARRAY_LENGTH(scales); ++i) {
			struct pcm_gain gain = {
				.lower = scales[i], .upper = scales[i],
				.correlation = 75., .energy = 100.,
			};
			struct pcm_gain saved = gain;
			for (unsigned code = 0; code < 256; ++code) {
				int16_t sample = host_sample(g711_decode(law, code), scales[i]);
				struct pcm_gain_interval cell, inverted, continuous, inverted_continuous;
				CHECK(pcm_gain_observed_cell(&gain, law, code, sample, 1, false, &cell),
				      "aligned pair rejected: law %u gain %g code %u", law, scales[i], code);
				cells[code] = (struct g711_interval){ cell.lower, cell.upper };
				CHECK(g711_encode(law, cells[code].lower) == code &&
				      g711_encode(law, cells[code].upper) == code &&
				      (cells[code].lower == INT16_MIN ||
				       g711_encode(law, cells[code].lower - 1) != code) &&
				      (cells[code].upper == INT16_MAX ||
				       g711_encode(law, cells[code].upper + 1) != code),
				      "cell is not the exact encoder preimage: law %u code %u", law, code);
				CHECK(pcm_gain_observed_cell(&gain, law, code, sample, -1, false, &inverted) &&
				      inverted.lower == -cell.upper && inverted.upper == -cell.lower &&
				      pcm_gain_observed_cell(&gain, law, code, sample, 1, true, &continuous) &&
				      continuous.lower == (cell.lower == INT16_MIN ? -INFINITY : cell.lower - .5) &&
				      continuous.upper == (cell.upper == INT16_MAX ? INFINITY : cell.upper + .5) &&
				      pcm_gain_observed_cell(&gain, law, code, sample, -1, true, &inverted_continuous) &&
				      inverted_continuous.lower == -continuous.upper &&
				      inverted_continuous.upper == -continuous.lower,
				      "polarity or continuous bounds changed codec cell: law %u code %u", law, code);
			}
			CHECK(memcmp(&gain, &saved, sizeof(gain)) == 0,
			      "observed cells narrowed or refitted frozen gain");
		}
		for (int32_t x = INT16_MIN; x <= INT16_MAX; ++x) {
			uint8_t code = g711_encode(law, (int16_t)x);
			CHECK(cells[code].lower <= x && x <= cells[code].upper,
			      "encoder input %d escaped its returned cell", (int)x);
		}
		if (law == G711_LAW_PCMU)
			CHECK(cells[0x7f].lower == -3 && cells[0x7f].upper == -1 &&
			      cells[0xff].lower == 0 && cells[0xff].upper == 3,
			      "signed-zero wire cells were merged");
	}
	struct pcm_gain unknown;
	struct pcm_gain_interval zero;
	double estimate = -1.;
	pcm_gain_init(&unknown);
	CHECK(pcm_gain_observed_cell(&unknown, G711_LAW_PCMU, 0x7f, 0, 1, false, &zero) &&
	      zero.lower == -3 && zero.upper == -1 &&
	      !pcm_gain_estimate(&unknown, &estimate) && estimate == -1. &&
	      unknown.lower == 0. && isinf(unknown.upper) && unknown.energy == 0. &&
	      unknown.correlation == 0., "a compatible zero invented gain evidence");
	return true;
}

static bool test_observed_cell_rejections(void)
{
	struct pcm_gain gain = { .lower = .75, .upper = .75 };
	struct pcm_gain saved = gain;
	struct pcm_gain_interval cell = { 1234, 5678 };
	CHECK(!pcm_gain_observed_cell(NULL, G711_LAW_PCMU, 0xff, 0, 1, false, &cell) &&
	      !pcm_gain_observed_cell(&gain, (enum g711_law)-1, 0xff, 0, 1, false, &cell) &&
	      !pcm_gain_observed_cell(&gain, G711_LAW_PCMA + 1, 0xff, 0, 1, false, &cell) &&
	      !pcm_gain_observed_cell(&gain, G711_LAW_PCMU, 0xff, 1, 1, false, &cell) &&
	      !pcm_gain_observed_cell(&gain, G711_LAW_PCMU, 0xff, 0, 0, false, &cell) &&
	      !pcm_gain_observed_cell(&gain, G711_LAW_PCMU, 0xff, 0, 2, true, &cell) &&
	      !pcm_gain_observed_cell(&gain, G711_LAW_PCMU, 0xff, 0, 1, false, NULL) &&
	      !pcm_gain_observed_cell(&gain, G711_LAW_PCMU, 0xff, 0, 1, false,
		      (struct pcm_gain_interval *)(void *)&gain.lower) &&
	      cell.lower == 1234 && cell.upper == 5678 &&
	      memcmp(&gain, &saved, sizeof(gain)) == 0,
	      "rejected cell changed output or gain owner");
	gain.lower = NAN;
	CHECK(!pcm_gain_observed_cell(&gain, G711_LAW_PCMU, 0xff, 0, 1, false, &cell) &&
	      cell.lower == 1234 && cell.upper == 5678 && isnan(gain.lower),
	      "invalid gain produced a cell or changed outputs");
	return true;
}

int main(void)
{
	return test_input_space() && test_clipping_and_ties() &&
		test_frozen_bounds() && test_estimate_clamping() && test_rejections() &&
		test_observed_cells() && test_observed_cell_rejections() ? 0 : 1;
}
