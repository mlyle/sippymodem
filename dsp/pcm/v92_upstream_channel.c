// SPDX-License-Identifier: BSD-3-Clause

#include "v92_upstream_channel.h"

#include "wide_uint.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

static bool correlations_are_bounded(const int64_t *values, size_t count, int64_t bound)
{
	for (size_t index = 0; index < count; ++index)
		if (values[index] < -bound || values[index] > bound)
			return false;
	return true;
}

static unsigned window_symbols(unsigned used, unsigned window)
{
	unsigned start = window * V92_UPSTREAM_CHANNEL_WINDOW_SYMBOLS;

	if (used <= start)
		return 0;
	return used - start < V92_UPSTREAM_CHANNEL_WINDOW_SYMBOLS ?
		used - start : V92_UPSTREAM_CHANNEL_WINDOW_SYMBOLS;
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
	for (unsigned i = 0; i < V92_UPSTREAM_CHANNEL_WINDOWS; ++i) {
		const struct v92_upstream_channel_window *window = &channel->window[i];
		int64_t limit = (int64_t)window_symbols(used, i) * 32768 * 32768;

		if (window->expected_energy < 0 || window->expected_energy > limit ||
		    window->observed_energy < 0 || window->observed_energy > limit ||
		    window->cross < -limit || window->cross > limit)
			return false;
	}
	for (size_t index = 0; index <= V92_UPSTREAM_CHANNEL_MAX_LAG; ++index) {
		if (channel->observed_history[index] < -32768 ||
		    channel->observed_history[index] > 32768 ||
		    (index >= channel->symbols &&
		     (channel->observed_history[index] != 0 || channel->expected_history[index] != 0)))
			return false;
	}
	for (size_t index = 0; index < V92_UPSTREAM_CHANNEL_MAX_LAG; ++index)
		if (channel->observed_boundary[index] < -32768 ||
		    channel->observed_boundary[index] > 32768 ||
		    (channel->symbols <= V92_UPSTREAM_CHANNEL_SETTLE_SYMBOLS &&
		     (channel->observed_boundary[index] || channel->expected_boundary[index])))
			return false;
	return true;
}

bool v92_upstream_channel_sample(
	struct v92_upstream_channel *channel, int16_t expected, int32_t observed)
{
	if (!v92_upstream_channel_is_valid(channel))
		return false;
	return v92_upstream_channel_sample_owned(channel, expected, observed);
}

bool v92_upstream_channel_sample_owned(
	struct v92_upstream_channel *channel, int16_t expected, int32_t observed)
{
	if (channel == NULL || channel->symbols > V92_UPSTREAM_CHANNEL_SYMBOLS ||
	    observed < -32768 || observed > 32768)
		return false;
	if (channel->symbols == V92_UPSTREAM_CHANNEL_SYMBOLS)
		return true;
	if (channel->symbols == V92_UPSTREAM_CHANNEL_SETTLE_SYMBOLS) {
		memcpy(channel->observed_boundary, channel->observed_history,
			sizeof(channel->observed_boundary));
		memcpy(channel->expected_boundary, channel->expected_history,
			sizeof(channel->expected_boundary));
	}
	memmove(channel->observed_history + 1, channel->observed_history,
		V92_UPSTREAM_CHANNEL_MAX_LAG * sizeof(channel->observed_history[0]));
	memmove(channel->expected_history + 1, channel->expected_history,
		V92_UPSTREAM_CHANNEL_MAX_LAG * sizeof(channel->expected_history[0]));
	channel->observed_history[0] = observed;
	channel->expected_history[0] = expected;
	if (++channel->symbols <= V92_UPSTREAM_CHANNEL_SETTLE_SYMBOLS)
		return true;
	unsigned used = channel->symbols - V92_UPSTREAM_CHANNEL_SETTLE_SYMBOLS;
	struct v92_upstream_channel_window *window =
		&channel->window[(used - 1u) / V92_UPSTREAM_CHANNEL_WINDOW_SYMBOLS];

	window->expected_energy += (int64_t)expected * expected;
	window->observed_energy += (int64_t)observed * observed;
	window->cross += (int64_t)expected * observed;
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

bool v92_upstream_channel_has_stable_gain(const struct v92_upstream_channel *channel)
{
	if (!v92_upstream_channel_is_valid(channel) ||
	    channel->symbols != V92_UPSTREAM_CHANNEL_SYMBOLS)
		return false;
	double lower = -DBL_MAX, upper = DBL_MAX;
	unsigned used = channel->symbols - V92_UPSTREAM_CHANNEL_SETTLE_SYMBOLS;

	for (unsigned i = 0; i < V92_UPSTREAM_CHANNEL_WINDOWS; ++i) {
		const struct v92_upstream_channel_window *window = &channel->window[i];
		double xx = window->expected_energy, xy = window->cross, yy = window->observed_energy;

		if (!(xx > 0.0))
			return false;
		double gain = xy / xx, residual = yy - gain * xy;

		if (residual < -1.0e-10 * yy)
			return false;
		/* Interference contributes uncertainty to the scalar estimate. Six
		 * estimated standard errors plus 1% relative gain tolerance keep
		 * static ISI/noise from being mistaken for a changing channel.
		 * These are estimator safeguards, not V.92 acceptance limits. */
		double margin = 0.01 * fabs(gain) +
			6.0 * sqrt(fmax(0.0, residual) / (window_symbols(used, i) * xx));

		lower = fmax(lower, gain - margin);
		upper = fmin(upper, gain + margin);
	}
	return lower <= upper;
}

bool v92_upstream_channel_fits_gain(const struct v92_upstream_channel *channel)
{
	if (!v92_upstream_channel_is_valid(channel) ||
	    channel->symbols != V92_UPSTREAM_CHANNEL_SYMBOLS)
		return false;
	int64_t cross = channel->cross_correlation[V92_UPSTREAM_CHANNEL_PRECURSOR_LAGS];
	int64_t expected = channel->expected_autocorrelation[0];
	int64_t observed = channel->observed_autocorrelation[0];

	if (cross <= 0 || expected <= 0 || observed <= 0)
		return false;
	/* The best scalar's unexplained energy is yy - xy*xy/xx. Compare
	 * without cancellation or division; the bounded training sums and
	 * their products fit in 128 bits, including the squared 1% bound. */
	struct wide_uint energy = wide_product_u64(expected, observed);
	struct wide_uint explained = wide_product_u64(cross, cross), residual;

	if (wide_compare(explained, energy) > 0)
		return false;
	wide_multiply_u32(wide_subtract(energy, explained), 10000, &residual);
	return wide_compare(residual, energy) <= 0;
}
