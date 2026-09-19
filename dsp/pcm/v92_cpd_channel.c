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

static bool short_inverse(double main, double post, double second,
	double error, double observed, double coefficients[TAPS])
{
	if (!isfinite(main) || !isfinite(post) || !isfinite(second) || !isfinite(error) ||
	    main <= 0.0 || fabs(second) >= main || main + post + second <= 0.0 ||
	    main - post + second <= 0.0 || error < -1.0e-10 * observed || error > 4.0e-4 * observed)
		return false;
	long first = lround(-post / main * 16384.0);
	long next = lround(-second / main * 16384.0);

	/* The real and quantized second-order recurrences must both be stable:
	 * |p2| < 1, p1+p2 < 1 and p2-p1 < 1 (strict Jury conditions). */
	if (first < INT16_MIN || first > INT16_MAX || next <= -16384 || next >= 16384 ||
	    first + next >= 16384 || next - first >= 16384)
		return false;
	/* V.92 6.4.2: for H=a+b*z^-1+c*z^-2, choose P1=-(b*z^-1+c*z^-2)/a
	 * and Z2=z^-D/a. Then H*Z2=(1-P1)*z^-D. Common wire scaling and B1u
	 * normalization below still apply. Keep the existing 2% relative RMS
	 * residual limit; channels that do not fit use the general designer. */
	memset(coefficients, 0, TAPS * sizeof(*coefficients));
	coefficients[DELAY] = 1.0 / main;
	coefficients[Z2_TAPS] = first / 16384.0;
	coefficients[Z2_TAPS + 1u] = next / 16384.0;
	return true;
}

static bool solve_short_channel(const struct v92_upstream_channel *channel, double coefficients[TAPS])
{
	double scale = fmax(channel->expected_autocorrelation[0],
		fmax(channel->previous_expected_energy, channel->observed_autocorrelation[0]));

	if (!(scale > 0.0))
		return false;
	double xx = channel->expected_autocorrelation[0] / scale;
	double pp = channel->previous_expected_energy / scale;
	double xp = channel->expected_autocorrelation[1] / scale;
	double yy = channel->observed_autocorrelation[0] / scale;
	double xy = cross(channel, 0, scale), py = cross(channel, 1, scale);
	double determinant = xx * pp - xp * xp;

	/* Avoid fitting two nearly dependent training columns. These quality
	 * bounds choose an implementation, not a negotiated capability. */
	if (!(xx > 0.0 && pp > 0.0 && yy > 0.0) || determinant <= 1.0e-6 * xx * pp)
		return false;
	double main = (xy * pp - py * xp) / determinant;
	double post = (py * xx - xy * xp) / determinant;
	double error = yy - main * xy - post * py;

	if (short_inverse(main, post, 0.0, error, yy, coefficients))
		return true;
	/* Add a second delayed target only when the simpler fit was inadequate.
	 * Eliminate its projection onto the first two columns, using exact
	 * same-window moments rather than assuming stationarity at the edges. */
	double ss = channel->second_expected_energy / scale;
	double xs = channel->expected_autocorrelation[2] / scale;
	double ps = channel->previous_expected_cross / scale;
	double sy = cross(channel, 2, scale);
	double x_projection = (xs * pp - ps * xp) / determinant;
	double p_projection = (ps * xx - xs * xp) / determinant;
	double remaining = ss - xs * x_projection - ps * p_projection;

	if (!(ss > 0.0) || remaining <= 1.0e-6 * ss)
		return false;
	double second = (sy - xs * main - ps * post) / remaining;

	main -= x_projection * second;
	post -= p_projection * second;
	error = yy - main * xy - post * py - second * sy;
	return short_inverse(main, post, second, error, yy, coefficients);
}

static int64_t boundary_product(const struct v92_upstream_channel *channel,
	bool left_observed, unsigned left, bool right_observed, unsigned right)
{
	int32_t begin_left = left_observed ? channel->observed_boundary[left] : channel->expected_boundary[left];
	int32_t begin_right = right_observed ? channel->observed_boundary[right] : channel->expected_boundary[right];
	int32_t end_left = left_observed ? channel->observed_history[left] : channel->expected_history[left];
	int32_t end_right = right_observed ? channel->observed_history[right] : channel->expected_history[right];

	return (int64_t)begin_left * begin_right - (int64_t)end_left * end_right;
}

/* Correlations anchor the later signal at the measured window. Moving both
 * signals back by m adds m pairs before that window and removes m at its end.
 * This gives the exact finite-window Gram entry, not a stationary estimate. */
