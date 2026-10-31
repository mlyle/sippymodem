// SPDX-License-Identifier: BSD-3-Clause
#include "interval_fit.h"

#include <assert.h>
#include <fenv.h>
#include <stdio.h>
#include <string.h>

static void test_half_normal(void)
{
	const int16_t q[] = { 1 };
	const double low[] = { 0. }, high[] = { INFINITY };
	const double coefficient[4] = { 0 }, gram[10] = { 1. };
	double next[4], variance;
	assert(interval_fit_step(coefficient, q, low, high, 1, 0, 1,
		gram, 100., next, &variance));
	long double ratio = 2.L / acosl(-1.L);
	assert(fabsl(next[0] - 10.L * sqrtl(ratio)) < 1e-12L);
	assert(fabsl(variance - 100.L * (1.L - ratio)) < 1e-11L);
	assert(next[1] == 0. && next[2] == 0. && next[3] == 0.);
}

struct fit_case {
	int16_t q[INTERVAL_FIT_SAMPLES];
	double low[INTERVAL_FIT_SAMPLES], high[INTERVAL_FIT_SAMPLES];
	double gram[GRAM_FIT_MOMENTS];
	unsigned taps;
};

static void prepare(struct fit_case *c, unsigned taps, unsigned seed)
{
	memset(c, 0, sizeof(*c));
	c->taps = taps;
	for (unsigned j = 0; j < INTERVAL_FIT_SAMPLES; j++) {
		seed = seed * 1664525u + 1013904223u;
		c->q[j] = (int)(seed % 13u) - 6;
	}
	for (unsigned j = taps; j < INTERVAL_FIT_SAMPLES; j++) {
		double row[4], rhs[4] = { 0 }, mean = 0.;
		for (unsigned i = 0; i < taps; i++) {
			row[i] = c->q[j - i];
			mean += row[i] * (i ? .125 / i : 1.);
		}
		gram_accumulate(c->gram, rhs, row, taps, 0.);
		c->low[j] = mean - 1.5;
		c->high[j] = mean + 1.5;
	}
}

static void advance(const struct fit_case *c, double coefficient[4], double *variance)
{
	assert(interval_fit_step(coefficient, c->q, c->low, c->high,
		INTERVAL_FIT_SAMPLES, c->taps, c->taps, c->gram, *variance,
		coefficient, variance));
}

static void test_independent_owners(void)
{
	for (unsigned taps = 1; taps <= 4; taps++) {
		struct fit_case c[2];
		prepare(c, taps, 0x1234u);
		prepare(c + 1, 5u - taps, 0xfedcu);
		double serial[2][4] = { { .9 }, { 1.1 } }, variance[2] = { 4., 8. };
		for (unsigned i = 0; i < 2; i++)
			for (unsigned j = 0; j < 96; j++) advance(c + i, serial[i], variance + i);
		double mixed[2][4] = { { .9 }, { 1.1 } }, noise[2] = { 4., 8. };
		unsigned iterations[2] = { 0 };
		/* Suspend either owner arbitrarily. No private loop state or borrowed
		 * pointer may make the result depend on interleaving or output alias. */
		for (unsigned turn = 0; iterations[0] < 96 || iterations[1] < 96; turn++) {
			unsigned i = turn % 3u == 0;
			if (iterations[i] == 96) continue;
			double output[6] = { -91., 0, 0, 0, 0, 92. }, next_noise;
			assert(interval_fit_step(mixed[i], c[i].q, c[i].low, c[i].high,
				INTERVAL_FIT_SAMPLES, c[i].taps, c[i].taps, c[i].gram,
				noise[i], output + 1, &next_noise));
			assert(output[0] == -91. && output[5] == 92.);
			memcpy(mixed[i], output + 1, sizeof(mixed[i]));
			noise[i] = next_noise;
			iterations[i]++;
		}
		assert(!memcmp(mixed, serial, sizeof(mixed)));
		assert(!memcmp(noise, variance, sizeof(noise)));
	}
}

static void test_failure_atomicity(void)
{
	struct fit_case c;
	prepare(&c, 3, 31u);
	double coefficient[4] = { 1., 0, 0, 0 }, output[4] = { 11., 12., 13., 14. };
	double before[4], variance = 97.;
	memcpy(before, output, sizeof(before));
#define REJECT(count, first, taps, noise) do { \
	assert(!interval_fit_step(coefficient, c.q, c.low, c.high, \
		count, first, taps, c.gram, noise, output, &variance)); \
	assert(!memcmp(before, output, sizeof(before)) && variance == 97.); \
} while (0)
	REJECT(INTERVAL_FIT_SAMPLES + 1u, 3, 3, 4.);
	REJECT(INTERVAL_FIT_SAMPLES, 0, 3, 4.);
	REJECT(INTERVAL_FIT_SAMPLES, INTERVAL_FIT_SAMPLES, 3, 4.);
	REJECT(INTERVAL_FIT_SAMPLES, 3, 0, 4.);
	REJECT(INTERVAL_FIT_SAMPLES, 4, 5, 4.);
	REJECT(INTERVAL_FIT_SAMPLES, 3, 3, NAN);
	REJECT(INTERVAL_FIT_SAMPLES, 3, 3, 0.);
	/* A failure at the last observation must not publish a partial update. */
	c.high[INTERVAL_FIT_SAMPLES - 1] = NAN;
	REJECT(INTERVAL_FIT_SAMPLES, 3, 3, 4.);
	prepare(&c, 3, 31u);
	memset(c.gram, 0, sizeof(c.gram));
	REJECT(INTERVAL_FIT_SAMPLES, 3, 3, 4.);
#undef REJECT
}

int main(void)
{
	int saved = fegetround();
	const int modes[] = { FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO };
	for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
		assert(fesetround(modes[i]) == 0);
		test_half_normal();
		test_independent_owners();
		test_failure_atomicity();
	}
	assert(fesetround(saved) == 0);
	puts("Gaussian interval fit step checks passed");
	return 0;
}
