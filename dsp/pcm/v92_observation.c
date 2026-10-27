// SPDX-License-Identifier: BSD-3-Clause
#include "v92_observation.h"
#include "v92_observation_fit.h"
#include "memory_region.h"
#include "gram_fit.h"
#include "integer_math.h"
#include "sample_math.h"

#include <fenv.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define V92_SCORE_SCALE 1e6

enum { INTERVALS = V92_OBSERVATION_INTERVALS, TAPS = V92_OBSERVATION_TAPS };
_Static_assert(INTERVALS <= sizeof(unsigned) * CHAR_BIT, "history tags must fit");
_Static_assert((int)TAPS == (int)GRAM_FIT_COLUMNS, "Gram moment stride must match");
_Static_assert(V92_OBSERVATION_STEPS < UINT16_MAX, "geometry tags must fit");
_Static_assert((V92_OBSERVATION_CACHE & (V92_OBSERVATION_CACHE - 1)) == 0 &&
	(V92_OBSERVATION_POINTS & (V92_OBSERVATION_POINTS - 1)) == 0,
	"cache sizes must be powers of two");
_Static_assert(V92_OBSERVATION_SPANS == 1024, "coordinate hash uses its upper ten bits");

static bool codec_bounds(enum g711_law law, double gain, int16_t observed,
	const int16_t *level, double offset, double *lower, double *upper);

static bool solve_channel(const struct v92_observation_fit_moments *moments, unsigned taps, double fitted[TAPS])
{
	return gram_fit(moments->gram, moments->rhs, taps, fitted) && fitted[0] > 0.;
}

/* On one side of zero, erfc(b)/erfc(a) <= exp(a*a-b*b).
 * A gap of 40 puts the subtracted tail below half an ulp of erfc(a),
 * including subnormal results. Directed rounding keeps the subtraction. */
static double positive_gaussian_probability(double a, double b)
{
	if (a >= 28.) return 0.;
	double tail = erfc(a);
	if (b >= 28. || ((b - a) * (b + a) >= 40. && fegetround() == FE_TONEAREST))
		return .5 * tail;
	return .5 * (tail - erfc(b));
}

/* Gaussian interval probability with arguments scaled by sqrt(2)*sigma.
 * erfc(28) is below half the smallest binary64 subnormal; erf(6) rounds
 * to one. These are representable-value tails, not probability cutoffs. */
static double gaussian_probability(double a, double b)
{
	if (a > 0.) return positive_gaussian_probability(a, b);
	if (b < 0.) return positive_gaussian_probability(-b, -a);
	return .5 * ((b >= 6. ? 1. : erf(b)) - (a <= -6. ? -1. : erf(a)));
}

/* log P(N(0,1) > x). Beyond 12 sigma the decreasing asymptotic series
 * reaches binary64 precision before its terms start growing. Working in
 * logs preserves candidate ordering even when the mass underflows. */
static double gaussian_log_tail(double x)
{
	if (x < 12.) return log(.5 * erfc(x / sqrt(2.)));
	double term = 1., sum = 1.;
	for (unsigned i = 1; i < 32; i++) {
		double next = term * (-(double)(2 * i - 1) / (x * x));
		if (fabs(next) > fabs(term)) break;
		sum += next; term = next;
		if (fabs(term) < DBL_EPSILON * fabs(sum)) break;
	}
	return -.5 * x * x - log(x) - .5 * log(2. * acos(-1.)) + log(sum);
}

static double gaussian_log_probability(double a, double b)
{
	double probability = gaussian_probability(a, b);
	if (probability >= DBL_MIN) return log(probability);
	if (b < 0.) { double swap = a; a = -b; b = -swap; }
	if (a == HUGE_VAL) return -HUGE_VAL;
	/* Arguments above are scaled by sqrt(2)*sigma, unlike log_tail. */
	double low = gaussian_log_tail(a * sqrt(2.));
	/* Q(sqrt(2)*b)/Q(sqrt(2)*a) <= exp(a*a-b*b) for a >= 0.
	 * Below exp(-40), expm1 rounds to -1 and its log correction is zero.
	 * Bound the argument so exponent rounding stays well below the gap
	 * margin; huge adjacent arguments can otherwise collapse to one tail.
	 * Directed rounding and nonfinite tails retain the original expression. */
	if (isfinite(low) && a >= 0. && a < 0x1p24 &&
		(b - a) * (b + a) >= 40. && fegetround() == FE_TONEAREST)
		return low;
	double high = gaussian_log_tail(b * sqrt(2.));
	return low + log(-expm1(high - low));
}

static double log_add(double a, double b)
{
	if (isnan(a) || isnan(b)) return NAN;
	if (a == -HUGE_VAL) return b;
	if (b == -HUGE_VAL) return a;
	return fmax(a, b) + log1p(exp(fmin(a, b) - fmax(a, b)));
}

static uint64_t score_value(double value)
{
	if (value <= 0.) return 0;
	return value < (double)UINT64_MAX ? (uint64_t)value : UINT64_MAX;
}

static double bin_cost(const int16_t *q, const double *low, const double *high,
	size_t count, unsigned taps, const struct v92_observation_interval *fit, double variance)
{
	double cost = 0., divisor = sqrt(2. * variance);
	for (size_t j = taps; j < count; j++) {
		double mean = 0.;
		for (unsigned i = 0; i < taps; i++) mean += fit->coefficient[i] * q[j - i];
		double probability = gaussian_probability((low[j] - mean) / divisor, (high[j] - mean) / divisor);
		if (!(probability > 0.)) return HUGE_VAL;
		cost -= 2. * log(probability);
	}
	return cost;
}

/* Conditional moments of a Gaussian inside the received codec interval.
 * Measuring only distance OUTSIDE the interval underestimates noise: a
 * prediction inside the bin is not evidence that no noise was present. */
static bool bin_moments(double low, double high, double mean, double variance,
	double sigma, double *expected, double *deviation)
{
	double a = (low - mean) / sigma, b = (high - mean) / sigma;
	double probability = gaussian_probability(a / sqrt(2.), b / sqrt(2.));
	if (!(probability > 0.)) return false;
	double pa = exp(-.5 * a * a) / sqrt(2. * acos(-1.));
	double pb = exp(-.5 * b * b) / sqrt(2. * acos(-1.));
	double shift = (pa - pb) / probability;
	double ap = isfinite(a) ? a * pa : 0., bp = isfinite(b) ? b * pb : 0.;
	*expected = mean + sigma * shift;
	*deviation = variance * fmax(0., 1. + (ap - bp) / probability - shift * shift);
	return isfinite(*expected) && isfinite(*deviation);
}

/* Shared conditional-bin E/M updates. Owners supply their fixed Gram system,
 * observation range and initial noise. Scalar gain retains all 96 updates;
 * the channel fallback may stop at its existing convergence boundary. */
static bool refine_bin_fit(double coefficient[TAPS], const int16_t *q,
	const double *low, const double *high, size_t count, size_t first,
	unsigned taps, struct v92_observation_fit_moments *m, double noise, bool converge, double *variance)
{
	for (unsigned iteration = 0; iteration < 96; iteration++) {
		double targets[V92_OBSERVATION_TRAINING], deviations[V92_OBSERVATION_TRAINING];
		double next[TAPS] = { 0 }, next_noise = 0.;
		/* Every bin in this update uses the same noise distribution. */
		double sigma = sqrt(noise);
		memset(m->rhs, 0, sizeof(m->rhs));
		for (size_t j = first; j < count; j++) {
			double mean = 0.;
			for (unsigned i = 0; i < taps; i++) mean += coefficient[i] * q[j - i];
			if (!bin_moments(low[j], high[j], mean, noise, sigma, targets + j, deviations + j)) return false;
			for (unsigned i = 0; i < taps; i++) m->rhs[i] += q[j - i] * targets[j];
		}
		if (taps == 1u) {
			next[0] = m->rhs[0] / m->gram[0];
			if (!isfinite(next[0]) || next[0] <= 0.) return false;
		} else if (!solve_channel(m, taps, next)) return false;
		for (size_t j = first; j < count; j++) {
			double residual = targets[j];
			for (unsigned i = 0; i < taps; i++) residual -= next[i] * q[j - i];
			next_noise += deviations[j] + residual * residual;
		}
		next_noise = fmax(1., next_noise / (count - first));
		if (!isfinite(next_noise)) return false;
		bool settled = false;
		if (converge) {
			double change = 0.;
			for (unsigned i = 0; i < taps; i++) change += fabs(next[i] - coefficient[i]);
			settled = fabs(next_noise - noise) < 1e-8 * (1. + noise) &&
				change < 1e-10 * (1. + next[0]);
		}
		memcpy(coefficient, next, sizeof(next)); noise = next_noise;
		if (settled) break;
	}
	*variance = noise;
	return true;
}

enum { BIN_FIT_COLUMNS = TAPS + 1, BIN_FIT_EVALUATIONS = 24 };

struct bin_fit_data {
	double q[V92_OBSERVATION_TRAINING][TAPS];
	double low[V92_OBSERVATION_TRAINING], high[V92_OBSERVATION_TRAINING];
	double scale[TAPS], extent;
	size_t count;
	unsigned taps, evaluations;
};

