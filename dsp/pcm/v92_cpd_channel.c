// SPDX-License-Identifier: BSD-3-Clause

#include "v92_cpd_channel.h"

#include "memory_region.h"
#include "v92_cpd_gain.h"

#include <math.h>
#include <string.h>

enum {
	Z2_TAPS = V92_UPSTREAM_CHANNEL_Z2_TAPS,
	P1_TAPS = V92_UPSTREAM_CHANNEL_P1_TAPS,
	TAPS = Z2_TAPS + P1_TAPS,
	DELAY = V92_UPSTREAM_CHANNEL_DECISION_DELAY,
};

static double dot(const double *left, const double *right)
{
	double result = 0.0;

	for (size_t i = 0; i < TAPS; ++i)
		result += left[i] * right[i];
	return result;
}

static double cross(const struct v92_upstream_channel *channel, int lag, double scale)
{
	return (double)channel->cross_correlation[V92_UPSTREAM_CHANNEL_PRECURSOR_LAGS + lag] / scale;
}

/* Apply the symmetric normal matrix without allocating a dense matrix.
 * H(z)Z2(z) = z^-DELAY (1-P1(z)); unknowns are z2 then p1. */
static void apply_normal(const struct v92_upstream_channel *channel,
	const double input[TAPS], double regularization, double scale, double output[TAPS])
{
	for (size_t row = 0; row < Z2_TAPS; ++row) {
		double value = regularization * input[row];

		for (size_t col = 0; col < Z2_TAPS; ++col) {
			size_t lag = row > col ? row - col : col - row;

			value += (double)channel->observed_autocorrelation[lag] / scale * input[col];
		}
		for (size_t col = 0; col < P1_TAPS; ++col)
			value += cross(channel, DELAY + (int)col + 1 - (int)row, scale) * input[Z2_TAPS + col];
		output[row] = value;
	}
	for (size_t row = 0; row < P1_TAPS; ++row) {
		double value = regularization * input[Z2_TAPS + row];

		for (size_t col = 0; col < Z2_TAPS; ++col)
			value += cross(channel, DELAY + (int)row + 1 - (int)col, scale) * input[col];
		for (size_t col = 0; col < P1_TAPS; ++col) {
			size_t lag = row > col ? row - col : col - row;

			value += (double)channel->expected_autocorrelation[lag] / scale * input[Z2_TAPS + col];
		}
		output[Z2_TAPS + row] = value;
	}
}

static bool solve(const struct v92_upstream_channel *channel, double coefficients[TAPS])
{
	double residual[TAPS], direction[TAPS], normal[TAPS], rhs[TAPS];
	double observed = (double)channel->observed_autocorrelation[0];
	double expected = (double)channel->expected_autocorrelation[0];
	double scale = fmax(observed, expected);

	if (observed <= 0.0 || expected <= 0.0)
		return false;
	double regularization = 1.0e-3 * (Z2_TAPS * observed + P1_TAPS * expected) / (TAPS * scale);

	for (size_t tap = 0; tap < Z2_TAPS; ++tap)
		rhs[tap] = cross(channel, DELAY - (int)tap, scale);
	for (size_t tap = 0; tap < P1_TAPS; ++tap)
		rhs[Z2_TAPS + tap] = (double)channel->expected_autocorrelation[tap + 1u] / scale;
	memcpy(residual, rhs, sizeof(residual));
	memcpy(direction, rhs, sizeof(direction));
	memset(coefficients, 0, TAPS * sizeof(*coefficients));
	double norm = dot(residual, residual), initial_norm = norm;

	if (!(norm > 1.0e-24) || !isfinite(norm))
		return false;
	for (unsigned iteration = 0; iteration < 4u * TAPS; ++iteration) {
		apply_normal(channel, direction, regularization, scale, normal);
		double denominator = dot(direction, normal);

		if (!(denominator > 0.0) || !isfinite(denominator))
			return false;
		double step = norm / denominator;

		for (size_t tap = 0; tap < TAPS; ++tap) {
			coefficients[tap] += step * direction[tap];
			residual[tap] -= step * normal[tap];
		}
		double next_norm = dot(residual, residual);

		if (!isfinite(next_norm))
			return false;
		if (next_norm <= initial_norm * 1.0e-14) {
			/* Reject a solved but uninformative/noisy inverse. This is an
			 * implementation quality bound, not a negotiated wire limit. */
			apply_normal(channel, coefficients, 0.0, scale, normal);
			double error = expected / scale - 2.0 * dot(coefficients, rhs) + dot(coefficients, normal);

			return isfinite(error) && error <= 0.05 * expected / scale;
		}
		for (size_t tap = 0; tap < TAPS; ++tap)
			direction[tap] = residual[tap] + next_norm / norm * direction[tap];
		norm = next_norm;
	}
	return false;
}

