// SPDX-License-Identifier: BSD-3-Clause
#include "v92_observation.h"
#include "memory_region.h"
#include "integer_math.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define V92_SCORE_SCALE 1e6

enum { INTERVALS = V92_OBSERVATION_INTERVALS };
_Static_assert(V92_OBSERVATION_STEPS < UINT16_MAX, "geometry tags must fit");
_Static_assert((V92_OBSERVATION_CACHE & (V92_OBSERVATION_CACHE - 1)) == 0 &&
	(V92_OBSERVATION_POINTS & (V92_OBSERVATION_POINTS - 1)) == 0,
	"cache sizes must be powers of two");
struct interval { double low, high, main, post, second, error, weight; };
struct moments { double aa, ab, ac, bb, bc, cc, ay, by, cy, yy; };
struct boundary { double at; unsigned next; };

static bool codec_bounds(enum g711_law law, double gain, int16_t observed,
	const int16_t *level, double *lower, double *upper);

static bool solve3(double aa, double ab, double ac, double bb, double bc, double cc,
	double ay, double by, double cy, double *a, double *b, double *c)
{
	double p = bb * cc - bc * bc, q = ac * bc - ab * cc, r = ab * bc - ac * bb;
	double determinant = aa * p + ab * q + ac * r;
	if (!(aa > 0. && bb > 0. && cc > 0.) ||
		determinant <= 64 * DBL_EPSILON * aa * bb * cc) return false;
	*a = (p * ay + q * by + r * cy) / determinant;
	*b = (q * ay + (aa * cc - ac * ac) * by + (ab * ac - aa * bc) * cy) / determinant;
	*c = (r * ay + (ab * ac - aa * bc) * by + (aa * bb - ab * ab) * cy) / determinant;
	return isfinite(*a) && isfinite(*b) && isfinite(*c);
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

static double bin_cost(const int16_t *q, const double *low, const double *high,
	size_t count, const struct v92_observation_interval *fit, double variance)
{
	double cost = 0., divisor = sqrt(2. * variance);
	for (size_t j = 3; j < count; j++) {
		double mean = fit->main * q[j] + fit->post * q[j - 1] + fit->second * q[j - 2];
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
 * three taps and noise together. A one-sample variance floor bounds numeric
 * precision, not channel quality. Payload never updates these parameters. */
static bool fit_bin_channel(struct v92_observation_interval *fit, const int16_t *q,
	const int16_t *y, const double *low, const double *high, size_t count, double *variance)
{
	struct moments m = { 0 };
	double a = fit->main, b = fit->post, c = fit->second, noise = 0.;
	for (size_t j = 3; j < count; j++) {
		double qa = q[j], qb = q[j - 1], qc = q[j - 2];
		m.aa += qa * qa; m.ab += qa * qb; m.ac += qa * qc;
		m.bb += qb * qb; m.bc += qb * qc; m.cc += qc * qc;
		double residual = y[j] - a * qa - b * qb - c * qc;
		noise += residual * residual;
	}
	noise = fmax(1., noise / (count - 3));
	for (unsigned iteration = 0; iteration < 96; iteration++) {
		double targets[V92_OBSERVATION_TRAINING], deviations[V92_OBSERVATION_TRAINING];
		double ay = 0., by = 0., cy = 0.;
		for (size_t j = 3; j < count; j++) {
			if (!bin_moments(low[j], high[j], a * q[j] + b * q[j - 1] + c * q[j - 2],
				noise, targets + j, deviations + j)) return false;
			ay += q[j] * targets[j]; by += q[j - 1] * targets[j]; cy += q[j - 2] * targets[j];
		}
		double next_a, next_b, next_c, next_noise = 0.;
		if (!solve3(m.aa, m.ab, m.ac, m.bb, m.bc, m.cc, ay, by, cy,
			&next_a, &next_b, &next_c) || next_a <= 0.) return false;
		for (size_t j = 3; j < count; j++) {
			double residual = targets[j] - next_a * q[j] - next_b * q[j - 1] - next_c * q[j - 2];
			next_noise += deviations[j] + residual * residual;
		}
		next_noise = fmax(1., next_noise / (count - 3));
		if (!isfinite(next_noise)) return false;
		bool settled = fabs(next_noise - noise) < 1e-8 * (1. + noise) &&
			fabs(next_a - a) + fabs(next_b - b) + fabs(next_c - c) < 1e-10 * (1. + a);
		a = next_a; b = next_b; c = next_c; noise = next_noise;
		if (settled) break;
	}
	fit->main = a; fit->post = b; fit->second = c;
	*variance = noise;
	return true;
}

/* The first-codec gain intervals come from the initial center fit. Select
 * noise from the best likelihood fit, then compare all retained hypotheses
 * under that common variance and their log-uniform gain prior. */
static bool refit_bins(struct v92_observation *model,
	const int32_t *x, const int16_t *y, const int16_t *levels, size_t count)
{
	double low[V92_OBSERVATION_TRAINING], high[V92_OBSERVATION_TRAINING];
	double costs[INTERVALS], variances[INTERVALS], best = HUGE_VAL, total = 0.;
	int16_t q[INTERVALS][V92_OBSERVATION_TRAINING];
	unsigned chosen = 0;
	for (size_t j = 0; j < count; j++) {
		double lower, upper;
		if (!codec_bounds(model->law, model->quantizer_gain, y[j],
			levels ? levels + j : NULL, &lower, &upper))
			return false;
		low[j] = y[j] + lower;
		high[j] = y[j] + upper;
	}
	for (unsigned i = 0; i < INTERVALS; i++) {
		struct v92_observation_interval *fit = &model->intervals[i];
		double gain = sqrt(fit->low * fit->high);
		for (size_t j = 0; j < count; j++) {
			double value = fmax(INT16_MIN, fmin(INT16_MAX, round(gain * x[j])));
			q[i][j] = model->law == G711_LAW_PCMU ? g711_decode_pcmu(g711_encode_pcmu((int16_t)value)) :
				g711_decode_pcma(g711_encode_pcma((int16_t)value));
		}
		costs[i] = fit_bin_channel(fit, q[i], y, low, high, count, variances + i) ?
			bin_cost(q[i], low, high, count, fit, variances[i]) : HUGE_VAL;
		if (costs[i] < best) { best = costs[i]; chosen = i; }
	}
	if (!isfinite(best)) return false;
	model->variance = variances[chosen];
	best = HUGE_VAL;
	for (unsigned i = 0; i < INTERVALS; i++) {
		if (isfinite(costs[i]))
			costs[i] = bin_cost(q[i], low, high, count, model->intervals + i, model->variance);
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

static int16_t quantize(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMU ? g711_decode_pcmu(g711_encode_pcmu(sample)) :
		g711_decode_pcma(g711_encode_pcma(sample));
}

static int16_t scaled(enum g711_law law, double gain, int32_t x)
{
	/* An inferred reciprocal host gain can overflow for a subnormal gain.
	 * Zero still maps to codec zero, not infinity times zero. */
	if (x == 0) return quantize(law, 0);
	double sample = gain * x;
	if (sample >= INT16_MAX) return quantize(law, INT16_MAX);
	if (sample <= INT16_MIN) return quantize(law, INT16_MIN);
	sample = round(sample);
	return quantize(law, (int16_t)sample);
}

/* Bounds relative to the host sample. Use the same arithmetic for training
 * and scoring, including subnormal gain and one-sided codec clipping. */
static bool codec_bounds(enum g711_law law, double gain, int16_t observed,
	const int16_t *level, double *lower, double *upper)
{
	int16_t reconstructed = level ? *level : scaled(law, 1. / gain, observed);
	struct g711_interval interval;
	if (quantize(law, reconstructed) != reconstructed ||
		!g711_quantization_interval(law, reconstructed, &interval)) return false;
	*lower = (interval.lower == INT16_MIN ? -HUGE_VAL : (interval.lower - .5) * gain) - observed;
	*upper = (interval.upper == INT16_MAX ? HUGE_VAL : (interval.upper + .5) * gain) - observed;
	return true;
}

static int16_t magnitude_code(enum g711_law law, int32_t x, unsigned magnitude)
{
	return quantize(law, (int16_t)(x < 0 ? -(int32_t)magnitude : (int32_t)magnitude));
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
	const int32_t *x, const int16_t *y, size_t count)
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
	for (size_t j = 3; j < count; j++) {
		double a = q[j], b = q[j - 1], c = q[j - 2];
		m.aa += a * a; m.ab += a * b; m.bb += b * b;
		m.ac += a * c; m.bc += b * c; m.cc += c * c;
		m.ay += a * y[j]; m.by += b * y[j]; m.cy += c * y[j]; m.yy += (double)y[j] * y[j];
	}
	/* Every known input crosses finitely many codec boundaries. Search all
	 * bounded cells; the all-saturated tail has no identifiable gain and
	 * cannot carry a proper log-uniform prior. */
	for (;;) {
		double high = events[heap[0]].at;
		if (!isfinite(high)) break;

		if (!(high > low)) return false;
		double a, b, c;
		if (low > 0. && solve3(m.aa, m.ab, m.ac, m.bb, m.bc, m.cc, m.ay, m.by, m.cy, &a, &b, &c)) {
			double error = fmax(0., (m.yy - a * m.ay - b * m.by - c * m.cy) / (count - 3));
			if (isfinite(a) && isfinite(b) && isfinite(error) && a > 0.) {
				struct interval fit = { .low = low, .high = high,
					.main = a, .post = b, .second = c, .error = error };
				for (size_t i = 0; i < INTERVALS; i++) {
					if (error >= fits[i].error) continue;
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
			if (j >= 3) {
				m.aa += square; m.ab += change * q[j - 1];
				m.ac += change * q[j - 2]; m.ay += change * y[j];
			}
			if (j >= 2 && j + 1 < count) {
				m.bb += square; m.ab += change * q[j + 1];
				m.bc += change * q[j - 1]; m.by += change * y[j + 1];
			}
			if (j + 2 < count) {
				m.cc += square; m.ac += change * q[j + 2];
				m.bc += change * q[j + 1]; m.cy += change * y[j + 2];
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
	return cells ? (unsigned)(sizeof(unsigned) * CHAR_BIT) - __builtin_clz(cells) : 0u;
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
	if (!model || model->count < 4 || model->count > V92_OBSERVATION_STEPS ||
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
			fit->high <= fit->low || !isfinite(fit->main) || fit->main <= 0. ||
			!isfinite(fit->post) || !isfinite(fit->second) || !isfinite(fit->weight) || fit->weight < 0. || fit->weight > 1.) return false;
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

bool v92_observation_fit(struct v92_observation *model,
	const struct v92_observation_training *training)
{
	if (!model || !training ||
		memory_regions_overlap(model, sizeof(*model), training, sizeof(*training))) return false;
	const int32_t *x = training->input;
	const int16_t *u = training->linear, *y = training->sample, *levels = training->levels;
	size_t count = training->count;
	enum g711_law law = training->law;
	if (!x || !u || !y || count < 4 || count > V92_OBSERVATION_TRAINING ||
		(law != G711_LAW_PCMU && law != G711_LAW_PCMA) ||
		!isfinite(training->host_gain) || training->host_gain <= 0. ||
		memory_regions_overlap(model, sizeof(*model), x, count * sizeof(*x)) ||
		memory_regions_overlap(model, sizeof(*model), u, count * sizeof(*u)) ||
		memory_regions_overlap(model, sizeof(*model), y, count * sizeof(*y)) ||
		(levels && memory_regions_overlap(model, sizeof(*model), levels, count * sizeof(*levels)))) return false;
	double energy = 0, linear_error = 0;
	for (size_t i = 3; i < count; i++) {
		double error = (double)y[i] - u[i];
		energy += (double)y[i] * y[i]; linear_error += error * error;
	}
	if (energy == 0. || linear_error == 0.) return false;
	struct interval fits[INTERVALS];
	if (!fit_intervals(fits, law, x, y, count)) return false;
	struct v92_observation initialized = { .law = law, .energy = energy / (count - 3),
		.variance = fmax(1., fits[0].error), .quantizer_gain = training->host_gain };
	double total = 0;
	for (size_t i = 0; i < INTERVALS; i++) {
		fits[i].weight = exp(-.5 * (count - 3) * (fits[i].error - fits[0].error) / initialized.variance) *
			log(fits[i].high / fits[i].low);
		total += fits[i].weight;
	}
	if (!(total > 0.) || !isfinite(total)) return false;
	for (size_t i = 0; i < INTERVALS; i++) fits[i].weight /= total;
	double error_energy = 0;
	for (size_t j = 3; j < count; j++) {
		double predicted = 0;
		for (size_t i = 0; i < INTERVALS; i++) predicted += fits[i].weight * (
			fits[i].main * interval_mean(law, fits + i, x[j]) +
			fits[i].post * interval_mean(law, fits + i, x[j - 1]) +
			fits[i].second * interval_mean(law, fits + i, x[j - 2]));
		double error = y[j] - predicted;
		error_energy += error * error;
	}
	if (!isfinite(error_energy) || error_energy >= linear_error) return false;
	for (size_t i = 0; i < INTERVALS; i++) initialized.intervals[i] = (struct v92_observation_interval){
		fits[i].low, fits[i].high, fits[i].main, fits[i].post, fits[i].second, fits[i].weight };
	if (!refit_bins(&initialized, x, y, levels, count) || !compile_geometry(&initialized) ||
		!v92_observation_is_valid(&initialized)) return false;
	*model = initialized;
	return true;
}

static size_t point_span(const struct v92_observation *model, int32_t x)
{
	unsigned shift = bucket_shift(model);
	int64_t extent = (int64_t)(V92_OBSERVATION_BUCKETS / 2u) << shift;
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

/* Probability of the observed final-codec bin under one Gaussian mean. */
static double bin_probability(double error, double variance, double divisor, double lower, double upper)
{
	if (lower == upper) return exp(-.5 * error * error / variance);
	return gaussian_probability((error + lower) / divisor, (error + upper) / divisor);
}

/* General per-observation integral: all three symbols share the same log-uniform
 * codec scale. Merge their exact quantizer boundaries, not independent means. */
static double compiled_likelihood(enum g711_law law, const struct v92_observation_interval *fit,
	const struct v92_observation_cell *cells[3], const int32_t x[3],
	int16_t observed, double variance, double divisor, double width, double lower, double upper)
{
	double begin = fit->low, likelihood = 0.;
	if (cells[0]->low == cells[0]->high && cells[1]->low == cells[1]->high &&
		cells[2]->low == cells[2]->high) {
		double error = observed;
		error -= fit->main * cells[0]->low;
		error -= fit->post * cells[1]->low;
		error -= fit->second * cells[2]->low;
		/* Preserve the general integral's arithmetic, including rounding
		 * through multiplication and division by the equal interval width. */
		return bin_probability(error, variance, divisor, lower, upper) * width / width;
	}
	double tap[3] = { fit->main, fit->post, fit->second };
	struct boundary boundary[3];
	unsigned magnitude[3];
	bool general[3];
	int16_t value[3];
	for (unsigned i = 0; i < 3; i++) {
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
		for (unsigned i = 0; i < 3; i++) {
			end = fmin(end, boundary[i].at);
			error -= tap[i] * value[i];
		}
		if (end < begin) return NAN;
		likelihood += bin_probability(error, variance, divisor, lower, upper) *
			log1p((end - begin) / begin) / width;
		if (end == fit->high) return likelihood;
		for (unsigned i = 0; i < 3; i++) {
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
	cache->relative_noise = relative_noise;
	cache->observed = 0;
	cache->variance = cache->bound_scale = 0.;
	cache->divisor = 0.;
	double extent[3] = { 0., 0., 0. };
	for (size_t i = 0; i < INTERVALS; i++) {
		const struct v92_observation_interval *fit = model->intervals + i;
		cache->width[i] = log1p((fit->high - fit->low) / fit->low);
		if (fit->weight > 0.) {
			extent[0] = fmax(extent[0], fabs(fit->main));
			extent[1] = fmax(extent[1], fabs(fit->post));
			extent[2] = fmax(extent[2], fabs(fit->second));
		}
	}
	for (unsigned i = 0; i < 3; i++) cache->margin[i] = 1e-12 * (1. + 65536. * extent[i]);
}

bool v92_observation_symbol_level(struct v92_observation_cache *cache,
	int16_t observed, const int16_t *level)
{
	const struct v92_observation *model = cache->model;
	if (!isfinite(cache->relative_noise) || cache->relative_noise < 0.) return false;
	double variance = model->variance * (1. + cache->relative_noise * (double)observed * observed) /
		(1. + cache->relative_noise * model->energy);
	double scale = V92_SCORE_SCALE / variance;
	if (!isfinite(variance) || variance <= 0. || !isfinite(scale) || scale <= 0.) return false;
	double lower, upper;
	if (!codec_bounds(model->law, model->quantizer_gain, observed, level, &lower, &upper)) return false;
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
	return v92_observation_symbol_level(cache, observed, NULL);
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
	*values = (struct v92_observation_values){ .main_low = HUGE_VAL, .post_low = HUGE_VAL,
		.second_low = HUGE_VAL, .main_high = -HUGE_VAL, .post_high = -HUGE_VAL,
		.second_high = -HUGE_VAL, .stable = true };
	for (size_t i = 0; i < INTERVALS; i++) {
		const struct v92_observation_interval *fit = model->intervals + i;
		if (fit->weight == 0.) continue;
		struct v92_observation_cell *cell = values->cell + i;
		cell->low = scaled(model->law, fit->low, x); cell->high = scaled(model->law, fit->high, x);
		extend_envelope(&values->main_low, &values->main_high, fit->main, cell->low, cell->high);
		extend_envelope(&values->post_low, &values->post_high, fit->post, cell->low, cell->high);
		extend_envelope(&values->second_low, &values->second_high, fit->second, cell->low, cell->high);
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
	double mt = cache->margin[0], pt = cache->margin[1], st = cache->margin[2];
	values->main_low -= mt; values->main_high += mt; values->post_low -= pt; values->post_high += pt;
	values->second_low -= st; values->second_high += st;
	cache->point_tag[chosen] = (uint16_t)(span + 1);
	return values;
}

struct v92_observation_point v92_observation_point(struct v92_observation_cache *cache, int32_t x)
{
	if (cache->model->input_coefficient)
		x = round_shift_s64_to_s32_saturating((int64_t)x * cache->model->input_coefficient, 15u);
	size_t span = point_span(cache->model, x);
	struct v92_observation_values values = *point_values(cache, span);
	return (struct v92_observation_point){ values, x, values.stable ? cache->model->first[span] : x, span };
}

uint64_t v92_observation_score(struct v92_observation_cache *cache, int32_t x,
	const struct v92_observation_point *previous,
	const struct v92_observation_point *second, uint64_t budget)
{
	const struct v92_observation *model = cache->model;
	if (model->input_coefficient)
		x = round_shift_s64_to_s32_saturating((int64_t)x * model->input_coefficient, 15u);
	size_t span = point_span(model, x);
	const struct v92_observation_values *a = point_values(cache, span), *b = &previous->values;
	if (budget != UINT64_MAX) {
		/* Every mixture mean lies in this deliberately enlarged envelope.
		 * At distance d from the observed codec cell, its Gaussian mass is
		 * at most exp(-d*d/(2*variance)), hence the score is at least
		 * SCORE_SCALE*d*d/variance. Correlation between taps can only shrink
		 * the envelope. Respect the exact scorer's probability floor too. */
		double low = a->main_low + b->post_low + second->values.second_low;
		double high = a->main_high + b->post_high + second->values.second_high;
		double distance = low - (cache->observed + cache->upper);
		double opposite = cache->observed + cache->lower - high;
		if (opposite > distance) distance = opposite;
		if (distance < 0.) distance = 0.;
		double bound = cache->bound_scale * distance * distance;
		const double ceiling = -2. * V92_SCORE_SCALE * log(1e-300);
		if (bound > ceiling) bound = ceiling;
		bound -= 64. * DBL_EPSILON * (1. + bound);
		uint64_t minimum = bound > 0. ? (uint64_t)bound : 0;

		if (minimum >= budget)
			return minimum;
	}

	int32_t xkey = a->stable ? model->first[span] : x;
	uint64_t key = ((uint64_t)(uint32_t)xkey << 32) | (uint32_t)previous->key;
	uint64_t hash = (key ^ (key >> 33) ^ (uint32_t)second->key) * UINT64_C(0xff51afd7ed558ccd);
	size_t slot = (hash ^ (hash >> 33)) & (V92_OBSERVATION_CACHE - 1);
	size_t vacant = V92_OBSERVATION_CACHE;
	for (size_t probe = 0; probe < 4; probe++) {
		size_t index = (slot + probe) & (V92_OBSERVATION_CACHE - 1);
		if (cache->stamp[index] != cache->generation) { vacant = index; break; }
		if (cache->key[index] == key && cache->second_key[index] == second->key) return cache->value[index];
	}
	double likelihood = 0.;
	for (size_t i = 0; i < INTERVALS; i++) if (model->intervals[i].weight > 0.) {
		likelihood += model->intervals[i].weight * compiled_likelihood(model->law, model->intervals + i,
			(const struct v92_observation_cell *[]){ a->cell + i, b->cell + i, second->values.cell + i },
			(int32_t[]){ x, previous->x, second->x }, cache->observed, cache->variance,
			cache->divisor, cache->width[i], cache->lower, cache->upper);
	}
	uint64_t value = (uint64_t)(-2 * V92_SCORE_SCALE * log(fmin(1., fmax(1e-300, likelihood))));
	if (vacant != V92_OBSERVATION_CACHE) {
		cache->key[vacant] = key;
		cache->second_key[vacant] = second->key;
		cache->value[vacant] = value;
		cache->stamp[vacant] = cache->generation;
	}
	return value;
}

struct v92_observation_range v92_observation_range(struct v92_observation_cache *cache, int32_t x)
{
	if (cache->model->input_coefficient)
		x = round_shift_s64_to_s32_saturating((int64_t)x * cache->model->input_coefficient, 15u);
	const struct v92_observation_values *values = point_values(cache, point_span(cache->model, x));
	return (struct v92_observation_range){ values->main_low, values->main_high };
}

double v92_observation_predict(const struct v92_observation *model,
	const int32_t input[3])
{
	if (!(model->quantizer_gain > 0.)) return NAN;
	int32_t x[3];
	for (unsigned j = 0; j < 3; j++)
		x[j] = model->input_coefficient ? round_shift_s64_to_s32_saturating(
			(int64_t)input[j] * model->input_coefficient, 15u) : input[j];
	double prediction = 0;
	for (unsigned i = 0; i < V92_OBSERVATION_INTERVALS; i++) {
		const struct v92_observation_interval *p = model->intervals + i;
		if (p->weight == 0) continue;
		double begin = p->low, width = log1p((p->high - p->low) / p->low);
		struct boundary bound[3];
		int16_t q[3];
		for (unsigned j = 0; j < 3; j++) {
			unsigned limit = x[j] < 0 ? 32768u : 32767u;
			double sample = begin * fabs((double)x[j]);
			unsigned magnitude = sample >= limit ? limit : (unsigned)round(sample);
			q[j] = magnitude_code(model->law, x[j], magnitude);
			bound[j] = next_boundary(model->law, x[j], magnitude);
		}
		for (unsigned steps = 0;; steps++) {
			double end = p->high;
			for (unsigned j = 0; j < 3; j++) end = fmin(end, bound[j].at);
			if (end < begin || steps > 3u * 128u) return NAN;
			double weight = p->weight * log1p((end - begin) / begin) / width;
			double linear = p->main * q[0] + p->post * q[1] + p->second * q[2];
			double sample = fmax(INT16_MIN,
				fmin(INT16_MAX, round(linear / model->quantizer_gain)));
			prediction += weight * model->quantizer_gain *
				quantize(model->law, (int16_t)sample);
			if (end == p->high) break;
			for (unsigned j = 0; j < 3; j++) {
				if (bound[j].at != end) continue;
				q[j] = magnitude_code(model->law, x[j], bound[j].next);
				bound[j] = next_boundary(model->law, x[j], bound[j].next);
			}
			begin = end;
		}
	}
	return prediction;
}