/* With beta = coefficient/sigma and precision = 1/sigma, both Gaussian
 * interval endpoints are affine. Their negative log probability is convex.
 * For endpoint derivatives va and vb, its gradient is ra*va - rb*vb and
 * Hessian is gg' - a*ra*va*va' + b*rb*vb*vb', where ra = phi(a)/P.
 * Normalized columns keep the channel/noise system well scaled. */
static bool bin_fit_evaluate(struct bin_fit_data *data, const double *x,
	double *loss, double *gradient, double hessian[BIN_FIT_COLUMNS][BIN_FIT_COLUMNS])
{
	if (data->evaluations == BIN_FIT_EVALUATIONS) return false;
	data->evaluations++;
	*loss = 0.;
	memset(gradient, 0, BIN_FIT_COLUMNS * sizeof(*gradient));
	memset(hessian, 0, BIN_FIT_COLUMNS * BIN_FIT_COLUMNS * sizeof(double));
	for (size_t j = 0; j < data->count; j++) {
		double mean = 0.;
		for (unsigned i = 0; i < data->taps; i++) mean += x[i] * data->q[j][i];
		double a = x[data->taps] * data->low[j] - mean;
		double b = x[data->taps] * data->high[j] - mean;
		double probability = gaussian_probability(a / sqrt(2.), b / sqrt(2.));
		/* Subnormal ratios retain the conditional-moment fallback. */
		if (!(probability >= DBL_MIN && probability <= 1.)) return false;
		*loss -= log(probability);
		double ra = exp(-.5 * a * a) / sqrt(2. * acos(-1.)) / probability;
		double rb = exp(-.5 * b * b) / sqrt(2. * acos(-1.)) / probability;
		double ar = isfinite(a) ? a * ra : 0., br = isfinite(b) ? b * rb : 0.;
		double va[BIN_FIT_COLUMNS] = { 0 }, vb[BIN_FIT_COLUMNS] = { 0 };
		double g[BIN_FIT_COLUMNS] = { 0 };
		for (unsigned i = 0; i < data->taps; i++) va[i] = vb[i] = -data->q[j][i];
		va[data->taps] = isfinite(data->low[j]) ? data->low[j] : 0.;
		vb[data->taps] = isfinite(data->high[j]) ? data->high[j] : 0.;
		for (unsigned i = 0; i <= data->taps; i++) {
			g[i] = ra * va[i] - rb * vb[i];
			gradient[i] += g[i];
		}
		for (unsigned i = 0; i <= data->taps; i++)
			for (unsigned k = 0; k <= i; k++)
				hessian[i][k] += g[i] * g[k] - ar * va[i] * va[k] + br * vb[i] * vb[k];
	}
	for (unsigned i = 0; i <= data->taps; i++) {
		if (!isfinite(gradient[i])) return false;
		for (unsigned k = 0; k <= i; k++) {
			if (!isfinite(hessian[i][k])) return false;
			hessian[k][i] = hessian[i][k];
		}
	}
	return isfinite(*loss);
}

/* Eliminate the precision coordinate with a Schur complement, retaining
 * the shared four-column Gram solver for channel taps. At the noise floor
 * solve the active face instead. Roundoff may require a diagonal shift. */
static bool bin_fit_direction(unsigned taps, const double *gradient,
	const double hessian[BIN_FIT_COLUMNS][BIN_FIT_COLUMNS], bool fixed,
	double *step, double *regularization)
{
	double diagonal = 1., shift = 0.;
	for (unsigned i = 0; i <= taps; i++) diagonal = fmax(diagonal, fabs(hessian[i][i]));
	for (unsigned attempt = 0; attempt < 8; attempt++) {
		if (attempt == 1) shift = diagonal * 1e-12;
		else if (attempt > 1) shift *= 100.;
		double gram[GRAM_FIT_MOMENTS] = { 0 }, rhs[TAPS] = { 0 };
		double precision = hessian[taps][taps] + shift;
		if (!fixed && !(precision > 0.)) continue;
		for (unsigned i = 0; i < taps; i++) {
			rhs[i] = -gradient[i] + (fixed ? 0. : hessian[i][taps] * gradient[taps] / precision);
			for (unsigned k = 0; k <= i; k++)
				gram[gram_moment_index(i, k)] = hessian[i][k] + (i == k ? shift : 0.) -
					(fixed ? 0. : hessian[i][taps] * hessian[k][taps] / precision);
		}
		memset(step, 0, BIN_FIT_COLUMNS * sizeof(*step));
		if (!gram_fit(gram, rhs, taps, step)) continue;
		if (!fixed) {
			double sum = gradient[taps];
			for (unsigned i = 0; i < taps; i++) sum += hessian[taps][i] * step[i];
			step[taps] = -sum / precision;
		}
		double product = 0.;
		bool finite = true;
		for (unsigned i = 0; i <= taps; i++) {
			product += gradient[i] * step[i];
			finite = finite && isfinite(step[i]);
		}
		if (finite && product < 0.) {
			*regularization = shift;
			return true;
		}
	}
	return false;
}

static bool bin_fit_commit(const struct bin_fit_data *data, const double *x,
	double coefficient[TAPS], double *variance)
{
	double sigma = data->extent / x[data->taps], fitted[TAPS] = { 0 };
	double noise = sigma * sigma;
	if (!isfinite(noise) || noise < 1. - 1e-12) return false;
	for (unsigned i = 0; i < data->taps; i++) {
		fitted[i] = x[i] * sigma / data->scale[i];
		if (!isfinite(fitted[i])) return false;
	}
	if (!(fitted[0] > 0.)) return false;
	memcpy(coefficient, fitted, sizeof(fitted));
	*variance = fmax(1., noise);
	return true;
}

/* Fit the channel and its noise together. Reuse derivatives of accepted
 * line-search points; every evaluation, including rejected trials, consumes
 * the fixed budget. Declining leaves the original conditional-moment seed
 * intact. Directed rounding keeps that original path as well. */
static bool fit_bin_channel_newton(double coefficient[TAPS], const int16_t *q,
	const double *low, const double *high, size_t count, unsigned taps,
	double noise, double *variance)
{
	if (fegetround() != FE_TONEAREST || !taps || taps > TAPS || count <= taps ||
	    count > V92_OBSERVATION_TRAINING || !isfinite(noise) || noise < 1.) return false;
	struct bin_fit_data data = { .count = count - taps, .taps = taps };
	double x[BIN_FIT_COLUMNS] = { 0 }, sigma = sqrt(noise);
	for (unsigned i = 0; i < taps; i++) {
		double sum = 0.;
		for (size_t j = taps; j < count; j++) sum += (double)q[j - i] * q[j - i];
		data.scale[i] = sqrt(sum / data.count);
		if (!(data.scale[i] > 0.)) return false;
		x[i] = coefficient[i] * data.scale[i] / sigma;
	}
	for (size_t j = taps; j < count; j++) {
		for (unsigned i = 0; i < taps; i++) data.q[j - taps][i] = q[j - i] / data.scale[i];
		if (isfinite(low[j])) data.extent = fmax(data.extent, fabs(low[j]));
		if (isfinite(high[j])) data.extent = fmax(data.extent, fabs(high[j]));
	}
	if (!(data.extent > 0.) || !isfinite(data.extent)) return false;
	x[taps] = data.extent / sigma;
	if (!(x[0] > 0.) || !(x[taps] > 0.)) return false;
	for (size_t j = taps; j < count; j++) {
		data.low[j - taps] = low[j] / data.extent;
		data.high[j - taps] = high[j] / data.extent;
	}
	double loss, gradient[BIN_FIT_COLUMNS], hessian[BIN_FIT_COLUMNS][BIN_FIT_COLUMNS];
	if (!bin_fit_evaluate(&data, x, &loss, gradient, hessian)) return false;
	for (unsigned iteration = 0; iteration < BIN_FIT_EVALUATIONS; iteration++) {
		/* Variance >= 1 bounds normalized precision above by extent. */
		bool upper = x[taps] >= data.extent * (1. - 1e-12), fixed = false;
		double norm = 0.;
		for (unsigned i = 0; i <= taps; i++)
			if (i < taps || !upper || gradient[taps] > 0.) norm = fmax(norm, fabs(gradient[i]));
		if (norm < 1e-10) return bin_fit_commit(&data, x, coefficient, variance);
		double step[BIN_FIT_COLUMNS], regularization;
		bool direction = bin_fit_direction(taps, gradient, hessian, false, step, &regularization);
		/* Coupling can drive the step outward even when the partial
		 * precision gradient points inward. Re-solve the active face. */
		if (upper && (!direction || step[taps] > 0.)) {
			fixed = true;
			direction = bin_fit_direction(taps, gradient, hessian, true, step, &regularization);
		}
		if (!direction) return false;
		double product = 0.;
		for (unsigned i = 0; i <= taps; i++) product += gradient[i] * step[i];
		if (-product * .5 < 1e-12 * (1. + fabs(loss)) && (!regularization || norm < 1e-8) &&
		    (!fixed || gradient[taps] <= 0.))
			return bin_fit_commit(&data, x, coefficient, variance);
		double length = 1.;
		if (step[taps] > 0.) length = fmin(length, (data.extent - x[taps]) / step[taps]);
		else if (step[taps] < 0.) length = fmin(length, -.99 * x[taps] / step[taps]);
		if (step[0] < 0.) length = fmin(length, -.99 * x[0] / step[0]);
		if (!(length > 0.)) return false;
		bool accepted = false;
		for (unsigned backtrack = 0; backtrack < BIN_FIT_EVALUATIONS; backtrack++) {
			if (data.evaluations == BIN_FIT_EVALUATIONS) return false;
			double proposal[BIN_FIT_COLUMNS] = { 0 };
			for (unsigned i = 0; i <= taps; i++) proposal[i] = x[i] + length * step[i];
			proposal[taps] = fmin(data.extent, proposal[taps]);
			if (!memcmp(proposal, x, sizeof(x))) return false;
			double next_loss, next_gradient[BIN_FIT_COLUMNS];
			double next_hessian[BIN_FIT_COLUMNS][BIN_FIT_COLUMNS];
			if (bin_fit_evaluate(&data, proposal, &next_loss, next_gradient, next_hessian) &&
			    next_loss <= loss + 1e-4 * length * product) {
				memcpy(x, proposal, sizeof(x));
				loss = next_loss;
				memcpy(gradient, next_gradient, sizeof(gradient));
				memcpy(hessian, next_hessian, sizeof(hessian));
				accepted = true;
				break;
			}
			length *= .5;
		}
		if (!accepted) return false;
	}
	return false;
}