static int64_t moment(const struct v92_upstream_channel *channel,
	bool left_observed, unsigned left, bool right_observed, unsigned right)
{
	unsigned shift = left < right ? left : right;
	int lag = (int)right - (int)left;
	int64_t value;

	if (left_observed == right_observed) {
		unsigned distance = left > right ? left - right : right - left;

		value = left_observed ? channel->observed_autocorrelation[distance] :
			channel->expected_autocorrelation[distance];
	} else {
		value = channel->cross_correlation[V92_UPSTREAM_CHANNEL_PRECURSOR_LAGS +
			(left_observed ? lag : -lag)];
	}
	for (unsigned i = 0; i < shift; ++i)
		value += boundary_product(channel, left_observed, left - shift + i,
			right_observed, right - shift + i);
	return value;
}

/* A third postcursor can be small enough to survive the linear inverse
 * yet large enough to change tandem-codec decisions. Use the same finite
 * measurement window and residual limit as the shorter fits. */
static bool solve_four_tap_channel(const struct v92_upstream_channel *channel,
	double coefficients[TAPS])
{
	double system[4][5], fit[4], observed = channel->observed_autocorrelation[0];

	if (!(observed > 0.)) return false;
	for (unsigned i = 0; i < 4; i++) {
		for (unsigned j = 0; j < 4; j++)
			system[i][j] = moment(channel, false, i, false, j) / observed;
		system[i][4] = cross(channel, i, observed);
	}
	for (unsigned i = 0; i < 4; i++) {
		double pivot = system[i][i];
		if (!(pivot > 1e-8)) return false;
		for (unsigned k = i; k < 5; k++) system[i][k] /= pivot;
		for (unsigned j = 0; j < 4; j++) if (i != j) {
			double factor = system[j][i];
			for (unsigned k = i; k < 5; k++) system[j][k] -= factor * system[i][k];
		}
	}
	double error = 1., feedback = 0.;
	for (unsigned i = 0; i < 4; i++) {
		fit[i] = system[i][4];
		if (!isfinite(fit[i])) return false;
		error -= fit[i] * cross(channel, i, observed);
		if (i) feedback += fabs(fit[i]);
	}
	/* Sum |P1| < 1 is a sufficient stability condition for this third-order
	 * recurrence; verify it again after quantization. Other channels still
	 * use the general designer rather than an unproven short inverse. */
	if (fit[0] <= feedback || !isfinite(error) || error < -1e-10 || error > 4e-4)
		return false;
	long quantized[3], magnitude = 0;
	for (unsigned i = 0; i < 3; i++) {
		quantized[i] = lround(-fit[i + 1] / fit[0] * 16384.);
		magnitude += quantized[i] < 0 ? -quantized[i] : quantized[i];
	}
	if (magnitude >= 16384) return false;
	memset(coefficients, 0, TAPS * sizeof(*coefficients));
	coefficients[DELAY] = 1. / fit[0];
	for (unsigned i = 0; i < 3; i++) coefficients[Z2_TAPS + i] = quantized[i] / 16384.;
	return true;
}

/* Apply the symmetric normal matrix without allocating a dense matrix.
 * H(z)Z2(z) = z^-DELAY (1-P1(z)); unknowns are z2 then p1. Walking each
 * diagonal updates its exact covariance with just two boundary products,
 * retaining quadratic work/storage-linear workspace per CG iteration. */
static void apply_normal(const struct v92_upstream_channel *channel,
	const double input[TAPS], double regularization, double scale, double output[TAPS])
{
	for (unsigned i = 0; i < TAPS; ++i)
		output[i] = regularization * input[i];
	for (unsigned block = 0; block < 2u; ++block) {
		bool observed = block == 0;
		unsigned count = observed ? Z2_TAPS : P1_TAPS;
		unsigned delay = observed ? 0u : DELAY + 1u;
		unsigned offset = observed ? 0u : Z2_TAPS;

		for (unsigned lag = 0; lag < count; ++lag) {
			int64_t value = moment(channel, observed, delay + lag, observed, delay);

			for (unsigned col = 0, row = lag; row < count; ++col, ++row) {
				double covariance = (double)value / scale;

				output[offset + row] += covariance * input[offset + col];
				if (row != col)
					output[offset + col] += covariance * input[offset + row];
				if (row + 1u < count)
					value += boundary_product(channel, observed, delay + row, observed, delay + col);
			}
		}
	}
	for (int lag = DELAY + 2 - Z2_TAPS; lag <= DELAY + P1_TAPS; ++lag) {
		unsigned y = lag < DELAY + 1 ? (unsigned)(DELAY + 1 - lag) : 0u;
		unsigned x = (unsigned)((int)y + lag);
		int64_t value = moment(channel, true, y, false, x);

		for (; y < Z2_TAPS && x <= DELAY + P1_TAPS; ++y, ++x) {
			unsigned col = Z2_TAPS + x - DELAY - 1u;
			double covariance = (double)value / scale;

			output[y] += covariance * input[col];
			output[col] += covariance * input[y];
			if (y + 1u < Z2_TAPS && x < DELAY + P1_TAPS)
				value += boundary_product(channel, true, y, false, x);
		}
	}
}

