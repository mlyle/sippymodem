// SPDX-License-Identifier: BSD-3-Clause

#include "v92_upstream_channel.h"

#include <stddef.h>
#include <string.h>

static bool correlations_are_bounded(const int64_t *values, size_t count, int64_t bound)
{
	for (size_t index = 0; index < count; ++index)
		if (values[index] < -bound || values[index] > bound)
			return false;
	return true;
}

bool v92_upstream_channel_is_valid(const struct v92_upstream_channel *channel)
{
	if (channel == NULL || channel->symbols > V92_UPSTREAM_CHANNEL_SYMBOLS)
		return false;
	unsigned int used = channel->symbols > V92_UPSTREAM_CHANNEL_SETTLE_SYMBOLS ?
		(unsigned int)channel->symbols - V92_UPSTREAM_CHANNEL_SETTLE_SYMBOLS : 0u;
	int64_t bound = (int64_t)used * INT64_C(32768) * INT64_C(32768);

	if (channel->observed_autocorrelation[0] < 0 || channel->expected_autocorrelation[0] < 0 ||
	    !correlations_are_bounded(channel->observed_autocorrelation,
		V92_UPSTREAM_CHANNEL_Z2_TAPS, bound) ||
	    !correlations_are_bounded(channel->expected_autocorrelation,
		V92_UPSTREAM_CHANNEL_P1_TAPS + 1u, bound) ||
	    !correlations_are_bounded(channel->cross_correlation,
		V92_UPSTREAM_CHANNEL_CROSS_LAGS, bound))
		return false;
	for (size_t index = 0; index <= V92_UPSTREAM_CHANNEL_MAX_LAG; ++index) {
		if (channel->observed_history[index] < -32768 ||
		    channel->observed_history[index] > 32768 ||
		    (index >= channel->symbols &&
		     (channel->observed_history[index] != 0 || channel->expected_history[index] != 0)))
			return false;
	}
	return true;
}

bool v92_upstream_channel_sample(
	struct v92_upstream_channel *channel, int16_t expected, int32_t observed)
{
	if (!v92_upstream_channel_is_valid(channel) || observed < -32768 || observed > 32768)
		return false;
	if (channel->symbols == V92_UPSTREAM_CHANNEL_SYMBOLS)
		return true;
	memmove(channel->observed_history + 1, channel->observed_history,
		V92_UPSTREAM_CHANNEL_MAX_LAG * sizeof(channel->observed_history[0]));
	memmove(channel->expected_history + 1, channel->expected_history,
		V92_UPSTREAM_CHANNEL_MAX_LAG * sizeof(channel->expected_history[0]));
	channel->observed_history[0] = observed;
	channel->expected_history[0] = expected;
	if (++channel->symbols <= V92_UPSTREAM_CHANNEL_SETTLE_SYMBOLS)
		return true;
	for (size_t lag = 0; lag < V92_UPSTREAM_CHANNEL_Z2_TAPS; ++lag)
		channel->observed_autocorrelation[lag] += (int64_t)observed * channel->observed_history[lag];
	for (size_t lag = 0; lag <= V92_UPSTREAM_CHANNEL_P1_TAPS; ++lag)
		channel->expected_autocorrelation[lag] += (int64_t)expected * channel->expected_history[lag];
	for (size_t lag = 0; lag <= V92_UPSTREAM_CHANNEL_MAX_LAG; ++lag)
		channel->cross_correlation[V92_UPSTREAM_CHANNEL_PRECURSOR_LAGS + lag] +=
			(int64_t)observed * channel->expected_history[lag];
	for (size_t lead = 1; lead <= V92_UPSTREAM_CHANNEL_PRECURSOR_LAGS; ++lead)
		channel->cross_correlation[V92_UPSTREAM_CHANNEL_PRECURSOR_LAGS - lead] +=
			(int64_t)expected * channel->observed_history[lead];
	return true;
}