/* Start the channel fit with its center residuals. The one-sample variance
 * floor bounds numeric precision, not channel quality. Payload never updates
 * these parameters. */
static bool fit_bin_channel(struct v92_observation_interval *fit, const int16_t *q,
	const int16_t *y, const double *low, const double *high, size_t count, unsigned taps, double *variance)
{
	struct v92_observation_fit_moments m = { 0 };
	double noise = 0.;
	for (size_t j = taps; j < count; j++) {
		double residual = y[j];
		for (unsigned a = 0; a < taps; a++) {
			residual -= fit->coefficient[a] * q[j - a];
			for (unsigned b = 0; b <= a; b++)
				m.gram[gram_moment_index(a, b)] += (double)q[j - a] * q[j - b];
		}
		noise += residual * residual;
	}
	double initial_noise = fmax(1., noise / (count - taps));
	if (fit_bin_channel_newton(fit->coefficient, q, low, high, count, taps, initial_noise, variance))
		return true;
	return refine_bin_fit(fit->coefficient, q, low, high, count, taps, taps,
		&m, initial_noise, true, variance);
}

/* The first-codec gain intervals come from the initial center fit. Select
 * noise from the best likelihood fit, then compare all retained hypotheses
 * under that common variance and their log-uniform gain prior. */
static bool refit_bins_begin(struct v92_observation_fit_work *w,
	const int16_t *y, const int16_t *levels, const double *offsets, size_t count)
{
	struct v92_observation *model = &w->model;
	for (size_t j = 0; j < count; j++) {
		double lower, upper;
		if (!codec_bounds(model->law, model->quantizer_gain, y[j],
			levels ? levels + j : NULL, offsets ? offsets[j] : 0., &lower, &upper))
			return false;
		w->lower[j] = y[j] + lower;
		w->upper[j] = y[j] + upper;
	}
	return true;
}

static void refit_bins_step(struct v92_observation_fit_work *w,
	const int32_t *x, const int16_t *y, size_t count, unsigned i)
{
	struct v92_observation *model = &w->model;
	struct v92_observation_interval *fit = &model->intervals[i];
	double gain = sqrt(fit->low * fit->high);
	for (size_t j = 0; j < count; j++) {
		w->bins[i][j] = g711_quantize(model->law, sample_round_s16(gain * x[j]));
	}
	w->costs[i] = fit_bin_channel(fit, w->bins[i], y, w->lower, w->upper, count, model->taps, w->variances + i) ?
		bin_cost(w->bins[i], w->lower, w->upper, count, model->taps, fit, w->variances[i]) : HUGE_VAL;
}

static bool refit_bins_finish(struct v92_observation_fit_work *w, size_t count)
{
	struct v92_observation *model = &w->model;
	double best = HUGE_VAL, total = 0.;
	unsigned chosen = 0;
	for (unsigned i = 0; i < INTERVALS; i++)
		if (w->costs[i] < best) { best = w->costs[i]; chosen = i; }
	if (!isfinite(best)) return false;
	model->variance = w->variances[chosen];
	best = HUGE_VAL;
	for (unsigned i = 0; i < INTERVALS; i++) {
		if (isfinite(w->costs[i]))
			w->costs[i] = bin_cost(w->bins[i], w->lower, w->upper, count, model->taps, model->intervals + i, model->variance);
		best = fmin(best, w->costs[i]);
	}
	for (unsigned i = 0; i < INTERVALS; i++) {
		struct v92_observation_interval *fit = &model->intervals[i];
		fit->weight = exp(-.5 * (w->costs[i] - best)) *
			log(fit->high / fit->low);
		total += fit->weight;
	}
	if (!(total > 0.)) return false;
	for (unsigned i = 0; i < INTERVALS; i++) model->intervals[i].weight /= total;
	return true;
}

static int16_t scaled(enum g711_law law, double gain, int32_t x)
{
	/* An inferred reciprocal host gain can overflow for a subnormal gain.
	 * Zero still maps to codec zero, not infinity times zero. */
	if (x == 0) return g711_quantize(law, 0);
	return g711_quantize(law, sample_round_s16(gain * x));
}

/* Bounds relative to the host sample. Use the same arithmetic for training
 * and scoring, including subnormal gain and one-sided codec clipping. */
static bool codec_bounds(enum g711_law law, double gain, int16_t observed,
	const int16_t *level, double offset, double *lower, double *upper)
{
	if (!isfinite(offset) || (offset != 0. && !level)) return false;
	int16_t reconstructed = level ? *level : scaled(law, 1. / gain, observed);
	struct g711_interval interval;
	if (g711_quantize(law, reconstructed) != reconstructed ||
		!g711_quantization_interval(law, reconstructed, &interval)) return false;
	*lower = (interval.lower == INT16_MIN ? -HUGE_VAL : (interval.lower - .5) * gain) + offset - observed;
	*upper = (interval.upper == INT16_MAX ? HUGE_VAL : (interval.upper + .5) * gain) + offset - observed;
	/* A finite but unrepresentably large shift can erase the interval's
	 * width. That is not a point observation with a perfect residual. */
	return offset == 0. || *lower < *upper;
}

static int16_t magnitude_code(enum g711_law law, int32_t x, unsigned magnitude)
{
	return g711_quantize(law, (int16_t)(x < 0 ? -(int32_t)magnitude : (int32_t)magnitude));
}

static struct v92_observation_fit_boundary next_boundary(enum g711_law law, int32_t x, unsigned magnitude)
{
	unsigned limit = x < 0 ? 32768u : 32767u;
	struct v92_observation_fit_boundary boundary = { .at = HUGE_VAL, .next = limit };
	if (x == 0) return boundary;
	int16_t sample = (int16_t)(x < 0 ? -(int32_t)magnitude : (int32_t)magnitude);
	struct g711_interval bin;
	if (!g711_quantization_interval(law, sample, &bin)) return boundary;
	int edge = x < 0 ? bin.lower : bin.upper;
	if (edge == (x < 0 ? INT16_MIN : INT16_MAX)) return boundary;
	/* The shared encoder interval includes clipping and the asymmetric
	 * negative A-law boundary, including the change from zero to -1. */
	unsigned next = x < 0 ? (unsigned)(1 - edge) : (unsigned)(edge + 1);
	boundary.next = next;
	boundary.at = (next - .5) / fabs((double)x);
	return boundary;
}

/* One log-uniform gain applies to every tap. Retain each coordinate's next
 * codec transition, then advance all coincident boundaries together. Cached
 * cells may replace a complete codec walk with zero or one transition. No
 * borrowed model, input or cell pointers survive initialization. */
struct gain_sweep {
	struct v92_observation_fit_boundary boundary[TAPS];
	int16_t value[TAPS], last[TAPS];
	bool general[TAPS];
	double begin, end, high;
	unsigned taps, steps;
	bool failed;
};

static inline struct gain_sweep gain_sweep_begin(enum g711_law law,
	const int32_t *input, unsigned taps, double low, double high,
	const struct v92_observation_cell *const *cells)
{
	struct gain_sweep sweep = { .begin = low, .high = high, .taps = taps };
	for (unsigned i = 0; i < taps; i++) {
		const struct v92_observation_cell *cell = cells ? cells[i] : NULL;
		sweep.general[i] = !cell || (cell->low != cell->high && !cell->crossing);
		if (sweep.general[i]) {
			unsigned limit = input[i] < 0 ? 32768u : 32767u;
			double sample = low * fabs((double)input[i]);
			unsigned magnitude = sample >= limit ? limit : (unsigned)round(sample);
			sweep.value[i] = cell ? cell->low : magnitude_code(law, input[i], magnitude);
			sweep.boundary[i] = next_boundary(law, input[i], magnitude);
		} else {
			sweep.value[i] = cell->low;
			sweep.last[i] = cell->high;
			sweep.boundary[i].at = cell->low == cell->high ?
				HUGE_VAL : (cell->crossing - .5) / fabs((double)input[i]);
		}
	}
	return sweep;
}

