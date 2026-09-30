// SPDX-License-Identifier: BSD-3-Clause

#include <v92_upstream_channel.h>

#include "test.h"

#include <limits.h>
#include <string.h>

static int16_t expected(int index, bool full_scale)
{
	if (index < 0)
		return 0;
	if (full_scale)
		return INT16_MIN;
	uint32_t value = (uint32_t)index * UINT32_C(1664525) + UINT32_C(1013904223);

	value ^= value >> 13;
	value *= UINT32_C(22695477);
	return (int16_t)((int)(value >> 16) - 32768);
}

static int32_t observed(int index, bool full_scale)
{
	return full_scale ? 32768 : expected(index, false) / 2 + expected(index - 1, false) / 4;
}

static bool test_correlations(bool full_scale)
{
	struct v92_upstream_channel channel = { 0 };

	CHECK(v92_upstream_channel_is_valid(&channel), "zero channel did not initialize");
	for (int index = 0; index < V92_UPSTREAM_CHANNEL_SYMBOLS; ++index) {
		CHECK(v92_upstream_channel_sample(&channel, expected(index, full_scale),
			      observed(index, full_scale)) && channel.symbols == index + 1 &&
		      v92_upstream_channel_is_valid(&channel), "channel accumulation failed");
		if (index < V92_UPSTREAM_CHANNEL_SETTLE_SYMBOLS)
			CHECK(channel.observed_autocorrelation[0] == 0, "channel included settling symbols");
		for (int lag = 0; lag < V92_UPSTREAM_CHANNEL_MAX_LAG; ++lag) {
			bool measured = index >= V92_UPSTREAM_CHANNEL_SETTLE_SYMBOLS;
			int start = V92_UPSTREAM_CHANNEL_SETTLE_SYMBOLS - 1 - lag;

			CHECK(channel.observed_boundary[lag] == (measured ? observed(start, full_scale) : 0) &&
			      channel.expected_boundary[lag] == (measured ? expected(start, full_scale) : 0),
			      "measurement boundary moved at symbol %d lag %d", index, lag);
		}
	}
	/* Pin every short-window moment independently, including the partial
	 * final window and the excluded settling prefix. */
	for (unsigned w = 0; w < V92_UPSTREAM_CHANNEL_WINDOWS; ++w) {
		int64_t xx = 0, yy = 0, xy = 0;
		unsigned start = V92_UPSTREAM_CHANNEL_SETTLE_SYMBOLS + w * V92_UPSTREAM_CHANNEL_WINDOW_SYMBOLS;
		unsigned end = start + V92_UPSTREAM_CHANNEL_WINDOW_SYMBOLS;

		if (end > V92_UPSTREAM_CHANNEL_SYMBOLS) end = V92_UPSTREAM_CHANNEL_SYMBOLS;
		for (unsigned n = start; n < end; ++n) {
			int64_t x = expected(n, full_scale), y = observed(n, full_scale);

			xx += x * x;
			yy += y * y;
			xy += x * y;
		}
		CHECK(channel.window[w].expected_energy == xx && channel.window[w].observed_energy == yy &&
		      channel.window[w].cross == xy, "short-window %u changed", w);
	}
	/* Compute each complete sum independently, without the streaming history. */
	int64_t previous_energy = 0, second_energy = 0, previous_cross = 0;

	for (int n = V92_UPSTREAM_CHANNEL_SETTLE_SYMBOLS; n < V92_UPSTREAM_CHANNEL_SYMBOLS; ++n) {
		previous_energy += (int64_t)expected(n - 1, full_scale) * expected(n - 1, full_scale);
		second_energy += (int64_t)expected(n - 2, full_scale) * expected(n - 2, full_scale);
		previous_cross += (int64_t)expected(n - 1, full_scale) * expected(n - 2, full_scale);
	}
	CHECK(previous_energy == channel.previous_expected_energy, "previous-target energy changed");
	CHECK(second_energy == channel.second_expected_energy && previous_cross == channel.previous_expected_cross,
		"second-target moments changed");
	for (int lag = 0; lag < V92_UPSTREAM_CHANNEL_Z2_TAPS; ++lag) {
		int64_t sum = 0;

		for (int n = V92_UPSTREAM_CHANNEL_SETTLE_SYMBOLS; n < V92_UPSTREAM_CHANNEL_SYMBOLS; ++n)
			sum += (int64_t)observed(n, full_scale) * observed(n - lag, full_scale);
		CHECK(sum == channel.observed_autocorrelation[lag], "observed lag %d changed", lag);
	}
	for (int lag = 0; lag <= V92_UPSTREAM_CHANNEL_P1_TAPS; ++lag) {
		int64_t sum = 0;

		for (int n = V92_UPSTREAM_CHANNEL_SETTLE_SYMBOLS; n < V92_UPSTREAM_CHANNEL_SYMBOLS; ++n)
			sum += (int64_t)expected(n, full_scale) * expected(n - lag, full_scale);
		CHECK(sum == channel.expected_autocorrelation[lag], "expected lag %d changed", lag);
	}
	for (int lag = -V92_UPSTREAM_CHANNEL_PRECURSOR_LAGS; lag <= V92_UPSTREAM_CHANNEL_MAX_LAG; ++lag) {
		int64_t sum = 0;

		for (int n = V92_UPSTREAM_CHANNEL_SETTLE_SYMBOLS; n < V92_UPSTREAM_CHANNEL_SYMBOLS; ++n)
			sum += lag >= 0 ? (int64_t)observed(n, full_scale) * expected(n - lag, full_scale) :
				(int64_t)expected(n, full_scale) * observed(n + lag, full_scale);
		CHECK(sum == channel.cross_correlation[V92_UPSTREAM_CHANNEL_PRECURSOR_LAGS + lag],
		      "cross-correlation lag %d changed", lag);
	}
	struct v92_upstream_channel saved = channel;

	CHECK(v92_upstream_channel_sample(&channel, 1, -32768) &&
	      memcmp(&channel, &saved, sizeof(channel)) == 0, "completed channel did not freeze");
	return true;
}

