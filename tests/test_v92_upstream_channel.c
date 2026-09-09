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
	}
	/* Compute each complete sum independently, without the streaming history. */
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
	channel.expected_history[1] = 1;
	CHECK(!v92_upstream_channel_is_valid(&channel), "unowned history accepted");
	return true;
}

int main(void)
{
	return test_correlations(false) && test_correlations(true) && test_rejection() ? 0 : 1;
}