static inline bool gain_sweep_next(struct gain_sweep *sweep,
	enum g711_law law, const int32_t *input)
{
	if (sweep->steps) {
		if (sweep->end == sweep->high) return false;
		for (unsigned i = 0; i < sweep->taps; i++) {
			if (sweep->boundary[i].at != sweep->end) continue;
			if (sweep->general[i]) {
				unsigned magnitude = sweep->boundary[i].next;
				sweep->value[i] = magnitude_code(law, input[i], magnitude);
				sweep->boundary[i] = next_boundary(law, input[i], magnitude);
			} else {
				sweep->value[i] = sweep->last[i];
				sweep->boundary[i].at = HUGE_VAL;
			}
		}
		sweep->begin = sweep->end;
	}
	sweep->end = sweep->high;
	for (unsigned i = 0; i < sweep->taps; i++)
		sweep->end = fmin(sweep->end, sweep->boundary[i].at);
	/* Negative A-law also visits +8 at rounded zero: at most 128
	 * transitions per coordinate, plus the final constant segment. */
	sweep->failed = sweep->end < sweep->begin || sweep->steps++ > sweep->taps * 128u;
	return !sweep->failed;
}

static void heap_down(size_t *heap, size_t count, const struct v92_observation_fit_boundary *events)
{
	size_t parent = 0;
	while (2 * parent + 1 < count) {
		size_t child = 2 * parent + 1;
		if (child + 1 < count && events[heap[child + 1]].at < events[heap[child]].at) child++;
		if (events[heap[parent]].at <= events[heap[child]].at) return;
		size_t swap = heap[parent]; heap[parent] = heap[child]; heap[child] = swap;
		parent = child;
	}
}

/* For an interior q[j] change, every Gram entry at lag d gains the same
 * change * (q[j-d] + q[j+d]); diagonal entries share the square difference.
 * Integer samples and at most 288 rows keep all products and intermediate
 * sums exact in binary64, so grouping these terms preserves the fit. */
static inline void update_interior_moments(struct v92_observation_fit_moments *m,
	const int16_t *q, const int16_t *y, unsigned taps,
	size_t j, double change, double square)
{
	for (unsigned a = 0; a < taps; a++) {
		m->rhs[a] += change * y[j + a];
		m->gram[gram_moment_index(a, a)] += square;
	}
	for (unsigned lag = 1; lag < taps; lag++) {
		double delta = change * ((double)q[j - lag] + q[j + lag]);
		for (unsigned col = 0; col + lag < taps; col++)
			m->gram[gram_moment_index(col + lag, col)] += delta;
	}
}

static void fit_intervals_begin(struct v92_observation_fit_work *w,
	enum g711_law law, const int32_t *x, const int16_t *y, size_t count, unsigned taps)
{
	for (size_t i = 0; i < INTERVALS; i++) w->fits[i] = (struct v92_observation_fit_interval){ .error = HUGE_VAL };
	for (size_t j = 1; j < count; j++) {
		unsigned limit = x[j] < 0 ? 32768u : 32767u;
		unsigned magnitude = (unsigned)fmin(limit, round(w->low * fabs((double)x[j])));
		w->q[j] = magnitude_code(law, x[j], magnitude);
		w->events[j] = next_boundary(law, x[j], magnitude);
		size_t child = w->used++;
		w->heap[child] = j;
		while (child) {
			size_t parent = (child - 1) / 2;
			if (w->events[w->heap[parent]].at <= w->events[w->heap[child]].at) break;
			size_t swap = w->heap[parent]; w->heap[parent] = w->heap[child]; w->heap[child] = swap;
			child = parent;
		}
	}
	for (size_t j = taps; j < count; j++) {
		for (unsigned a = 0; a < taps; a++) {
			w->moments.rhs[a] += (double)w->q[j - a] * y[j];
			for (unsigned b = 0; b <= a; b++)
				w->moments.gram[gram_moment_index(a, b)] += (double)w->q[j - a] * w->q[j - b];
		}
		w->moments.energy += (double)y[j] * y[j];
	}
}

static int fit_intervals_step(struct v92_observation_fit_work *w,
	enum g711_law law, const int32_t *x, const int16_t *y, size_t count, unsigned taps)
{
	/* Every known input crosses finitely many codec boundaries. Search all
	 * bounded cells; the all-saturated tail has no identifiable gain and
	 * cannot carry a proper log-uniform prior. */
	unsigned budget = 1024;
	while (budget) {
		double high = w->pending ? w->end : w->events[w->heap[0]].at;
		if (!isfinite(high)) return isfinite(w->fits[INTERVALS - 1].error) ? 1 : -1;

		if (!w->pending) {
			if (!(high > w->low)) return -1;
			struct v92_observation_fit_interval fit = { .low = w->low, .high = high };
			if (w->low > 0. && solve_channel(&w->moments, taps, fit.coefficient)) {
				double error = w->moments.energy;
				for (unsigned i = 0; i < taps; i++) error -= fit.coefficient[i] * w->moments.rhs[i];
				fit.error = fmax(0., error / (count - taps));
				if (isfinite(fit.error)) {
					for (size_t i = 0; i < INTERVALS; i++) {
						if (fit.error >= w->fits[i].error) continue;
						memmove(w->fits + i + 1, w->fits + i, (INTERVALS - i - 1) * sizeof(*w->fits));
						w->fits[i] = fit;
						break;
					}
				}
			}
			w->pending = true;
			w->end = high;
			budget--;
		}
		while (w->events[w->heap[0]].at == high) {
			if (!budget) return 0;
			budget--;
			size_t j = w->heap[0];
			int16_t value = magnitude_code(law, x[j], w->events[j].next);
			double change = value - w->q[j], square = (double)value * value - (double)w->q[j] * w->q[j];
			/* Window edges omit different rows and retain their individual updates. */
			if (j >= taps && j + taps <= count) {
				if (taps == 3)
					update_interior_moments(&w->moments, w->q, y, 3, j, change, square);
				else
					update_interior_moments(&w->moments, w->q, y, 4, j, change, square);
			} else {
				for (unsigned a = 0; a < taps; a++) {
					size_t row = j + a;
					if (row < taps || row >= count) continue;
					w->moments.rhs[a] += change * y[row];
					w->moments.gram[gram_moment_index(a, a)] += square;
					/* Each affected row contributes once to its shared moment;
					 * both index orders still matter as w->q[j] moves. */
					for (unsigned b = 0; b < taps; b++) if (b != a) {
						double delta = change * w->q[row - b];
						w->moments.gram[gram_moment_index(a, b)] += delta;
					}
				}
			}
			w->q[j] = value;
			w->events[j] = next_boundary(law, x[j], w->events[j].next);
			heap_down(w->heap, w->used, w->events);
		}
		w->low = high;
		w->pending = false;
	}
	return 0;
}

/* Integral of Q(k*x) for log-uniform k. Every segment is an exact codec cell;
 * endpoint ties have zero measure. No sampled gain grid or quadrature count. */
static double interval_mean(enum g711_law law, const struct v92_observation_fit_interval *fit, int32_t x)
{
	int16_t first = scaled(law, fit->low, x), last = scaled(law, fit->high, x);
	if (first == last) return first;
	double integral = 0;
	double width = log1p((fit->high - fit->low) / fit->low);
	struct gain_sweep sweep = gain_sweep_begin(law, &x, 1u, fit->low, fit->high, NULL);
	while (gain_sweep_next(&sweep, law, &x))
		integral += sweep.value[0] * log1p((sweep.end - sweep.begin) / sweep.begin);
	return sweep.failed ? NAN : integral / width;
}

static int compare_steps(const void *left, const void *right)
{
	int32_t a = *(const int32_t *)left;
	int32_t b = *(const int32_t *)right;
	return a < b ? -1 : a > b;
}

/* A power-of-two bucket width covers every finite codec transition.
 * first[0] is the INT32_MIN sentinel; the other signed anchors ensure a
 * positive extent. Derive this from immutable geometry, not payload. */
static unsigned bucket_shift(const struct v92_observation *model)
{
	uint32_t negative = (uint32_t)(-(int64_t)model->first[1]);
	uint32_t positive = (uint32_t)model->first[model->count - 1];
	uint32_t extent = negative > positive ? negative : positive;
	uint32_t cells = (extent - 1u) / (V92_OBSERVATION_BUCKETS / 2u);
	return bit_width_unsigned(cells);
}

static int64_t bucket_edge(unsigned bucket, unsigned shift)
{
	return ((int64_t)bucket - V92_OBSERVATION_BUCKETS / 2u) * (INT64_C(1) << shift);
}