static bool test_rejection(void)
{
	struct v92_upstream_channel channel = { 0 }, saved = channel;

	CHECK(!v92_upstream_channel_is_valid(NULL) &&
	      !v92_upstream_channel_sample(NULL, 0, 0) &&
	      !v92_upstream_channel_sample(&channel, 0, -32769) &&
	      !v92_upstream_channel_sample(&channel, 0, 32769) &&
	      memcmp(&channel, &saved, sizeof(channel)) == 0, "invalid input changed channel");
	channel.window[0].expected_energy = 1;
	CHECK(!v92_upstream_channel_is_valid(&channel), "unowned window energy accepted");
	channel = saved;
	channel.symbols = V92_UPSTREAM_CHANNEL_SYMBOLS;
	channel.window[V92_UPSTREAM_CHANNEL_WINDOWS - 1u].observed_energy =
		INT64_C(32768) * 32768 * V92_UPSTREAM_CHANNEL_WINDOW_SYMBOLS;
	CHECK(!v92_upstream_channel_is_valid(&channel), "partial-window energy bound ignored");
	channel = saved;
	CHECK(!v92_upstream_channel_has_stable_gain(&channel) && !v92_upstream_channel_has_stable_gain(NULL),
	      "incomplete stability evidence accepted");
	channel.symbols = V92_UPSTREAM_CHANNEL_SYMBOLS + 1;
	saved = channel;
	CHECK(!v92_upstream_channel_sample(&channel, 0, 0) &&
	      memcmp(&channel, &saved, sizeof(channel)) == 0, "invalid extent changed channel");
	channel = (struct v92_upstream_channel){ 0 };
	channel.cross_correlation[0] = INT64_MAX;
	saved = channel;
	CHECK(!v92_upstream_channel_sample(&channel, 0, 0) &&
	      memcmp(&channel, &saved, sizeof(channel)) == 0, "unbounded correlation accepted");
	channel = (struct v92_upstream_channel){ 0 };
	channel.previous_expected_energy = 1;
	CHECK(!v92_upstream_channel_is_valid(&channel), "unowned previous-target energy accepted");
	channel.previous_expected_energy = -1;
	CHECK(!v92_upstream_channel_is_valid(&channel), "negative previous-target energy accepted");
	channel = (struct v92_upstream_channel){ 0 };
	channel.second_expected_energy = 1;
	CHECK(!v92_upstream_channel_is_valid(&channel), "unowned second-target energy accepted");
	channel.second_expected_energy = -1;
	CHECK(!v92_upstream_channel_is_valid(&channel), "negative second-target energy accepted");
	channel = (struct v92_upstream_channel){ 0 };
	channel.previous_expected_cross = -1;
	CHECK(!v92_upstream_channel_is_valid(&channel), "unowned delayed-target correlation accepted");
	channel.symbols = V92_UPSTREAM_CHANNEL_SYMBOLS;
	channel.previous_expected_cross = INT64_MAX;
	CHECK(!v92_upstream_channel_is_valid(&channel), "unbounded delayed-target correlation accepted");
	channel = (struct v92_upstream_channel){ 0 };
	channel.expected_history[1] = 1;
	CHECK(!v92_upstream_channel_is_valid(&channel), "unowned history accepted");
	channel = (struct v92_upstream_channel){ 0 };
	channel.expected_boundary[0] = 1;
	CHECK(!v92_upstream_channel_is_valid(&channel), "unowned boundary accepted");
	channel.symbols = V92_UPSTREAM_CHANNEL_SETTLE_SYMBOLS;
	CHECK(!v92_upstream_channel_is_valid(&channel), "boundary captured before measurement");
	channel.symbols++;
	CHECK(v92_upstream_channel_is_valid(&channel), "measured boundary rejected");
	channel.observed_boundary[V92_UPSTREAM_CHANNEL_MAX_LAG - 1u] = 32769;
	CHECK(!v92_upstream_channel_is_valid(&channel), "unbounded starting observation accepted");
	return true;
}

