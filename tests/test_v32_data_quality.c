// SPDX-License-Identifier: BSD-3-Clause

#include <data_quality.h>

#include "test.h"

#include <limits.h>
#include <string.h>

static bool windows(struct v32_data_quality *quality, int16_t error, unsigned count)
{
	while (count-- != 0)
		if (v32_data_quality_observe(quality, error, V32_QUALITY_WINDOW_SYMBOLS))
			return true;
	return false;
}

static bool test_duration_and_rates(void)
{
	const unsigned rates[] = { 4800, 7200, 9600, 12000, 14400 };
	uint16_t previous = UINT16_MAX;

	for (unsigned rate = 0; rate < ARRAY_LENGTH(rates); ++rate) {
		struct v32_data_quality quality = { 0 };
		CHECK(v32_data_quality_start(&quality, rates[rate]) &&
		      quality.threshold < previous,
		      "rate %u has no stricter recovery limit", rates[rate]);
		previous = quality.threshold;
		CHECK(!windows(&quality, INT16_MAX, V32_QUALITY_WARMUP_WINDOWS) &&
		      quality.bad_windows == 0,
		      "startup settling consumed the bad-reception allowance");
		/* Polling without a new decision cannot consume the duration. */
		for (unsigned poll = 0; poll < 10000; ++poll)
			CHECK(!v32_data_quality_observe(&quality, INT16_MAX, 0),
			      "empty polling requested a retrain");
		CHECK(!windows(&quality, quality.threshold, V32_QUALITY_BAD_WINDOWS - 1),
		      "rate %u retrained before 1.8 seconds", rates[rate]);
		CHECK(!v32_data_quality_observe(&quality, quality.threshold,
			V32_QUALITY_WINDOW_SYMBOLS - 1),
		      "partial final window retrained early");
		CHECK(v32_data_quality_observe(&quality, quality.threshold, 1) &&
		      quality.automatic_retrains == 1,
		      "rate %u failed to recover sustained bad reception", rates[rate]);
	}
	return true;
}

static bool test_transients_and_window_average(void)
{
	struct v32_data_quality quality = { 0 };
	CHECK(v32_data_quality_start(&quality, 9600) &&
	      !windows(&quality, 0, V32_QUALITY_WARMUP_WINDOWS), "warmup failed");
	for (unsigned episode = 0; episode < 20; ++episode) {
		CHECK(!windows(&quality, INT16_MAX, V32_QUALITY_BAD_WINDOWS - 1) &&
		      !windows(&quality, quality.threshold - 1, 1) &&
		      quality.bad_windows == 0,
		      "separate transients accumulated into a retrain");
	}
	/* A narrow spike is averaged over the whole physical window. */
	for (unsigned window = 0; window < 50; ++window) {
		CHECK(!v32_data_quality_observe(&quality, INT16_MAX, 1) &&
		      !v32_data_quality_observe(&quality, 0, V32_QUALITY_WINDOW_SYMBOLS - 1) &&
		      quality.bad_windows == 0, "one-symbol spike requested recovery");
	}
	return true;
}

static bool test_recovery_budget_and_rearm(void)
{
	struct v32_data_quality quality = { 0 };
	for (unsigned attempt = 0; attempt < V32_QUALITY_MAX_RETRAINS; ++attempt) {
		CHECK(v32_data_quality_start(&quality, attempt ? 7200 : 9600) &&
		      quality.automatic_retrains == attempt &&
		      !windows(&quality, 0, V32_QUALITY_WARMUP_WINDOWS) &&
		      windows(&quality, INT16_MAX, V32_QUALITY_BAD_WINDOWS),
		      "retraining lost its call-owned recovery budget");
	}
	CHECK(v32_data_quality_start(&quality, 4800) &&
	      !windows(&quality, 0, V32_QUALITY_WARMUP_WINDOWS) &&
	      !windows(&quality, INT16_MAX, 100),
	      "persistent impairment caused an unlimited retrain loop");
	CHECK(!windows(&quality, 0, V32_QUALITY_REARM_WINDOWS - 1) &&
	      quality.automatic_retrains == V32_QUALITY_MAX_RETRAINS &&
	      !windows(&quality, INT16_MAX, 1) &&
	      !windows(&quality, 0, V32_QUALITY_REARM_WINDOWS - 1) &&
	      quality.automatic_retrains == V32_QUALITY_MAX_RETRAINS,
	      "interrupted clean reception rearmed recovery early");
	CHECK(!windows(&quality, 0, 1) && quality.automatic_retrains == 0 &&
	      windows(&quality, INT16_MAX, V32_QUALITY_BAD_WINDOWS),
	      "30 seconds of clean reception did not rearm later recovery");
	return true;
}

static bool test_chunking_and_invalid_input(void)
{
	struct v32_data_quality single = { 0 }, grouped = { 0 };
	CHECK(v32_data_quality_start(&single, 14400) &&
	      v32_data_quality_start(&grouped, 14400), "initialization failed");
	for (unsigned window = 0;
	     window < V32_QUALITY_WARMUP_WINDOWS + V32_QUALITY_BAD_WINDOWS; ++window) {
		bool single_retrain = false, grouped_retrain = false;
		for (unsigned symbol = 0; symbol < V32_QUALITY_WINDOW_SYMBOLS; ++symbol)
			single_retrain |= v32_data_quality_observe(&single, 5000, 1);
		for (unsigned group = 0; group < V32_QUALITY_WINDOW_SYMBOLS / 12; ++group)
			grouped_retrain |= v32_data_quality_observe(&grouped, 5000, 12);
		CHECK(single_retrain == grouped_retrain &&
		      memcmp(&single, &grouped, sizeof(single)) == 0,
		      "timing block size changed recovery at window %u", window);
	}
	struct v32_data_quality saved = grouped;
	CHECK(!v32_data_quality_start(&grouped, 12345) &&
	      !v32_data_quality_observe(&grouped, -1, 240) &&
	      memcmp(&saved, &grouped, sizeof(saved)) == 0 &&
	      !v32_data_quality_start(NULL, 9600) &&
	      !v32_data_quality_observe(NULL, 0, 1),
	      "invalid input mutated recovery state");
	return true;
}

int main(void)
{
	if (!test_duration_and_rates() || !test_transients_and_window_average() ||
	    !test_recovery_budget_and_rearm() || !test_chunking_and_invalid_input())
		return 1;
	puts("V.32 sustained receive-quality recovery passed");
	return 0;
}