static bool compile_geometry(struct v92_observation *model)
{
	model->count = 4;
	model->first[0] = INT32_MIN;
	model->first[1] = -1;
	model->first[2] = 0;
	model->first[3] = 1;
	for (size_t i = 0; i < INTERVALS; i++) for (unsigned end = 0; end < 2; end++) {
		/* Only exact zero is inactive; compile the final refined weights. */
		if (model->intervals[i].weight == 0.0) continue;
		double gain = end ? model->intervals[i].high : model->intervals[i].low;
		int32_t first = INT32_MIN;
		int16_t value = scaled(model->law, gain, first), last = scaled(model->law, gain, INT32_MAX);
		while (value != last) {
			int64_t low = (int64_t)first + 1, high = INT32_MAX;
			while (low < high) {
				int64_t middle = low + (high - low) / 2;
				if (scaled(model->law, gain, (int32_t)middle) > value) high = middle;
				else low = middle + 1;
			}
			first = (int32_t)low;
			value = scaled(model->law, gain, first);
			if (model->count == V92_OBSERVATION_STEPS) return false;
			model->first[model->count++] = first;
		}
	}
	qsort(model->first, model->count, sizeof(model->first[0]), compare_steps);
	size_t count = 0;
	for (size_t i = 0; i < model->count; i++) {
		if (count && model->first[i] == model->first[count - 1]) continue;
		model->first[count++] = model->first[i];
	}
	model->count = count;
	memset(model->first + count, 0, (V92_OBSERVATION_STEPS - count) * sizeof(model->first[0]));
	size_t span = 0;
	unsigned shift = bucket_shift(model);
	for (unsigned bucket = 0; bucket <= V92_OBSERVATION_BUCKETS; bucket++) {
		int64_t first = bucket_edge(bucket, shift);
		while (span + 1 < count && model->first[span + 1] <= first) span++;
		model->bucket[bucket] = (uint16_t)span;
	}
	return true;
}

bool v92_observation_is_valid(const struct v92_observation *model)
{
	if (!model || (model->taps != 3 && model->taps != 4) || model->count < 4 || model->count > V92_OBSERVATION_STEPS ||
		(model->law != G711_LAW_PCMU && model->law != G711_LAW_PCMA) ||
		!isfinite(model->variance) || model->variance < 1. ||
		!isfinite(model->quantizer_gain) || model->quantizer_gain <= 0. ||
		model->input_coefficient < 0 ||
		!isfinite(model->energy) || model->energy <= 0. || model->first[0] != INT32_MIN)
		return false;
	double total = 0.;
	for (size_t i = 0; i < INTERVALS; i++) {
		const struct v92_observation_interval *fit = model->intervals + i;
		if (!isfinite(fit->low) || !isfinite(fit->high) || fit->low <= 0. ||
			fit->high <= fit->low || fit->coefficient[0] <= 0. ||
			!isfinite(fit->weight) || fit->weight < 0. || fit->weight > 1.) return false;
		for (unsigned j = 0; j < TAPS; j++)
			if (!isfinite(fit->coefficient[j]) || (j >= model->taps && fit->coefficient[j] != 0.)) return false;
		total += fit->weight;
	}
	if (fabs(total - 1.) > 32 * DBL_EPSILON) return false;
	bool negative = false, zero = false, positive = false;
	for (size_t i = 0; i < model->count; i++) {
		if (i && model->first[i] <= model->first[i - 1]) return false;
		negative |= model->first[i] == -1; zero |= model->first[i] == 0; positive |= model->first[i] == 1;
	}
	if (!negative || !zero || !positive) return false;
	unsigned shift = bucket_shift(model);
	for (unsigned bucket = 0; bucket <= V92_OBSERVATION_BUCKETS; bucket++) {
		size_t span = model->bucket[bucket];
		int64_t first = bucket_edge(bucket, shift);
		if (span >= model->count || model->first[span] > first ||
			(span + 1 < model->count && model->first[span + 1] <= first)) return false;
	}
	return true;
}

static bool training_is_valid(const void *output, size_t size,
	const struct v92_observation_training *training)
{
	if (!output || !training ||
		memory_regions_overlap(output, size, training, sizeof(*training))) return false;
	const int32_t *x = training->input;
	const int16_t *u = training->linear, *y = training->sample, *levels = training->levels;
	const double *offsets = training->offsets;
	size_t count = training->count;
	enum g711_law law = training->law;
	if (!x || !u || !y || count < 4 || count > V92_OBSERVATION_TRAINING ||
		(law != G711_LAW_PCMU && law != G711_LAW_PCMA) ||
		!isfinite(training->host_gain) || training->host_gain <= 0. ||
		memory_regions_overlap(output, size, x, count * sizeof(*x)) ||
		memory_regions_overlap(output, size, u, count * sizeof(*u)) ||
		memory_regions_overlap(output, size, y, count * sizeof(*y)) ||
		(levels && memory_regions_overlap(output, size, levels, count * sizeof(*levels))) ||
		(offsets && (!levels || memory_regions_overlap(output, size, offsets,
			count * sizeof(*offsets))))) return false;
	if (offsets)
		for (size_t i = 0; i < count; i++)
			if (!isfinite(offsets[i])) return false;
	return true;
}

/* Compare both hypotheses against the actual final-codec cells using the
 * same noise variance. Extra codec structure must improve on the existing
 * linear prediction, not just move closer to quantized sample centres. */
static bool prefer_linear(const struct v92_observation *model,
	const struct v92_observation_training *training)
{
	if (!training->levels) return false;
	uint64_t linear = 0;
	double divisor = sqrt(2. * model->variance);
	for (size_t i = model->taps; i < training->count; ++i) {
		double lower, upper;
		if (!codec_bounds(training->law, training->host_gain, training->sample[i],
			training->levels + i, training->offsets ? training->offsets[i] : 0.,
			&lower, &upper)) return false;
		double error = (double)training->sample[i] - training->linear[i];
		double probability = gaussian_log_probability((error + lower) / divisor,
			(error + upper) / divisor);
		if (!isfinite(probability)) return false;
		uint64_t cost = score_value(-2. * V92_SCORE_SCALE * probability);
		if (cost == UINT64_MAX || cost > UINT64_MAX - linear) return false;
		linear += cost;
	}
	if (!linear) return true;
	struct v92_observation_cache cache;
	v92_observation_begin(&cache, model, 0.);
	uint64_t fitted = 0;
	for (size_t i = model->taps; i < training->count; ++i) {
		if (!v92_observation_symbol_level(&cache, training->sample[i],
			training->levels + i, training->offsets ? training->offsets[i] : 0.))
			return false;
		struct v92_observation_point previous[V92_OBSERVATION_TAPS - 1];
		for (unsigned tap = 1; tap < model->taps; ++tap)
			previous[tap - 1] = v92_observation_point(&cache, training->input[i - tap]);
		uint64_t cost = v92_observation_score(&cache, training->input[i], previous,
			linear - fitted);
		/* The scorer may return a lower bound once this budget is reached.
		 * Nonnegative later costs cannot restore a win. */
		if (cost >= linear - fitted) return true;
		fitted += cost;
	}
	return false;
}

/* If the unchanged linear predictor lies strictly inside every actual codec
 * cell, its zero-noise likelihood is already maximal. Do not let the numeric
 * variance floor reward unnecessary structure that moves within those cells.
 * This is model admission, not a noise estimate for the remaining channels. */
static bool linear_explains_cells(const struct v92_observation_training *training)
{
	if (!training->levels) return false;
	for (size_t i = training->taps; i < training->count; i++) {
		double lower, upper;
		if (!codec_bounds(training->law, training->host_gain, training->sample[i],
			training->levels + i, training->offsets ? training->offsets[i] : 0.,
			&lower, &upper)) return false;
		double error = (double)training->sample[i] - training->linear[i];
		if (!(error + lower < 0. && error + upper > 0.)) return false;
	}
	return true;
}

/* A positive scalar may explain all wire cells even when gain one does
 * not. Intersect their open gain constraints, then check the actual integer
 * codec: a rounded half-boundary is not proof of a noiseless explanation. */
static bool scalar_explains_cells(const struct v92_observation_training *training)
{
	if (!training->levels) return false;
	double low = 0., high = HUGE_VAL;
	for (size_t i = training->taps; i < training->count; ++i) {
		double lower, upper, x = training->linear[i];
		if (!codec_bounds(training->law, training->host_gain, training->sample[i],
			training->levels + i, training->offsets ? training->offsets[i] : 0.,
			&lower, &upper)) return false;
		lower += training->sample[i]; upper += training->sample[i];
		if (x == 0.) {
			if (!(lower < 0. && upper > 0.)) return false;
			continue;
		}
		double a = lower / x, b = upper / x;
		low = fmax(low, x > 0. ? a : b);
		high = fmin(high, x > 0. ? b : a);
		if (!(low < high)) return false;
	}
	if (!isfinite(low) || !isfinite(high)) return false;
	/* Admission must agree with the scalar fallback actually used by the
	 * receiver, not just a different feasible gain. Keep that fitter and
	 * validate its reconstruction before declining a tandem hypothesis. */
	double gain;
	if (!v92_observation_fit_scalar(&gain, training) ||
	    !(gain > low && gain < high)) return false;
	for (size_t i = training->taps; i < training->count; ++i) {
		double offset = training->offsets ? training->offsets[i] : 0.;
		double physical = (gain * training->linear[i] - offset) / training->host_gain;
		if (!isfinite(physical) ||
		    g711_quantize(training->law, sample_round_s16(physical)) != training->levels[i])
			return false;
	}
	return true;
}