static bool test_gain_fit(void)
{
	static const struct {
		double gain, postcursor, noise;
		bool fits;
	} cases[] = {
		{ 0.25, 0.0, 0.0, true },
		{ 0.75, 0.0, 0.0, true },
		{ 1.25, 0.0, 0.0, true },
		{ 0.75, 0.0, 0.003, true },
		{ 0.75, 0.0, 0.02, false },
		{ 0.75, 0.02, 0.0, false },
		{ 0.75, 0.10, 0.0, false },
		{ -0.75, 0.0, 0.0, false },
		{ 0.0, 0.0, 0.0, false },
	};

	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index) {
		struct v92_upstream_channel channel = { 0 };

		for (int n = 0; n < V92_UPSTREAM_CHANNEL_SYMBOLS; ++n) {
			CHECK(!v92_upstream_channel_fits_gain(&channel), "incomplete gain fit accepted");
			int16_t target = expected(n, false) / 4;
			int32_t input = (int32_t)(cases[index].gain * target +
				cases[index].postcursor * (expected(n - 1, false) / 4) +
				cases[index].noise * (expected(n + 100000, false) / 4));

			CHECK(v92_upstream_channel_sample(&channel, target, input), "gain fixture failed");
		}
		struct v92_upstream_channel saved = channel;

		CHECK(v92_upstream_channel_fits_gain(&channel) == cases[index].fits &&
		      memcmp(&channel, &saved, sizeof(channel)) == 0,
		      "gain fit case %zu changed classification or measurement", index);
	}
	struct v92_upstream_channel edge = { .symbols = V92_UPSTREAM_CHANNEL_SYMBOLS };

	edge.expected_autocorrelation[0] = 10000;
	edge.observed_autocorrelation[0] = 10001;
	edge.cross_correlation[V92_UPSTREAM_CHANNEL_PRECURSOR_LAGS] = 10000;
	CHECK(v92_upstream_channel_fits_gain(&edge), "gain-fit residual below 1%% rejected");
	edge.observed_autocorrelation[0]++;
	CHECK(!v92_upstream_channel_fits_gain(&edge), "gain-fit residual above 1%% accepted");
	edge.observed_autocorrelation[0] = 9999;
	CHECK(!v92_upstream_channel_fits_gain(&edge), "inconsistent energy accepted");
	int64_t maximum = (int64_t)(V92_UPSTREAM_CHANNEL_SYMBOLS -
		V92_UPSTREAM_CHANNEL_SETTLE_SYMBOLS) * 32768 * 32768;

	edge.expected_autocorrelation[0] = edge.observed_autocorrelation[0] = maximum;
	edge.cross_correlation[V92_UPSTREAM_CHANNEL_PRECURSOR_LAGS] = maximum;
	CHECK(v92_upstream_channel_fits_gain(&edge), "full-scale gain fit overflowed");
	edge.cross_correlation[V92_UPSTREAM_CHANNEL_PRECURSOR_LAGS] = maximum / 2;
	CHECK(!v92_upstream_channel_fits_gain(&edge), "full-scale poor fit overflowed");
	CHECK(!v92_upstream_channel_fits_gain(NULL), "missing measurement accepted");
	edge.symbols++;
	CHECK(!v92_upstream_channel_fits_gain(&edge), "invalid measurement accepted");
	return true;
}

int main(void)
{
	return test_correlations(false) && test_correlations(true) && test_rejection() && test_gain_fit() ? 0 : 1;
}
