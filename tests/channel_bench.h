// SPDX-License-Identifier: BSD-3-Clause

#ifndef CHANNEL_BENCH_H
#define CHANNEL_BENCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	CHANNEL_BENCH_MAX_EFFECTS = 16,
	/* Eight seconds at the repository's 8 kHz media clock. */
	CHANNEL_BENCH_MAX_HISTORY_SAMPLES = 8 * 8000,
};

enum channel_bench_result {
	CHANNEL_BENCH_OK,
	CHANNEL_BENCH_INVALID_ARGUMENT,
	CHANNEL_BENCH_NO_MEMORY,
	CHANNEL_BENCH_RECORDING_MISSING,
	CHANNEL_BENCH_RECORDING_TOO_LARGE,
	CHANNEL_BENCH_RECORDING_MALFORMED,
	CHANNEL_BENCH_IO_ERROR,
	CHANNEL_BENCH_NO_COMPARABLE_WINDOW,
};

enum channel_bench_effect_kind {
	CHANNEL_BENCH_GAIN,
	CHANNEL_BENCH_NOISE,
	CHANNEL_BENCH_DELAY,
	CHANNEL_BENCH_FRACTIONAL_DELAY,
	CHANNEL_BENCH_ECHO,
	CHANNEL_BENCH_PCMU,
	CHANNEL_BENCH_PCMA,
	CHANNEL_BENCH_MUTE,
};

struct channel_bench_effect_spec {
	enum channel_bench_effect_kind kind;
	/* end_sample zero means the effect remains active after start_sample. */
	uint64_t start_sample;
	uint64_t end_sample;
	union {
		struct {
			/* 32768 is unity. Negative values invert the signal. */
			int32_t scale_q15;
		} gain;
		struct {
			/* Uniform noise spans [-peak, peak] in linear PCM units. */
			uint16_t peak;
			uint32_t seed;
		} noise;
		struct {
			size_t samples;
		} delay;
		struct {
			/* Signed Q16 samples in the inclusive range [-1, 1]. */
			int32_t phase_q16;
		} fractional_delay;
		struct {
			size_t delay_samples;
			/* 32768 returns the delayed signal at unity gain. */
			int32_t gain_q15;
		} echo;
	} parameters;
};

struct channel_bench_effect_state;

/* The caller owns this state. Initialize a zeroed instance once, then destroy
 * it before reuse. Effects run in specification order, and scheduled effect
 * state advances only inside its active window. No mutable state is global,
 * so independent channels may be advanced concurrently. Do not copy a live
 * instance because it is the unique owner of effect state. */
struct channel_bench {
	struct channel_bench_effect_state *effects;
	size_t effect_count;
	uint64_t samples_processed;
};

enum channel_bench_result channel_bench_init(
	struct channel_bench *bench,
	const struct channel_bench_effect_spec *effects,
	size_t effect_count);
void channel_bench_destroy(struct channel_bench *bench);
enum channel_bench_result channel_bench_process(
	struct channel_bench *bench, int16_t *samples, size_t count);

/* Small deterministic excitation source shared by generated-channel tests.
 * Seed zero selects a documented nonzero seed instead of the xorshift lockup
 * state. The generator is explicitly caller-owned and chunk invariant. */
struct channel_bench_generator {
	uint32_t state;
};

void channel_bench_generator_init(
	struct channel_bench_generator *generator, uint32_t seed);
uint32_t channel_bench_generator_next(
	struct channel_bench_generator *generator);
enum channel_bench_result channel_bench_generate_uniform(
	struct channel_bench_generator *generator,
	int16_t *samples, size_t count, uint16_t peak);
enum channel_bench_result channel_bench_generate_bipolar(
	struct channel_bench_generator *generator,
	int16_t *samples, size_t count, uint16_t amplitude);

struct channel_bench_recording {
	int16_t *samples;
	size_t count;
};

/* Loads canonical little-endian signed 16-bit PCM. max_samples is mandatory
 * so an optional external evidence checkout cannot cause an unbounded read.
 * A missing file has its own result and may be reported as a test skip. */
enum channel_bench_result channel_bench_recording_load(
	const char *path, size_t max_samples,
	struct channel_bench_recording *recording);
void channel_bench_recording_release(
	struct channel_bench_recording *recording);

struct channel_bench_comparison {
	/* Positive lag means observed audio begins later than the reference. */
	ptrdiff_t lag_samples;
	size_t reference_start;
	size_t observed_start;
	size_t compared_samples;
	bool correlation_defined;
	double correlation;
	double fitted_gain;
	double fitted_bias;
	double raw_rmse;
	double fitted_rmse;
	uint32_t maximum_absolute_error;
	/* Relative to the compared window, or SIZE_MAX when samples are exact. */
	size_t first_difference;
};

enum channel_bench_result channel_bench_compare_window(
	const int16_t *reference, size_t reference_count,
	size_t reference_start, const int16_t *observed,
	size_t observed_count, size_t observed_start, size_t count,
	struct channel_bench_comparison *comparison);

/* Searches inclusive lags [-maximum_lag, maximum_lag], ranking absolute
 * centered correlation before fitted and raw error. Every candidate must
 * retain at least minimum_samples; callers choose that floor so a short,
 * accidentally correlated tail cannot masquerade as ground truth. */
enum channel_bench_result channel_bench_find_alignment(
	const int16_t *reference, size_t reference_count,
	const int16_t *observed, size_t observed_count,
	size_t maximum_lag, size_t minimum_samples,
	struct channel_bench_comparison *comparison);

const char *channel_bench_result_name(enum channel_bench_result result);

#endif