static bool initialize_fit(struct v92_observation_fit_work *w,
	const struct v92_observation_training *training)
{
	const int32_t *x = training->input;
	const int16_t *y = training->sample;
	size_t count = training->count;
	unsigned taps = training->taps;
	enum g711_law law = training->law;
	double energy = w->model.energy, linear_error = w->linear_error;
	w->model = (struct v92_observation){ .law = law, .taps = taps, .energy = energy / (count - taps),
		.variance = fmax(1., w->fits[0].error), .quantizer_gain = training->host_gain };
	double total = 0;
	for (size_t i = 0; i < INTERVALS; i++) {
		w->fits[i].weight = exp(-.5 * (count - taps) * (w->fits[i].error - w->fits[0].error) / w->model.variance) *
			log(w->fits[i].high / w->fits[i].low);
		total += w->fits[i].weight;
	}
	if (!(total > 0.) || !isfinite(total)) return false;
	for (size_t i = 0; i < INTERVALS; i++) w->fits[i].weight /= total;
	double error_energy = 0;
	for (size_t j = taps; j < count; j++) {
		double predicted = 0;
		for (size_t i = 0; i < INTERVALS; i++)
			for (unsigned k = 0; k < taps; k++)
				predicted += w->fits[i].weight * w->fits[i].coefficient[k] * interval_mean(law, w->fits + i, x[j - k]);
		double error = y[j] - predicted;
		error_energy += error * error;
	}
	if (!isfinite(error_energy) || error_energy >= linear_error) return false;
	for (size_t i = 0; i < INTERVALS; i++) {
		w->model.intervals[i] = (struct v92_observation_interval){
			.low = w->fits[i].low, .high = w->fits[i].high, .weight = w->fits[i].weight };
		memcpy(w->model.intervals[i].coefficient, w->fits[i].coefficient, sizeof(w->fits[i].coefficient));
	}
	return true;
}

bool v92_observation_fit_begin(struct v92_observation_fit_work *w,
	const struct v92_observation_training *training)
{
	if (!training_is_valid(w, sizeof(*w), training) ||
		(training->taps != 3 && training->taps != 4) || training->count <= training->taps) return false;
	double energy = 0., linear_error = 0.;
	for (size_t i = training->taps; i < training->count; i++) {
		double error = (double)training->sample[i] - training->linear[i];
		energy += (double)training->sample[i] * training->sample[i]; linear_error += error * error;
	}
	memset(w, 0, sizeof(*w));
	if (energy == 0. || linear_error == 0. || linear_explains_cells(training) ||
		scalar_explains_cells(training)) return false;
	w->linear_error = linear_error;
	w->model.energy = energy;
	fit_intervals_begin(w, training->law, training->input, training->sample, training->count, training->taps);
	w->stage = V92_FIT_SWEEP;
	return true;
}

enum v92_observation_fit_result v92_observation_fit_step(struct v92_observation_fit_work *w,
	const struct v92_observation_training *training, struct v92_observation *output)
{
	const int32_t *x = training->input;
	const int16_t *y = training->sample;
	size_t count = training->count;
	enum g711_law law = training->law;
	if (w->stage == V92_FIT_SWEEP) {
		int result = fit_intervals_step(w, law, x, y, count, training->taps);
		if (result < 0) goto decline;
		if (result) w->stage = V92_FIT_CENTER;
	} else if (w->stage == V92_FIT_CENTER) {
		if (!initialize_fit(w, training)) goto decline;
		w->stage = V92_FIT_BOUNDS;
	} else if (w->stage == V92_FIT_BOUNDS) {
		if (!refit_bins_begin(w, y, training->levels, training->offsets, count)) goto decline;
		w->stage = V92_FIT_REFINE;
	} else if (w->stage == V92_FIT_REFINE) {
		refit_bins_step(w, x, y, count, w->cursor++);
		if (w->cursor == INTERVALS) w->stage = V92_FIT_WEIGHTS;
	} else if (w->stage == V92_FIT_WEIGHTS) {
		if (!refit_bins_finish(w, count)) goto decline;
		w->stage = V92_FIT_GEOMETRY;
	} else if (w->stage == V92_FIT_GEOMETRY) {
		if (!compile_geometry(&w->model) || !v92_observation_is_valid(&w->model)) goto decline;
		w->stage = V92_FIT_VALIDATE;
	} else if (w->stage == V92_FIT_VALIDATE) {
		if (prefer_linear(&w->model, training)) goto decline;
		*output = w->model;
		w->stage = V92_FIT_IDLE;
		return V92_OBSERVATION_FIT_READY;
	} else goto decline;
	return V92_OBSERVATION_FIT_MORE;
decline:
	w->stage = V92_FIT_IDLE;
	return V92_OBSERVATION_FIT_DECLINED;
}

bool v92_observation_fit(struct v92_observation *model,
	const struct v92_observation_training *training)
{
	if (!training_is_valid(model, sizeof(*model), training)) return false;
	struct v92_observation_fit_work work;
	if (!v92_observation_fit_begin(&work, training)) return false;
	enum v92_observation_fit_result result;
	do {
		result = v92_observation_fit_step(&work, training, model);
	} while (result == V92_OBSERVATION_FIT_MORE);
	return result == V92_OBSERVATION_FIT_READY;
}

double v92_observation_predict_scalar(enum g711_law law, double gain,
	double host_gain, int16_t linear, double offset)
{
	if ((law != G711_LAW_PCMU && law != G711_LAW_PCMA) ||
	    !isfinite(gain) || gain <= 0. || !isfinite(host_gain) || host_gain <= 0. ||
	    !isfinite(offset)) return NAN;
	double physical = (gain * linear - offset) / host_gain;
	if (!isfinite(physical)) return NAN;
	int16_t sample = sample_round_s16(physical);
	double prediction = host_gain * g711_quantize(law, sample) + offset;
	return isfinite(prediction) ? prediction : NAN;
}

bool v92_observation_fit_scalar(double *gain,
	const struct v92_observation_training *training)
{
	if (!training_is_valid(gain, sizeof(*gain), training) || !training->levels) return false;
	double low[V92_OBSERVATION_TRAINING], high[V92_OBSERVATION_TRAINING];
	double xx = 0., xy = 0., linear_error = 0.;
	size_t count = training->count;
	for (size_t i = 0; i < count; i++) {
		double lower, upper, x = training->linear[i], y = training->sample[i];
		if (!codec_bounds(training->law, training->host_gain, training->sample[i],
			training->levels + i, training->offsets ? training->offsets[i] : 0.,
			&lower, &upper)) return false;
		low[i] = y + lower; high[i] = y + upper;
		xx += x * x; xy += x * y; linear_error += (x - y) * (x - y);
	}
	if (!(xx > 0. && xy > 0. && linear_error > 0.)) return false;
	double coefficient[TAPS] = { xy / xx }, variance;
	struct v92_observation_fit_moments m = { .gram = { xx } };
	if (!refine_bin_fit(coefficient, training->linear, low, high, count, 0, 1,
		&m, fmax(1., linear_error / count), false, &variance)) return false;
	double fitted = coefficient[0];

	double error_energy = 0.;
	for (size_t i = 0; i < count; i++) {
		double predicted = v92_observation_predict_scalar(training->law, fitted,
			training->host_gain, training->linear[i], training->offsets ? training->offsets[i] : 0.);
		double error = training->sample[i] - predicted;
		error_energy += error * error;
	}
	if (!isfinite(error_energy) || error_energy >= linear_error) return false;
	*gain = fitted;
	return true;
}

static size_t find_span(const struct v92_observation_cache *cache, int32_t x)
{
	const struct v92_observation *model = cache->model;
	unsigned shift = cache->bucket_shift;
	int64_t extent = cache->bucket_extent;
	if (x >= extent) return model->count - 1;
	if (x < -extent) return 0;
	unsigned bucket = (unsigned)(((int64_t)x + extent) >> shift);
	size_t low = model->bucket[bucket];
	if (!shift) return low; /* Every integer coordinate has its own bucket. */
	size_t high = model->bucket[bucket + 1] + 1u;
	while (low + 1 < high) {
		size_t middle = low + (high - low) / 2;
		if (model->first[middle] <= x) low = middle;
		else high = middle;
	}
	return low;
}

static inline struct v92_observation_geometry *point_geometry(struct v92_observation_cache *cache)
{
	return cache->geometry ? cache->geometry : &cache->local_geometry;
}

/* Candidate envelopes repeatedly visit the same transformed coordinates.
 * Retain only exact integer lookups, never an observation-dependent score. */
static inline size_t point_span(struct v92_observation_cache *cache, int32_t x)
{
	struct v92_observation_geometry *geometry = point_geometry(cache);
	unsigned slot = ((uint32_t)x * UINT32_C(2654435761)) >> 22;
	if (geometry->spans[slot].tag && geometry->spans[slot].key == x)
		return geometry->spans[slot].tag - 1u;
	size_t span = find_span(cache, x);
	geometry->spans[slot].key = x;
	geometry->spans[slot].tag = (uint16_t)(span + 1u);
	return span;
}

/* Log probability of the observed final-codec bin under one Gaussian mean. */
static double bin_log_probability(double error, double variance, double divisor, double lower, double upper)
{
	if (lower == upper) return -.5 * error * error / variance;
	return gaussian_log_probability((error + lower) / divisor, (error + upper) / divisor);
}

/* The ordinary-probability and log-domain paths integrate exactly the same
 * shared gain partition. Logs retain ordering in unrepresentable tails. */