static bool normalize_gain(struct v92_cpd_parameters *cpd, enum g711_law law)
{
	struct v92_pcm_mapping mapping;
	struct v92_pcm_frame_tx transmitter;
	uint8_t ones[V92_PCM_MAPPING_MAX_BITS];
	uint64_t energy = 0u;

	if (v92_pcm_mapping_init(&mapping, law, cpd) != V92_PCM_MAPPING_OK ||
	    v92_pcm_frame_tx_init(&transmitter, &mapping) != V92_PCM_FRAME_OK)
		return false;
	memset(ones, 1, mapping.data_bits_per_frame);
	for (unsigned i = 0; i < V92_B1U_FRAMES; ++i) {
		struct v92_pcm_frame frame;

		if (v92_pcm_frame_encode(&transmitter, ones, mapping.data_bits_per_frame, &frame) != V92_PCM_FRAME_OK)
			return false;
		for (size_t n = 0; n < V92_PCM_MAPPING_FRAME_SYMBOLS; ++n) {
			uint64_t square = (uint64_t)((int64_t)frame.v[n] * frame.v[n]);

			if (square > UINT64_MAX - energy)
				return false;
			energy += square;
		}
	}
	cpd->four_times_gain_q0_16 = v92_cpd_gain_for_energy(energy, V92_B1U_SYMBOLS);
	return cpd->four_times_gain_q0_16 != 0u;
}

bool v92_cpd_channel_design(struct v92_cpd_parameters *parameters,
	const struct v92_upstream_channel *channel, enum g711_law law,
	int16_t upstream_level, struct v92_upstream_plan *plan)
{
	double coefficients[TAPS], largest = 0.0;
	struct v92_cpd_parameters next;
	struct v92_upstream_plan aligned = { .observation = V92_PCM_FRAME_CONSTELLATION, .delay = DELAY };

	if (parameters == NULL || plan == NULL || !v92_upstream_channel_is_valid(channel) ||
	    channel->symbols != V92_UPSTREAM_CHANNEL_SYMBOLS || upstream_level <= 0 ||
	    !g711_law_is_valid(law) || !v92_cpd_parameters_are_valid(parameters) ||
	    !parameters->filters_present || !parameters->constellations_present || !parameters->modulus_present ||
	    memory_regions_overlap(parameters, sizeof(*parameters), channel, sizeof(*channel)) ||
	    memory_regions_overlap(parameters, sizeof(*parameters), plan, sizeof(*plan)) ||
	    memory_regions_overlap(plan, sizeof(*plan), channel, sizeof(*channel)) ||
	    !solve(channel, coefficients))
		return false;
	for (size_t tap = 0; tap < TAPS; ++tap) {
		if (!isfinite(coefficients[tap]) ||
		    (tap >= Z2_TAPS && (coefficients[tap] <= -2.0 || coefficients[tap] >= 2.0)))
			return false;
		if (tap < Z2_TAPS)
			largest = fmax(largest, fabs(coefficients[tap]));
	}
	if (!(largest > 1.0e-8))
		return false;
	double z2_scale = 24576.0 / largest;

	next = *parameters;
	memset(next.filter, 0, sizeof(next.filter));
	next.filter[V92_CPD_PRECODER_FEEDBACK].count = P1_TAPS;
	next.filter[V92_CPD_PREFILTER_FEED_FORWARD].count = Z2_TAPS;
	for (size_t tap = 0; tap < Z2_TAPS; ++tap)
		next.filter[V92_CPD_PREFILTER_FEED_FORWARD].coefficient[tap] = (int16_t)lround(coefficients[tap] * z2_scale);
	for (size_t tap = 0; tap < P1_TAPS; ++tap) {
		long value = lround(coefficients[Z2_TAPS + tap] * 16384.0);

		if (value < INT16_MIN || value > INT16_MAX)
			return false;
		next.filter[V92_CPD_PRECODER_FEEDBACK].coefficient[tap] = (int16_t)value;
	}
	/* The direct 32-state selection omits outer representatives that an
	 * identity precoder cannot reach. Restore them for precoder feedback. */
	if (next.trellis == V92_CPD_TRELLIS_32_STATE && next.constellation[0].count == 32u)
		next.constellation[0].count = 64u;
	/* A dense direct-channel window is not preserved by the new filters
	 * and gain. Use alternating compander levels for these larger families
	 * before normalizing the actual channel-compensated B1u waveform. */
	if (next.constellation[0].count == 32u || next.constellation[0].count == 64u) {
		for (unsigned point = 0; point < next.constellation[0].count; ++point)
			if (!g711_ucode_sample(law, (uint8_t)(2u * point + 1u), true,
				    &next.constellation[0].positive_linear[point]))
				return false;
	}
	if (!normalize_gain(&next, law))
		return false;
	aligned.gain = 32768.0 * 262144.0 / (z2_scale * next.four_times_gain_q0_16 * upstream_level);
	if (!v92_upstream_plan_is_valid(&aligned) || !v92_cpd_parameters_are_valid(&next))
		return false;
	*parameters = next;
	*plan = aligned;
	return true;
}
