// SPDX-License-Identifier: BSD-3-Clause
#include "v92_observation.h"
#include "memory_region.h"
#include "gram_fit.h"
#include "integer_math.h"
#include "sample_math.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define V92_SCORE_SCALE 1e6

enum { INTERVALS = V92_OBSERVATION_INTERVALS, TAPS = V92_OBSERVATION_TAPS };
_Static_assert((int)TAPS == (int)GRAM_FIT_COLUMNS, "Gram moment stride must match");
_Static_assert(V92_OBSERVATION_STEPS < UINT16_MAX, "geometry tags must fit");
_Static_assert((V92_OBSERVATION_CACHE & (V92_OBSERVATION_CACHE - 1)) == 0 &&
	(V92_OBSERVATION_POINTS & (V92_OBSERVATION_POINTS - 1)) == 0,
	"cache sizes must be powers of two");
struct interval { double low, high, coefficient[TAPS], error, weight; };
struct moments { double gram[GRAM_FIT_MOMENTS], rhs[TAPS], energy; };
struct boundary { double at; unsigned next; };

static bool codec_bounds(enum g711_law law, double gain, int16_t observed,
	const int16_t *level, double offset, double *lower, double *upper);

static bool solve_channel(const struct moments *moments, unsigned taps, double fitted[TAPS])
{
	return gram_fit(moments->gram, moments->rhs, taps, fitted) && fitted[0] > 0.;
}

/* Gaussian interval probability with arguments scaled by sqrt(2)*sigma.
 * erfc(28) is below half the smallest binary64 subnormal; erf(6) rounds
 * to one. These are representable-value tails, not probability cutoffs. */