static double bin_likelihood(double error, double variance, double divisor,
	double lower, double upper, bool logarithmic)
{
	if (logarithmic) return bin_log_probability(error, variance, divisor, lower, upper);
	return lower == upper ? exp(-.5 * error * error / variance) :
		gaussian_probability((error + lower) / divisor, (error + upper) / divisor);
}

/* General per-observation integral: all symbols share the same log-uniform
 * codec scale. Merge their exact quantizer boundaries, not independent means. */
static double compiled_likelihood(enum g711_law law, const struct v92_observation_interval *fit,
	const struct v92_observation_cell *const *cells, const int32_t *x, unsigned taps,
	int16_t observed, double variance, double divisor, double width, double lower, double upper,
	bool logarithmic)
{
	double likelihood = logarithmic ? -HUGE_VAL : 0.;
	bool stable = true;
	for (unsigned i = 0; i < taps; i++) stable &= cells[i]->low == cells[i]->high;
	if (stable) {
		double error = observed;
		for (unsigned i = 0; i < taps; i++) error -= fit->coefficient[i] * cells[i]->low;
		return bin_likelihood(error, variance, divisor, lower, upper, logarithmic);
	}
	struct gain_sweep sweep = gain_sweep_begin(law, x, taps, fit->low, fit->high, cells);
	while (gain_sweep_next(&sweep, law, x)) {
		double error = observed;
		for (unsigned i = 0; i < taps; i++) error -= fit->coefficient[i] * sweep.value[i];
		if (sweep.end > sweep.begin) {
			double probability = bin_likelihood(error, variance, divisor, lower, upper, logarithmic);
			double weight = log1p((sweep.end - sweep.begin) / sweep.begin) / width;
			likelihood = logarithmic ? log_add(likelihood, probability + log(weight)) :
				likelihood + probability * weight;
		}
	}
	return sweep.failed ? NAN : likelihood;
}

void v92_observation_geometry_reset(struct v92_observation_geometry *geometry)
{
	memset(geometry->point_tag, 0, sizeof(geometry->point_tag));
	memset(geometry->spans, 0, sizeof(geometry->spans));
	geometry->rounding = fegetround();
}

void v92_observation_begin(struct v92_observation_cache *cache,
	const struct v92_observation *model, double relative_noise)
{
	v92_observation_begin_cached(cache, model, relative_noise, NULL);
}

void v92_observation_begin_cached(struct v92_observation_cache *cache,
	const struct v92_observation *model, double relative_noise,
	struct v92_observation_geometry *geometry)
{
	cache->geometry = geometry;
	int rounding = fegetround();
	if (!geometry || rounding < 0 || geometry->rounding != rounding)
		v92_observation_geometry_reset(point_geometry(cache));
	memset(cache->stamp, 0, sizeof(cache->stamp));
	cache->generation = 0;
	cache->model = model;
	cache->bucket_shift = bucket_shift(model);
	cache->bucket_extent = (int64_t)(V92_OBSERVATION_BUCKETS / 2u) << cache->bucket_shift;
	cache->relative_noise = relative_noise;
	cache->observed = 0;
	cache->variance = cache->bound_scale = 0.;
	cache->divisor = 0.;
	double extent[TAPS] = { 0. };
	for (size_t i = 0; i < INTERVALS; i++) {
		const struct v92_observation_interval *fit = model->intervals + i;
		cache->width[i] = log1p((fit->high - fit->low) / fit->low);
		cache->log_weight[i] = fit->weight > 0. ? log(fit->weight) : -HUGE_VAL;
		if (fit->weight > 0.)
			for (unsigned j = 0; j < model->taps; j++)
				extent[j] = fmax(extent[j], fabs(fit->coefficient[j]));
	}
	for (unsigned i = 0; i < TAPS; i++) cache->margin[i] = 1e-12 * (1. + 65536. * extent[i]);
}

bool v92_observation_symbol_level(struct v92_observation_cache *cache,
	int16_t observed, const int16_t *level, double offset)
{
	const struct v92_observation *model = cache->model;
	if (!isfinite(cache->relative_noise) || cache->relative_noise < 0.) return false;
	double variance = model->variance * (1. + cache->relative_noise * (double)observed * observed) /
		(1. + cache->relative_noise * model->energy);
	double scale = V92_SCORE_SCALE / variance;
	if (!isfinite(variance) || variance <= 0. || !isfinite(scale) || scale <= 0.) return false;
	double lower, upper;
	if (!codec_bounds(model->law, model->quantizer_gain, observed, level, offset, &lower, &upper)) return false;
	if (++cache->generation == 0) {
		memset(cache->stamp, 0, sizeof(cache->stamp));
		cache->generation = 1;
	}
	cache->observed = observed;
	cache->lower = lower;
	cache->upper = upper;
	cache->variance = variance;
	cache->bound_scale = scale;
	cache->divisor = sqrt(2. * variance);
	return true;
}

bool v92_observation_symbol(struct v92_observation_cache *cache, int16_t observed)
{
	return v92_observation_symbol_level(cache, observed, NULL, 0.);
}

/* Taps are finite and codec levels are bounded integers. Products may
 * overflow to an infinity, but cannot be NaN, so ordered comparisons have
 * the same envelope as nested fmin/fmax without repeated library calls. */
static void extend_envelope(double *low, double *high, double tap, int16_t first, int16_t last)
{
	double a = tap * first, b = tap * last;
	if (a > b) { double swap = a; a = b; b = swap; }
	if (a < *low) *low = a;
	if (b > *high) *high = b;
}

static const struct v92_observation_values *load_point_values(struct v92_observation_cache *cache, size_t span)
{
	struct v92_observation_geometry *geometry = point_geometry(cache);
	size_t slot = span & (V92_OBSERVATION_POINTS - 1), chosen = slot;
	for (size_t probe = 0; probe < 4; probe++) {
		size_t index = (slot + probe) & (V92_OBSERVATION_POINTS - 1);
		if (geometry->point_tag[index] == span + 1) return geometry->points + index;
		if (!geometry->point_tag[index]) { chosen = index; break; }
	}
	const struct v92_observation *model = cache->model;
	int32_t x = model->first[span];
	struct v92_observation_values *values = geometry->points + chosen;
	*values = (struct v92_observation_values){ .stable = true };
	for (unsigned j = 0; j < model->taps; j++) {
		values->low[j] = HUGE_VAL;
		values->high[j] = -HUGE_VAL;
	}
	for (size_t i = 0; i < INTERVALS; i++) {
		const struct v92_observation_interval *fit = model->intervals + i;
		if (fit->weight == 0.) continue;
		struct v92_observation_cell *cell = values->cell + i;
		cell->low = scaled(model->law, fit->low, x); cell->high = scaled(model->law, fit->high, x);
		for (unsigned j = 0; j < model->taps; j++)
			extend_envelope(&values->low[j], &values->high[j],
				fit->coefficient[j], cell->low, cell->high);
		if (cell->low != cell->high) {
			values->stable = false;
			unsigned limit = x < 0 ? 32768u : 32767u;
			unsigned magnitude = (unsigned)fmin(limit, round(fit->low * fabs((double)x)));
			struct v92_observation_fit_boundary boundary = next_boundary(model->law, x, magnitude);
			if (magnitude_code(model->law, x, boundary.next) == cell->high) cell->crossing = (uint16_t)boundary.next;
		}
	}
	/* Fixed per-model outward margins preserve monotonicity of the main
	 * envelope across point spans, including a plateau in one endpoint.
	 * A margin proportional to both current endpoints can decrease a
	 * plateau when only the other endpoint increases. */
	for (unsigned j = 0; j < model->taps; j++) {
		values->low[j] -= cache->margin[j];
		values->high[j] += cache->margin[j];
	}
	geometry->point_tag[chosen] = (uint16_t)(span + 1);
	return values;
}

/* Most geometry requests repeat an already visited span. Keep its direct
 * hit outside the codec-cell construction path. The bounded probe on a
 * miss retains the same replacement policy and owns all mutations. */
static inline const struct v92_observation_values *point_values(
	struct v92_observation_cache *cache, size_t span)
{
	struct v92_observation_geometry *geometry = point_geometry(cache);
	size_t slot = span & (V92_OBSERVATION_POINTS - 1);

	return geometry->point_tag[slot] == span + 1 ? geometry->points + slot :
		load_point_values(cache, span);
}

struct v92_observation_point v92_observation_point(struct v92_observation_cache *cache, int32_t x)
{
	if (cache->model->input_coefficient)
		x = round_shift_s64_to_s32_saturating((int64_t)x * cache->model->input_coefficient, 15u);
	size_t span = point_span(cache, x);
	struct v92_observation_values values = *point_values(cache, span);
	return (struct v92_observation_point){ values, x, values.stable ? cache->model->first[span] : x, span };
}