static bool solve(const struct v92_upstream_channel *channel, double coefficients[TAPS])
{
	double residual[TAPS], direction[TAPS], normal[TAPS], rhs[TAPS];
	double observed = (double)channel->observed_autocorrelation[0];
	double expected = (double)channel->expected_autocorrelation[0];
	double target = (double)moment(channel, false, DELAY, false, DELAY);
	double scale = fmax(observed, expected);

	if (observed <= 0.0 || expected <= 0.0 || target <= 0.0)
		return false;
	double regularization = 1.0e-3 * (Z2_TAPS * observed + P1_TAPS * expected) / (TAPS * scale);

	for (size_t tap = 0; tap < Z2_TAPS; ++tap)
		rhs[tap] = (double)moment(channel, true, tap, false, DELAY) / scale;
	for (size_t tap = 0; tap < P1_TAPS; ++tap)
		rhs[Z2_TAPS + tap] = (double)moment(channel, false, DELAY + tap + 1u, false, DELAY) / scale;
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
			double error = target / scale - 2.0 * dot(coefficients, rhs) + dot(coefficients, normal);

			return isfinite(error) && error >= -1.0e-10 * target / scale &&
				error <= 0.05 * target / scale;
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

static uint8_t balanced_modulus(unsigned int data_bits)
{
	/* V.92 6.4.1-6.4.2: an ordinary interval needs M signed points,
	 * each fourth (trellis) interval needs 2M. Choose the smallest N
	 * with (2N)^9 * N^3 >= 2^K. Products fit in 128 bits for N<=64. */
	for (unsigned int points = 1u; points <= 64u; ++points) {
		__uint128_t capacity = 1u;

		for (size_t symbol = 0u; symbol < V92_CPD_MODULUS_COUNT; ++symbol)
			capacity *= symbol % 4u == 3u ? points : 2u * points;
		if (capacity >= ((__uint128_t)1u << data_bits))
			return (uint8_t)points;
	}
	return 0u;
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
	    (!solve_short_channel(channel, coefficients) &&
	     !solve_four_tap_channel(channel, coefficients) && !solve(channel, coefficients)))
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
	/* Balance 16-state channel mappings from 34.667k upward. Concentrating
	 * large points in some trellis intervals can clip after B1u normalization
	 * or erase payload distinctions during companding. Keep the low-rate
	 * and level-selected direct mappings separate. */
	if (next.data_rate_number >= 9u && next.trellis == V92_CPD_TRELLIS_16_STATE) {
		uint8_t modulus = balanced_modulus(v92_pcm_mapping_data_bits(next.data_rate_number));

		if (modulus == 0u)
			return false;
		for (size_t symbol = 0u; symbol < V92_CPD_MODULUS_COUNT; ++symbol)
			next.modulus[symbol] = symbol % 4u == 3u ? modulus : 2u * modulus;
	}
	/* The direct 40k/44k selections omit outer representatives that an
	 * identity precoder cannot reach. Restore them for precoder feedback. */
	if ((next.trellis == V92_CPD_TRELLIS_32_STATE && next.constellation[0].count == 32u) ||
	    (next.data_rate_number == 16u && next.trellis == V92_CPD_TRELLIS_16_STATE &&
	     next.constellation[0].count == 27u))
		next.constellation[0].count = 64u;
	/* CPd carries linear points, not codec indices. A sinh grid has a
	 * finite spacing floor near zero and progressively wider outer gaps.
	 * For the dense 16-state family, this protects small symbols from residual ISI
	 * instead of compressing odd codec levels into adjoining wire cells
	 * after power normalization. Shape 4 balances that floor and companding
	 * noise; peak 30000 leaves signed-observation headroom. These are design
	 * choices, not wire limits. Keep the 32-state family's spacing: its
	 * redundant representatives have different error margins. Normalize the
	 * actual filtered B1u below. */
	if (next.trellis == V92_CPD_TRELLIS_16_STATE && next.constellation[0].count == 64u) {
		for (unsigned point = 0; point < 64u; ++point)
			next.constellation[0].positive_linear[point] = (int16_t)lround(
				30000.0 * sinh(4.0 * (point + 0.5) / 63.5) / sinh(4.0));
	} else if (next.constellation[0].count == 32u || next.constellation[0].count == 64u) {
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