static double gaussian_probability(double a, double b)
{
	if (a > 0.) {
		if (a >= 28.) return 0.;
		return .5 * (erfc(a) - (b >= 28. ? 0. : erfc(b)));
	}
	if (b < 0.) {
		if (b <= -28.) return 0.;
		return .5 * (erfc(-b) - (a <= -28. ? 0. : erfc(-a)));
	}
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
	double low = gaussian_log_tail(a * sqrt(2.)), high = gaussian_log_tail(b * sqrt(2.));
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
	double *expected, double *deviation)
{
	double sigma = sqrt(variance), a = (low - mean) / sigma, b = (high - mean) / sigma;
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

/* Bounded expectation/maximization, starting with the center fit. The E
 * step conditions latent samples on their codec bins; the M step fits the
 * channel taps and noise together. A one-sample variance floor bounds numeric
 * precision, not channel quality. Payload never updates these parameters. */
static bool fit_bin_channel(struct v92_observation_interval *fit, const int16_t *q,
	const int16_t *y, const double *low, const double *high, size_t count, unsigned taps, double *variance)
{
	struct moments m = { 0 };
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
	noise = fmax(1., noise / (count - taps));
	for (unsigned iteration = 0; iteration < 96; iteration++) {
		double targets[V92_OBSERVATION_TRAINING], deviations[V92_OBSERVATION_TRAINING];
		double next[TAPS] = { 0 }, next_noise = 0.;
		memset(m.rhs, 0, sizeof(m.rhs));
		for (size_t j = taps; j < count; j++) {
			double mean = 0.;
			for (unsigned i = 0; i < taps; i++) mean += fit->coefficient[i] * q[j - i];
			if (!bin_moments(low[j], high[j], mean, noise, targets + j, deviations + j)) return false;
			for (unsigned i = 0; i < taps; i++) m.rhs[i] += q[j - i] * targets[j];
		}
		if (!solve_channel(&m, taps, next)) return false;
		for (size_t j = taps; j < count; j++) {
			double residual = targets[j];
			for (unsigned i = 0; i < taps; i++) residual -= next[i] * q[j - i];
			next_noise += deviations[j] + residual * residual;
		}
		next_noise = fmax(1., next_noise / (count - taps));
		if (!isfinite(next_noise)) return false;
		double change = 0.;
		for (unsigned i = 0; i < taps; i++) change += fabs(next[i] - fit->coefficient[i]);
		bool settled = fabs(next_noise - noise) < 1e-8 * (1. + noise) &&
			change < 1e-10 * (1. + next[0]);
		memcpy(fit->coefficient, next, sizeof(next)); noise = next_noise;
		if (settled) break;
	}
	*variance = noise;
	return true;
}

/* The first-codec gain intervals come from the initial center fit. Select
 * noise from the best likelihood fit, then compare all retained hypotheses
 * under that common variance and their log-uniform gain prior. */
static bool refit_bins(struct v92_observation *model,
	const int32_t *x, const int16_t *y, const int16_t *levels,
	const double *offsets, size_t count)
{
	double low[V92_OBSERVATION_TRAINING], high[V92_OBSERVATION_TRAINING];
	double costs[INTERVALS], variances[INTERVALS], best = HUGE_VAL, total = 0.;
	int16_t q[INTERVALS][V92_OBSERVATION_TRAINING];
	unsigned chosen = 0;
	for (size_t j = 0; j < count; j++) {
		double lower, upper;
		if (!codec_bounds(model->law, model->quantizer_gain, y[j],
			levels ? levels + j : NULL, offsets ? offsets[j] : 0., &lower, &upper))
			return false;
		low[j] = y[j] + lower;
		high[j] = y[j] + upper;
	}
	for (unsigned i = 0; i < INTERVALS; i++) {
		struct v92_observation_interval *fit = &model->intervals[i];
		double gain = sqrt(fit->low * fit->high);
		for (size_t j = 0; j < count; j++) {
			double value = fmax(INT16_MIN, fmin(INT16_MAX, round(gain * x[j])));
			q[i][j] = g711_quantize(model->law, (int16_t)value);
		}
		costs[i] = fit_bin_channel(fit, q[i], y, low, high, count, model->taps, variances + i) ?
			bin_cost(q[i], low, high, count, model->taps, fit, variances[i]) : HUGE_VAL;
		if (costs[i] < best) { best = costs[i]; chosen = i; }
	}
	if (!isfinite(best)) return false;
	model->variance = variances[chosen];
	best = HUGE_VAL;
	for (unsigned i = 0; i < INTERVALS; i++) {
		if (isfinite(costs[i]))
			costs[i] = bin_cost(q[i], low, high, count, model->taps, model->intervals + i, model->variance);
		best = fmin(best, costs[i]);
	}
	for (unsigned i = 0; i < INTERVALS; i++) {
		struct v92_observation_interval *fit = &model->intervals[i];
		fit->weight = exp(-.5 * (costs[i] - best)) *
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

static struct boundary next_boundary(enum g711_law law, int32_t x, unsigned magnitude)
{
	unsigned limit = x < 0 ? 32768u : 32767u;
	struct boundary boundary = { .at = HUGE_VAL, .next = limit };
	if (x == 0) return boundary;
	int16_t sample = (int16_t)(x < 0 ? -(int32_t)magnitude : (int32_t)magnitude);
	unsigned code = law == G711_LAW_PCMU ? (unsigned)(uint8_t)~g711_encode_pcmu(sample) & 127u :
		(unsigned)(g711_encode_pcma(sample) ^ 0x55u) & 127u;
	/* The largest segment also includes the encoder's clipped tail. */
	if (code == 127u) return boundary;
	unsigned segment = code >> 4, quantum = code & 15u, next;
	if (law == G711_LAW_PCMU)
		next = ((17u + quantum) << (segment + 3u)) - 132u;
	else if (x < 0 && magnitude == 0u)
		next = 1u; /* A-law changes from +8 at zero to -8 at -1. */
	else {
		next = segment == 0u ? (quantum + 1u) << 4u : (17u + quantum) << (segment + 3u);
		if (x < 0) next++; /* Complement the negative magnitude before scaling. */
	}
	boundary.next = next;
	boundary.at = (next - .5) / fabs((double)x);
	return boundary;
}

static void heap_down(size_t *heap, size_t count, const struct boundary *events)
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

static bool fit_intervals(struct interval fits[INTERVALS], enum g711_law law,
	const int32_t *x, const int16_t *y, size_t count, unsigned taps)
{
	struct boundary events[V92_OBSERVATION_TRAINING];
	size_t heap[V92_OBSERVATION_TRAINING], used = 0;
	int16_t q[V92_OBSERVATION_TRAINING] = { 0 };
	struct moments m = { 0 };
	double low = 0.;
	for (size_t i = 0; i < INTERVALS; i++) fits[i] = (struct interval){ .error = HUGE_VAL };
	for (size_t j = 1; j < count; j++) {
		unsigned limit = x[j] < 0 ? 32768u : 32767u;
		unsigned magnitude = (unsigned)fmin(limit, round(low * fabs((double)x[j])));
		q[j] = magnitude_code(law, x[j], magnitude);
		events[j] = next_boundary(law, x[j], magnitude);
		size_t child = used++;
		heap[child] = j;
		while (child) {
			size_t parent = (child - 1) / 2;
			if (events[heap[parent]].at <= events[heap[child]].at) break;
			size_t swap = heap[parent]; heap[parent] = heap[child]; heap[child] = swap;
			child = parent;
		}
	}
	for (size_t j = taps; j < count; j++) {
		for (unsigned a = 0; a < taps; a++) {
			m.rhs[a] += (double)q[j - a] * y[j];
			for (unsigned b = 0; b <= a; b++)
				m.gram[gram_moment_index(a, b)] += (double)q[j - a] * q[j - b];
		}
		m.energy += (double)y[j] * y[j];
	}
	/* Every known input crosses finitely many codec boundaries. Search all
	 * bounded cells; the all-saturated tail has no identifiable gain and
	 * cannot carry a proper log-uniform prior. */
	for (;;) {
		double high = events[heap[0]].at;
		if (!isfinite(high)) break;

		if (!(high > low)) return false;
		struct interval fit = { .low = low, .high = high };
		if (low > 0. && solve_channel(&m, taps, fit.coefficient)) {
			double error = m.energy;
			for (unsigned i = 0; i < taps; i++) error -= fit.coefficient[i] * m.rhs[i];
			fit.error = fmax(0., error / (count - taps));
			if (isfinite(fit.error)) {
				for (size_t i = 0; i < INTERVALS; i++) {
					if (fit.error >= fits[i].error) continue;
					memmove(fits + i + 1, fits + i, (INTERVALS - i - 1) * sizeof(*fits));
					fits[i] = fit;
					break;
				}
			}
		}
		while (events[heap[0]].at == high) {
			size_t j = heap[0];
			int16_t value = magnitude_code(law, x[j], events[j].next);
			double change = value - q[j], square = (double)value * value - (double)q[j] * q[j];
			for (unsigned a = 0; a < taps; a++) {
				size_t row = j + a;
				if (row < taps || row >= count) continue;
				m.rhs[a] += change * y[row];
				m.gram[gram_moment_index(a, a)] += square;
				/* Each affected row contributes once to its shared moment;
				 * both index orders still matter as q[j] moves. */
				for (unsigned b = 0; b < taps; b++) if (b != a) {
					double delta = change * q[row - b];
					m.gram[gram_moment_index(a, b)] += delta;
				}
			}
			q[j] = value;
			events[j] = next_boundary(law, x[j], events[j].next);
			heap_down(heap, used, events);
		}
		low = high;
	}
	return isfinite(fits[INTERVALS - 1].error);
}

/* Integral of Q(k*x) for log-uniform k. Every segment is an exact codec cell;
 * endpoint ties have zero measure. No sampled gain grid or quadrature count. */
static double interval_mean(enum g711_law law, const struct interval *fit, int32_t x)
{
	int16_t first = scaled(law, fit->low, x), last = scaled(law, fit->high, x);
	if (first == last) return first;
	double begin = fit->low, integral = 0;
	double width = log1p((fit->high - fit->low) / fit->low);
	unsigned limit = x < 0 ? 32768u : 32767u;
	unsigned magnitude = (unsigned)fmin(limit, round(begin * fabs((double)x)));
	for (;;) {
		struct boundary boundary = next_boundary(law, x, magnitude);
		double end = fmin(fit->high, boundary.at);
		if (end < begin) return NAN;
		integral += magnitude_code(law, x, magnitude) * log1p((end - begin) / begin);
		if (end == fit->high) break;
		magnitude = boundary.next;
		begin = end;
	}
	return integral / width;
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

bool v92_observation_fit(struct v92_observation *model,
	const struct v92_observation_training *training)
{
	if (!training_is_valid(model, sizeof(*model), training) ||
		(training->taps != 3 && training->taps != 4) || training->count <= training->taps) return false;
	const int32_t *x = training->input;
	const int16_t *u = training->linear, *y = training->sample, *levels = training->levels;
	const double *offsets = training->offsets;
	size_t count = training->count;
	unsigned taps = training->taps;
	enum g711_law law = training->law;
	double energy = 0, linear_error = 0;
	for (size_t i = taps; i < count; i++) {
		double error = (double)y[i] - u[i];
		energy += (double)y[i] * y[i]; linear_error += error * error;
	}
	if (energy == 0. || linear_error == 0.) return false;
	struct interval fits[INTERVALS];
	if (!fit_intervals(fits, law, x, y, count, taps)) return false;
	struct v92_observation initialized = { .law = law, .taps = taps, .energy = energy / (count - taps),
		.variance = fmax(1., fits[0].error), .quantizer_gain = training->host_gain };
	double total = 0;
	for (size_t i = 0; i < INTERVALS; i++) {
		fits[i].weight = exp(-.5 * (count - taps) * (fits[i].error - fits[0].error) / initialized.variance) *
			log(fits[i].high / fits[i].low);
		total += fits[i].weight;
	}
	if (!(total > 0.) || !isfinite(total)) return false;
	for (size_t i = 0; i < INTERVALS; i++) fits[i].weight /= total;
	double error_energy = 0;
	for (size_t j = taps; j < count; j++) {
		double predicted = 0;
		for (size_t i = 0; i < INTERVALS; i++)
			for (unsigned k = 0; k < taps; k++)
				predicted += fits[i].weight * fits[i].coefficient[k] * interval_mean(law, fits + i, x[j - k]);
		double error = y[j] - predicted;
		error_energy += error * error;
	}
	if (!isfinite(error_energy) || error_energy >= linear_error) return false;
	for (size_t i = 0; i < INTERVALS; i++) {
		initialized.intervals[i] = (struct v92_observation_interval){
			.low = fits[i].low, .high = fits[i].high, .weight = fits[i].weight };
		memcpy(initialized.intervals[i].coefficient, fits[i].coefficient, sizeof(fits[i].coefficient));
	}
	if (!refit_bins(&initialized, x, y, levels, offsets, count) || !compile_geometry(&initialized) ||
		!v92_observation_is_valid(&initialized)) return false;
	*model = initialized;
	return true;
}

double v92_observation_predict_scalar(enum g711_law law, double gain,
	double host_gain, int16_t linear, double offset)
{
	if ((law != G711_LAW_PCMU && law != G711_LAW_PCMA) ||
	    !isfinite(gain) || gain <= 0. || !isfinite(host_gain) || host_gain <= 0. ||
	    !isfinite(offset)) return NAN;
	double physical = (gain * linear - offset) / host_gain;
	if (!isfinite(physical)) return NAN;
	int16_t sample = (int16_t)fmax(INT16_MIN, fmin(INT16_MAX, round(physical)));
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
	double fitted = xy / xx, variance = fmax(1., linear_error / count);
	/* The same bounded conditional-bin fit as the tandem channel, with
 * one gain instead of a channel kernel. Fitting distance only outside bins
	 * would mistake quantization uncertainty for noise-free observations. */
	for (unsigned iteration = 0; iteration < 96; iteration++) {
		double targets[V92_OBSERVATION_TRAINING], deviations[V92_OBSERVATION_TRAINING];
		double next_xy = 0., next_variance = 0.;
		for (size_t i = 0; i < count; i++) {
			if (!bin_moments(low[i], high[i], fitted * training->linear[i], variance,
				targets + i, deviations + i)) return false;
			next_xy += training->linear[i] * targets[i];
		}
		fitted = next_xy / xx;
		if (!isfinite(fitted) || fitted <= 0.) return false;
		for (size_t i = 0; i < count; i++) {
			double error = targets[i] - fitted * training->linear[i];
			next_variance += deviations[i] + error * error;
		}
		variance = fmax(1., next_variance / count);
		if (!isfinite(variance)) return false;
	}
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

static size_t point_span(const struct v92_observation_cache *cache, int32_t x)
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

/* Log probability of the observed final-codec bin under one Gaussian mean. */
static double bin_log_probability(double error, double variance, double divisor, double lower, double upper)
{
	if (lower == upper) return -.5 * error * error / variance;
	return gaussian_log_probability((error + lower) / divisor, (error + upper) / divisor);
}

/* General per-observation integral: all symbols share the same log-uniform
 * codec scale. Merge their exact quantizer boundaries, not independent means. */
static double compiled_log_likelihood(enum g711_law law, const struct v92_observation_interval *fit,
	const struct v92_observation_cell *const *cells, const int32_t *x, unsigned taps,
	int16_t observed, double variance, double divisor, double width, double lower, double upper)
{
	double begin = fit->low, likelihood = -HUGE_VAL;
	bool stable = true;
	for (unsigned i = 0; i < taps; i++) stable &= cells[i]->low == cells[i]->high;
	if (stable) {
		double error = observed;
		for (unsigned i = 0; i < taps; i++) error -= fit->coefficient[i] * cells[i]->low;
		return bin_log_probability(error, variance, divisor, lower, upper);
	}
	struct boundary boundary[TAPS];
	unsigned magnitude[TAPS];
	bool general[TAPS];
	int16_t value[TAPS];
	for (unsigned i = 0; i < taps; i++) {
		const struct v92_observation_cell *cell = cells[i];
		general[i] = cell->low != cell->high && !cell->crossing;
		value[i] = cell->low;
		if (general[i]) {
			unsigned limit = x[i] < 0 ? 32768u : 32767u;
			double sample = begin * fabs((double)x[i]);
			magnitude[i] = sample >= limit ? limit : (unsigned)round(sample);
			boundary[i] = next_boundary(law, x[i], magnitude[i]);
		} else {
			boundary[i] = (struct boundary){ .at = cell->low == cell->high ?
				HUGE_VAL : (cell->crossing - .5) / fabs((double)x[i]) };
		}
	}
	for (;;) {
		double end = fit->high, error = observed;
		for (unsigned i = 0; i < taps; i++) {
			end = fmin(end, boundary[i].at);
			error -= fit->coefficient[i] * value[i];
		}
		if (end < begin) return NAN;
		if (end > begin)
			likelihood = log_add(likelihood, bin_log_probability(error, variance, divisor, lower, upper) +
				log(log1p((end - begin) / begin) / width));
		if (end == fit->high) return likelihood;
		for (unsigned i = 0; i < taps; i++) {
			if (boundary[i].at != end) continue;
			if (general[i]) {
				magnitude[i] = boundary[i].next;
				value[i] = magnitude_code(law, x[i], magnitude[i]);
				boundary[i] = next_boundary(law, x[i], magnitude[i]);
			} else {
				value[i] = cells[i]->high;
				boundary[i].at = HUGE_VAL;
			}
		}
		begin = end;
	}
}

void v92_observation_begin(struct v92_observation_cache *cache,
	const struct v92_observation *model, double relative_noise)
{
	memset(cache->stamp, 0, sizeof(cache->stamp));
	memset(cache->point_tag, 0, sizeof(cache->point_tag));
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

static const struct v92_observation_values *point_values(struct v92_observation_cache *cache, size_t span)
{
	size_t slot = span & (V92_OBSERVATION_POINTS - 1), chosen = slot;
	for (size_t probe = 0; probe < 4; probe++) {
		size_t index = (slot + probe) & (V92_OBSERVATION_POINTS - 1);
		if (cache->point_tag[index] == span + 1) return cache->points + index;
		if (!cache->point_tag[index]) { chosen = index; break; }
	}
	const struct v92_observation *model = cache->model;
	int32_t x = model->first[span];
	struct v92_observation_values *values = cache->points + chosen;
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
			struct boundary boundary = next_boundary(model->law, x, magnitude);
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
	cache->point_tag[chosen] = (uint16_t)(span + 1);
	return values;
}

struct v92_observation_point v92_observation_point(struct v92_observation_cache *cache, int32_t x)
{
	if (cache->model->input_coefficient)
		x = round_shift_s64_to_s32_saturating((int64_t)x * cache->model->input_coefficient, 15u);
	size_t span = point_span(cache, x);
	struct v92_observation_values values = *point_values(cache, span);
	return (struct v92_observation_point){ values, x, values.stable ? cache->model->first[span] : x, span };
}

uint64_t v92_observation_score(struct v92_observation_cache *cache, int32_t x,
	const struct v92_observation_point *previous, uint64_t budget)
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
	double likelihood = -HUGE_VAL;
	for (size_t i = 0; i < INTERVALS; i++) if (model->intervals[i].weight > 0.) {
		const struct v92_observation_cell *cells[TAPS] = { a->cell + i };
		int32_t input[TAPS] = { x };
		for (unsigned j = 1; j < model->taps; j++) {
			cells[j] = previous[j - 1].values.cell + i;
			input[j] = previous[j - 1].x;
		}
		likelihood = log_add(likelihood, cache->log_weight[i] +
			compiled_log_likelihood(model->law, model->intervals + i, cells, input, model->taps,
			cache->observed, cache->variance, cache->divisor, cache->width[i], cache->lower, cache->upper));
	}
	uint64_t value = score_value(-2. * V92_SCORE_SCALE * likelihood);
	if (vacant != V92_OBSERVATION_CACHE) {
		memcpy(cache->key[vacant], key, sizeof(key));
		cache->value[vacant] = value;
		cache->stamp[vacant] = cache->generation;
	}
	return value;
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
	int16_t center = g711_quantize(law, (int16_t)fmax(INT16_MIN,
		fmin(INT16_MAX, round(mean / gain))));
	double prediction = center;
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
			prediction += (next - level) * .5 * erfc(distance);
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
		double begin = p->low, width = log1p((p->high - p->low) / p->low);
		struct boundary bound[TAPS];
		int16_t q[TAPS];
		for (unsigned j = 0; j < model->taps; j++) {
			unsigned limit = x[j] < 0 ? 32768u : 32767u;
			double sample = begin * fabs((double)x[j]);
			unsigned magnitude = sample >= limit ? limit : (unsigned)round(sample);
			q[j] = magnitude_code(model->law, x[j], magnitude);
			bound[j] = next_boundary(model->law, x[j], magnitude);
		}
		for (unsigned steps = 0;; steps++) {
			double end = p->high;
			for (unsigned j = 0; j < model->taps; j++) end = fmin(end, bound[j].at);
			if (end < begin || steps > model->taps * 128u) return NAN;
			double weight = p->weight * log1p((end - begin) / begin) / width;
			double linear = 0.;
			for (unsigned j = 0; j < model->taps; j++) linear += p->coefficient[j] * q[j];
			prediction += weight * codec_mean(model->law, linear - offset,
				model->variance, model->quantizer_gain);
			if (end == p->high) break;
			for (unsigned j = 0; j < model->taps; j++) {
				if (bound[j].at != end) continue;
				q[j] = magnitude_code(model->law, x[j], bound[j].next);
				bound[j] = next_boundary(model->law, x[j], bound[j].next);
			}
			begin = end;
		}
	}
	return prediction + offset;
}