/* Build the same joint input for either numeric representation. */
static double observation_likelihood(const struct v92_observation_cache *cache,
	const struct v92_observation_values *a, const struct v92_observation_point *previous,
	int32_t x, bool logarithmic, struct v92_observation_history *history)
{
	const struct v92_observation *model = cache->model;
	double likelihood = logarithmic ? -HUGE_VAL : 0.;
	for (size_t i = 0; i < INTERVALS; i++) if (model->intervals[i].weight > 0.) {
		const struct v92_observation_interval *fit = model->intervals + i;
		const struct v92_observation_cell *current = a->cell + i;
		unsigned bit = 1u << i;
		if (history && !logarithmic && !(history->prepared & bit)) {
			bool stable = true;
			for (unsigned j = 1; j < model->taps; j++) {
				const struct v92_observation_cell *cell = previous[j - 1].values.cell + i;
				stable &= cell->low == cell->high;
				history->level[i][j - 1] = cell->low;
			}
			if (stable) history->stable |= bit;
			history->prepared |= bit;
		}
		double value;
		if (history && !logarithmic && (history->stable & bit) && current->low == current->high) {
			/* Retain the ordered products and subtracts, including fused
			 * rounding. Only exact integer-to-double conversions are cached. */
			double error = cache->observed;
			error -= fit->coefficient[0] * current->low;
			for (unsigned j = 1; j < model->taps; j++)
				error -= fit->coefficient[j] * history->level[i][j - 1];
			value = bin_likelihood(error, cache->variance, cache->divisor, cache->lower, cache->upper, false);
		} else {
			const struct v92_observation_cell *cells[TAPS] = { current };
			int32_t input[TAPS] = { x };
			for (unsigned j = 1; j < model->taps; j++) {
				cells[j] = previous[j - 1].values.cell + i;
				input[j] = previous[j - 1].x;
			}
			value = compiled_likelihood(model->law, fit, cells, input, model->taps,
				cache->observed, cache->variance, cache->divisor, cache->width[i], cache->lower, cache->upper,
				logarithmic);
		}
		likelihood = logarithmic ? log_add(likelihood, cache->log_weight[i] + value) :
			likelihood + fit->weight * value;
	}
	return likelihood;
}

uint64_t v92_observation_score_history(struct v92_observation_cache *cache, int32_t x,
	const struct v92_observation_point *previous, uint64_t budget,
	struct v92_observation_history *history)
{
	const struct v92_observation *model = cache->model;
	if (model->input_coefficient)
		x = round_shift_s64_to_s32_saturating((int64_t)x * model->input_coefficient, 15u);
	size_t span = point_span(cache, x);
	const struct v92_observation_values *a = point_values(cache, span);
	if (budget != UINT64_MAX) {
		/* Every mixture mean lies in this deliberately enlarged envelope.
		 * At distance d from the observed codec cell, its Gaussian mass is
		 * at most exp(-d*d/(2*variance)), hence the score is at least
		 * SCORE_SCALE*d*d/variance. Correlation between taps can only shrink
		 * the envelope. */
		double low = a->low[0], high = a->high[0];
		for (unsigned j = 1; j < model->taps; j++) {
			low += previous[j - 1].values.low[j];
			high += previous[j - 1].values.high[j];
		}
		double distance = low - (cache->observed + cache->upper);
		double opposite = cache->observed + cache->lower - high;
		if (opposite > distance) distance = opposite;
		if (distance < 0.) distance = 0.;
		double bound = cache->bound_scale * distance * distance;
		if (isfinite(bound)) bound -= 64. * DBL_EPSILON * (1. + bound);
		uint64_t minimum = isnan(bound) ? 0 : score_value(bound);

		if (minimum >= budget)
			return minimum;
	}

	int32_t key[TAPS] = { a->stable ? model->first[span] : x };
	uint64_t hash = (uint32_t)key[0];
	for (unsigned j = 1; j < model->taps; j++) {
		key[j] = previous[j - 1].key;
		hash = (hash ^ (hash >> 33) ^ (uint32_t)key[j]) * UINT64_C(0xff51afd7ed558ccd);
	}
	size_t slot = (hash ^ (hash >> 33)) & (V92_OBSERVATION_CACHE - 1);
	size_t vacant = V92_OBSERVATION_CACHE;
	for (size_t probe = 0; probe < 4; probe++) {
		size_t index = (slot + probe) & (V92_OBSERVATION_CACHE - 1);
		if (cache->stamp[index] != cache->generation) { vacant = index; break; }
		if (!memcmp(cache->key[index], key, sizeof(key))) return cache->value[index];
	}
	/* Normal masses need only one logarithm, rather than a log/exp pair
	 * per gain cell and another per mixture component. Retain the original
	 * evaluation for directed rounding, deep tails and integer boundaries.
	 * The 0.01 score-unit guard is deliberately much wider than binary64
	 * summation error over the bounded eight four-tap codec partitions.
	 * The mass floor limits log magnitudes and makes underflowed terms
	 * negligible; it selects an evaluation method, never caps a score. */
	double score = NAN;
	if (fegetround() == FE_TONEAREST) {
		double probability = observation_likelihood(cache, a, previous, x, false, history);
		if (probability >= 1e-200 && probability <= 1.)
			score = -2. * V92_SCORE_SCALE * log(probability);
	}
	uint64_t value;
	if (isfinite(score) && (score < .01 || fabs(score - nearbyint(score)) > .01)) {
		value = score_value(score);
	} else {
		double likelihood = observation_likelihood(cache, a, previous, x, true, NULL);
		value = score_value(-2. * V92_SCORE_SCALE * likelihood);
	}
	if (vacant != V92_OBSERVATION_CACHE) {
		memcpy(cache->key[vacant], key, sizeof(key));
		cache->value[vacant] = value;
		cache->stamp[vacant] = cache->generation;
	}
	return value;
}

uint64_t v92_observation_score(struct v92_observation_cache *cache, int32_t x,
	const struct v92_observation_point *previous, uint64_t budget)
{
	return v92_observation_score_history(cache, x, previous, budget, NULL);
}

struct v92_observation_range v92_observation_range(struct v92_observation_cache *cache, int32_t x)
{
	if (cache->model->input_coefficient)
		x = round_shift_s64_to_s32_saturating((int64_t)x * cache->model->input_coefficient, 15u);
	const struct v92_observation_values *values = point_values(cache, point_span(cache, x));
	return (struct v92_observation_range){ values->low[0], values->high[0] };
}

/* E[Q(mean + noise)], not Q(E[mean + noise]). A small channel-fit error
 * near a codec boundary must not jump the predicted sample by a whole
 * quantization step. Sum level jumps times Gaussian tail probabilities,
 * starting at the mean to avoid cancellation between saturated levels.
 * Work in host coordinates so a tiny codec gain cannot overflow variance.
 * erfc(28) is unrepresentable, as in gaussian_probability; this is a
 * numeric tail boundary, not a channel-noise cutoff. */
static double codec_mean(enum g711_law law, double mean, double variance, double gain)
{
	if (!isfinite(mean) || !isfinite(variance) || variance <= 0. ||
	    !isfinite(gain) || gain <= 0.) return NAN;
	double divisor = sqrt(variance) * sqrt(2.);
	int16_t center = g711_quantize(law, sample_round_s16(mean / gain));
	double prediction = center;
	bool nearest = fegetround() == FE_TONEAREST;
	for (int direction = -1; direction <= 1; direction += 2) {
		int16_t level = center;
		for (unsigned step = 0; step < 256; step++) {
			struct g711_interval bin;
			if (!g711_quantization_interval(law, level, &bin)) return NAN;
			int edge = direction < 0 ? bin.lower : bin.upper;
			if (edge == (direction < 0 ? INT16_MIN : INT16_MAX)) break;
			double boundary = (edge + direction * .5) * gain;
			double distance = direction * (boundary - mean) / divisor;
			if (distance >= 28.) break;
			int16_t next = g711_quantize(law, (int16_t)(edge + direction));
			double tail = erfc(distance);
			prediction += (next - level) * .5 * tail;
			/* Later tails decrease and have this same sign. Any int16
			 * level jump has half-magnitude below 32768. If even that
			 * bound rounds away, every remaining addition would too.
			 * Directed rounding retains the complete walk. */
			if (nearest && prediction + direction * 32768. * tail == prediction)
				break;
			level = next;
		}
	}
	return gain * prediction;
}

double v92_observation_predict(const struct v92_observation *model,
	const int32_t *input, double offset)
{
	if (!(model->quantizer_gain > 0.) || !isfinite(offset)) return NAN;
	int32_t x[TAPS];
	for (unsigned j = 0; j < model->taps; j++)
		x[j] = model->input_coefficient ? round_shift_s64_to_s32_saturating(
			(int64_t)input[j] * model->input_coefficient, 15u) : input[j];
	double prediction = 0;
	for (unsigned i = 0; i < V92_OBSERVATION_INTERVALS; i++) {
		const struct v92_observation_interval *p = model->intervals + i;
		if (p->weight == 0) continue;
		double width = log1p((p->high - p->low) / p->low);
		struct gain_sweep sweep = gain_sweep_begin(model->law, x, model->taps, p->low, p->high, NULL);
		while (gain_sweep_next(&sweep, model->law, x)) {
			double weight = p->weight * log1p((sweep.end - sweep.begin) / sweep.begin) / width;
			double linear = 0.;
			for (unsigned j = 0; j < model->taps; j++) linear += p->coefficient[j] * sweep.value[j];
			prediction += weight * codec_mean(model->law, linear - offset,
				model->variance, model->quantizer_gain);
		}
		if (sweep.failed) return NAN;
	}
	return prediction + offset;
}
