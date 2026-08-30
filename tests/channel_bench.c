// SPDX-License-Identifier: BSD-3-Clause

#include "channel_bench.h"

#include <fractional_delay.h>
#include <g711.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
	CHANNEL_BENCH_DEFAULT_SEED = 0x6d2b79f5u,
	CHANNEL_BENCH_Q15_ONE = 32768,
};

struct channel_bench_effect_state {
	struct channel_bench_effect_spec spec;
	struct channel_bench_generator generator;
	struct fractional_delay fractional_delay;
	int16_t *history;
	size_t history_count;
	size_t history_cursor;
};

static int16_t saturate_sample(int64_t value)
{
	if (value > INT16_MAX)
		return INT16_MAX;
	if (value < INT16_MIN)
		return INT16_MIN;
	return (int16_t)value;
}

static int16_t scale_sample(int16_t sample, int32_t scale_q15)
{
	return saturate_sample(
		((int64_t)sample * scale_q15) / CHANNEL_BENCH_Q15_ONE);
}

void channel_bench_generator_init(
	struct channel_bench_generator *generator, uint32_t seed)
{
	if (generator != NULL)
		generator->state = seed != 0 ? seed : CHANNEL_BENCH_DEFAULT_SEED;
}

uint32_t channel_bench_generator_next(
	struct channel_bench_generator *generator)
{
	uint32_t value;

	if (generator == NULL)
		return 0;
	value = generator->state != 0 ? generator->state :
					       CHANNEL_BENCH_DEFAULT_SEED;
	value ^= value << 13;
	value ^= value >> 17;
	value ^= value << 5;
	generator->state = value;
	return value;
}

static int16_t uniform_sample(
	struct channel_bench_generator *generator, uint16_t peak)
{
	uint32_t random = channel_bench_generator_next(generator);
	uint64_t levels = (uint64_t)peak * 2u + 1u;
	uint32_t bucket = (uint32_t)(
		((uint64_t)(random & UINT32_C(0xffff)) * levels) >> 16);

	return (int16_t)((int32_t)bucket - peak);
}

enum channel_bench_result channel_bench_generate_uniform(
	struct channel_bench_generator *generator,
	int16_t *samples, size_t count, uint16_t peak)
{
	if (generator == NULL || (samples == NULL && count != 0) ||
	    peak > INT16_MAX)
		return CHANNEL_BENCH_INVALID_ARGUMENT;
	for (size_t index = 0; index < count; ++index)
		samples[index] = uniform_sample(generator, peak);
	return CHANNEL_BENCH_OK;
}

enum channel_bench_result channel_bench_generate_bipolar(
	struct channel_bench_generator *generator,
	int16_t *samples, size_t count, uint16_t amplitude)
{
	if (generator == NULL || (samples == NULL && count != 0) ||
	    amplitude > INT16_MAX)
		return CHANNEL_BENCH_INVALID_ARGUMENT;
	for (size_t index = 0; index < count; ++index) {
		uint32_t random = channel_bench_generator_next(generator);

		samples[index] = (random & UINT32_C(0x80000000)) != 0 ?
					 (int16_t)amplitude :
					 (int16_t)-(int32_t)amplitude;
	}
	return CHANNEL_BENCH_OK;
}

static void release_effect(struct channel_bench_effect_state *effect)
{
	free(effect->history);
	effect->history = NULL;
	effect->history_count = 0;
	effect->history_cursor = 0;
}

void channel_bench_destroy(struct channel_bench *bench)
{
	if (bench == NULL)
		return;
	for (size_t index = 0; index < bench->effect_count; ++index)
		release_effect(&bench->effects[index]);
	free(bench->effects);
	memset(bench, 0, sizeof(*bench));
}

static enum channel_bench_result prepare_effect(
	struct channel_bench_effect_state *state,
	const struct channel_bench_effect_spec *spec)
{
	size_t history_count = 0;

	if (spec->end_sample != 0 && spec->end_sample <= spec->start_sample)
		return CHANNEL_BENCH_INVALID_ARGUMENT;
	state->spec = *spec;
	switch (spec->kind) {
	case CHANNEL_BENCH_GAIN:
		break;
	case CHANNEL_BENCH_NOISE:
		if (spec->parameters.noise.peak > INT16_MAX)
			return CHANNEL_BENCH_INVALID_ARGUMENT;
		channel_bench_generator_init(
			&state->generator, spec->parameters.noise.seed);
		break;
	case CHANNEL_BENCH_DELAY:
		history_count = spec->parameters.delay.samples;
		break;
	case CHANNEL_BENCH_FRACTIONAL_DELAY:
		fractional_delay_init(&state->fractional_delay);
		if (fractional_delay_set_phase(
			    &state->fractional_delay,
			    spec->parameters.fractional_delay.phase_q16) !=
		    FRACTIONAL_DELAY_OK)
			return CHANNEL_BENCH_INVALID_ARGUMENT;
		break;
	case CHANNEL_BENCH_ECHO:
		history_count = spec->parameters.echo.delay_samples;
		break;
	case CHANNEL_BENCH_PCMU:
	case CHANNEL_BENCH_PCMA:
	case CHANNEL_BENCH_MUTE:
		break;
	default:
		return CHANNEL_BENCH_INVALID_ARGUMENT;
	}
	if ((spec->kind == CHANNEL_BENCH_DELAY ||
	     spec->kind == CHANNEL_BENCH_ECHO) &&
	    (history_count == 0 ||
	     history_count > CHANNEL_BENCH_MAX_HISTORY_SAMPLES))
		return CHANNEL_BENCH_INVALID_ARGUMENT;
	if (history_count != 0) {
		state->history = calloc(history_count, sizeof(*state->history));
		if (state->history == NULL)
			return CHANNEL_BENCH_NO_MEMORY;
		state->history_count = history_count;
	}
	return CHANNEL_BENCH_OK;
}

enum channel_bench_result channel_bench_init(
	struct channel_bench *bench,
	const struct channel_bench_effect_spec *effects,
	size_t effect_count)
{
	struct channel_bench initialized = { 0 };
	enum channel_bench_result result;

	if (bench == NULL || bench->effects != NULL ||
	    bench->effect_count != 0 || bench->samples_processed != 0 ||
	    (effects == NULL && effect_count != 0) ||
	    effect_count > CHANNEL_BENCH_MAX_EFFECTS)
		return CHANNEL_BENCH_INVALID_ARGUMENT;
	if (effect_count == 0)
		return CHANNEL_BENCH_OK;
	initialized.effects = calloc(effect_count, sizeof(*initialized.effects));
	if (initialized.effects == NULL)
		return CHANNEL_BENCH_NO_MEMORY;
	initialized.effect_count = effect_count;
	for (size_t index = 0; index < effect_count; ++index) {
		result = prepare_effect(&initialized.effects[index], &effects[index]);
		if (result != CHANNEL_BENCH_OK) {
			channel_bench_destroy(&initialized);
			return result;
		}
	}
	*bench = initialized;
	return CHANNEL_BENCH_OK;
}

static bool effect_is_active(
	const struct channel_bench_effect_state *effect, uint64_t sample)
{
	return sample >= effect->spec.start_sample &&
	       (effect->spec.end_sample == 0 || sample < effect->spec.end_sample);
}

static bool apply_effect(
	struct channel_bench_effect_state *effect,
	int16_t *samples, size_t count, uint64_t sample_start)
{
	for (size_t index = 0; index < count; ++index) {
		int16_t sample = samples[index];

		if (!effect_is_active(effect, sample_start + index))
			continue;
		switch (effect->spec.kind) {
		case CHANNEL_BENCH_GAIN:
			sample = scale_sample(
				sample, effect->spec.parameters.gain.scale_q15);
			break;
		case CHANNEL_BENCH_NOISE:
			sample = saturate_sample(
				(int64_t)sample +
				uniform_sample(
					&effect->generator,
					effect->spec.parameters.noise.peak));
			break;
		case CHANNEL_BENCH_DELAY: {
			int16_t delayed = effect->history[effect->history_cursor];

			effect->history[effect->history_cursor] = sample;
			effect->history_cursor =
				(effect->history_cursor + 1) % effect->history_count;
			sample = delayed;
			break;
		}
		case CHANNEL_BENCH_FRACTIONAL_DELAY:
			if (fractional_delay_process(
				    &effect->fractional_delay, sample, &sample) !=
			    FRACTIONAL_DELAY_OK)
				return false;
			break;
		case CHANNEL_BENCH_ECHO: {
			int16_t delayed = effect->history[effect->history_cursor];

			effect->history[effect->history_cursor] = sample;
			effect->history_cursor =
				(effect->history_cursor + 1) % effect->history_count;
			sample = saturate_sample(
				(int64_t)sample +
				((int64_t)delayed *
				 effect->spec.parameters.echo.gain_q15) /
					CHANNEL_BENCH_Q15_ONE);
			break;
		}
		case CHANNEL_BENCH_PCMU:
			sample = g711_decode_pcmu(g711_encode_pcmu(sample));
			break;
		case CHANNEL_BENCH_PCMA:
			sample = g711_decode_pcma(g711_encode_pcma(sample));
			break;
		case CHANNEL_BENCH_MUTE:
			sample = 0;
			break;
		}
		samples[index] = sample;
	}
	return true;
}

enum channel_bench_result channel_bench_process(
	struct channel_bench *bench, int16_t *samples, size_t count)
{
	if (bench == NULL || (samples == NULL && count != 0) ||
	    (bench->effects == NULL && bench->effect_count != 0) ||
	    count > UINT64_MAX - bench->samples_processed)
		return CHANNEL_BENCH_INVALID_ARGUMENT;
	for (size_t index = 0; index < bench->effect_count; ++index)
		if (!apply_effect(
			    &bench->effects[index], samples, count,
			    bench->samples_processed))
			return CHANNEL_BENCH_INVALID_ARGUMENT;
	bench->samples_processed += count;
	return CHANNEL_BENCH_OK;
}

void channel_bench_recording_release(
	struct channel_bench_recording *recording)
{
	if (recording == NULL)
		return;
	free(recording->samples);
	recording->samples = NULL;
	recording->count = 0;
}

static enum channel_bench_result read_recording(
	int descriptor, int16_t *samples, size_t bytes)
{
	uint8_t *raw = (uint8_t *)samples;
	size_t offset = 0;

	while (offset < bytes) {
		ssize_t received = read(descriptor, raw + offset, bytes - offset);

		if (received < 0 && errno == EINTR)
			continue;
		if (received <= 0)
			return CHANNEL_BENCH_IO_ERROR;
		offset += (size_t)received;
	}
	for (;;) {
		uint8_t extra;
		ssize_t received = read(descriptor, &extra, 1);

		if (received < 0 && errno == EINTR)
			continue;
		if (received < 0)
			return CHANNEL_BENCH_IO_ERROR;
		if (received != 0)
			return CHANNEL_BENCH_RECORDING_MALFORMED;
		break;
	}
	for (size_t index = 0; index < bytes / 2; ++index) {
		uint16_t value = (uint16_t)(
			raw[index * 2] | ((uint16_t)raw[index * 2 + 1] << 8));
		int32_t signed_value = value <= INT16_MAX ?
					       (int32_t)value :
					       (int32_t)value - 65536;

		samples[index] = (int16_t)signed_value;
	}
	return CHANNEL_BENCH_OK;
}

enum channel_bench_result channel_bench_recording_load(
	const char *path, size_t max_samples,
	struct channel_bench_recording *recording)
{
	struct channel_bench_recording loaded = { 0 };
	struct stat status;
	enum channel_bench_result result;
	size_t bytes;
	int descriptor;

	if (path == NULL || path[0] == '\0' || max_samples == 0 ||
	    recording == NULL || recording->samples != NULL ||
	    recording->count != 0)
		return CHANNEL_BENCH_INVALID_ARGUMENT;
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0) {
		return errno == ENOENT || errno == ENOTDIR ?
			       CHANNEL_BENCH_RECORDING_MISSING :
			       CHANNEL_BENCH_IO_ERROR;
	}
	if (fstat(descriptor, &status) != 0) {
		result = CHANNEL_BENCH_IO_ERROR;
		goto close;
	}
	if (!S_ISREG(status.st_mode) || status.st_size < 0 ||
	    (status.st_size & 1) != 0) {
		result = CHANNEL_BENCH_RECORDING_MALFORMED;
		goto close;
	}
	if ((uintmax_t)status.st_size / sizeof(int16_t) > max_samples ||
	    (uintmax_t)status.st_size > SIZE_MAX) {
		result = CHANNEL_BENCH_RECORDING_TOO_LARGE;
		goto close;
	}
	bytes = (size_t)status.st_size;
	if (bytes != 0) {
		loaded.samples = malloc(bytes);
		if (loaded.samples == NULL) {
			result = CHANNEL_BENCH_NO_MEMORY;
			goto close;
		}
	}
	result = read_recording(descriptor, loaded.samples, bytes);
	if (result != CHANNEL_BENCH_OK)
		goto close;
	loaded.count = bytes / sizeof(int16_t);

close:
	if (close(descriptor) != 0 && result == CHANNEL_BENCH_OK)
		result = CHANNEL_BENCH_IO_ERROR;
	if (result != CHANNEL_BENCH_OK) {
		channel_bench_recording_release(&loaded);
		return result;
	}
	*recording = loaded;
	return CHANNEL_BENCH_OK;
}

enum channel_bench_result channel_bench_compare_window(
	const int16_t *reference, size_t reference_count,
	size_t reference_start, const int16_t *observed,
	size_t observed_count, size_t observed_start, size_t count,
	struct channel_bench_comparison *comparison)
{
	struct channel_bench_comparison report = {
		.reference_start = reference_start,
		.observed_start = observed_start,
		.compared_samples = count,
		.first_difference = SIZE_MAX,
	};
	long double reference_sum = 0.0L;
	long double observed_sum = 0.0L;
	long double raw_error_squared = 0.0L;
	long double reference_energy = 0.0L;
	long double observed_energy = 0.0L;
	long double cross_energy = 0.0L;
	long double fitted_error_squared = 0.0L;
	long double reference_mean;
	long double observed_mean;
	long double fitted_gain;
	long double fitted_bias;

	if (reference == NULL || observed == NULL || comparison == NULL ||
	    count == 0 || reference_start > reference_count ||
	    observed_start > observed_count ||
	    count > reference_count - reference_start ||
	    count > observed_count - observed_start)
		return CHANNEL_BENCH_INVALID_ARGUMENT;
	for (size_t index = 0; index < count; ++index) {
		int32_t reference_sample = reference[reference_start + index];
		int32_t observed_sample = observed[observed_start + index];
		int32_t difference = observed_sample - reference_sample;
		uint32_t absolute = difference < 0 ?
					    (uint32_t)-(int64_t)difference :
					    (uint32_t)difference;

		reference_sum += reference_sample;
		observed_sum += observed_sample;
		raw_error_squared += (long double)difference * difference;
		if (absolute > report.maximum_absolute_error)
			report.maximum_absolute_error = absolute;
		if (difference != 0 && report.first_difference == SIZE_MAX)
			report.first_difference = index;
	}
	reference_mean = reference_sum / count;
	observed_mean = observed_sum / count;
	for (size_t index = 0; index < count; ++index) {
		long double centered_reference =
			reference[reference_start + index] - reference_mean;
		long double centered_observed =
			observed[observed_start + index] - observed_mean;

		reference_energy += centered_reference * centered_reference;
		observed_energy += centered_observed * centered_observed;
		cross_energy += centered_reference * centered_observed;
	}
	if (reference_energy > 0.0L) {
		fitted_gain = cross_energy / reference_energy;
		fitted_bias = observed_mean - fitted_gain * reference_mean;
	} else {
		fitted_gain = 0.0L;
		fitted_bias = observed_mean;
	}
	report.fitted_gain = (double)fitted_gain;
	report.fitted_bias = (double)fitted_bias;
	if (reference_energy > 0.0L && observed_energy > 0.0L) {
		long double correlation =
			cross_energy / sqrtl(reference_energy * observed_energy);

		if (correlation > 1.0L)
			correlation = 1.0L;
		else if (correlation < -1.0L)
			correlation = -1.0L;
		report.correlation = (double)correlation;
		report.correlation_defined = true;
	}
	for (size_t index = 0; index < count; ++index) {
		long double fitted = fitted_gain *
					     reference[reference_start + index] +
				     fitted_bias;
		long double residual = observed[observed_start + index] - fitted;

		fitted_error_squared += residual * residual;
	}
	report.raw_rmse = (double)sqrtl(raw_error_squared / count);
	report.fitted_rmse = (double)sqrtl(fitted_error_squared / count);
	*comparison = report;
	return CHANNEL_BENCH_OK;
}

static double comparison_score(
	const struct channel_bench_comparison *comparison)
{
	if (comparison->correlation_defined)
		return fabs(comparison->correlation);
	return comparison->raw_rmse == 0.0 ? 1.0 : -1.0;
}

static size_t lag_magnitude(ptrdiff_t lag)
{
	return lag >= 0 ? (size_t)lag : (size_t)-lag;
}

static bool comparison_is_better(
	const struct channel_bench_comparison *candidate,
	const struct channel_bench_comparison *best)
{
	double candidate_score = comparison_score(candidate);
	double best_score = comparison_score(best);

	if (candidate_score < best_score || candidate_score > best_score)
		return candidate_score > best_score;
	if (candidate->fitted_rmse < best->fitted_rmse ||
	    candidate->fitted_rmse > best->fitted_rmse)
		return candidate->fitted_rmse < best->fitted_rmse;
	if (candidate->raw_rmse < best->raw_rmse ||
	    candidate->raw_rmse > best->raw_rmse)
		return candidate->raw_rmse < best->raw_rmse;
	if (candidate->compared_samples != best->compared_samples)
		return candidate->compared_samples > best->compared_samples;
	return lag_magnitude(candidate->lag_samples) <
	       lag_magnitude(best->lag_samples);
}

static void consider_alignment(
	const int16_t *reference, size_t reference_count,
	const int16_t *observed, size_t observed_count,
	ptrdiff_t lag, size_t minimum_samples,
	struct channel_bench_comparison *best, bool *found)
{
	struct channel_bench_comparison candidate;
	size_t reference_start = lag < 0 ? (size_t)-lag : 0;
	size_t observed_start = lag > 0 ? (size_t)lag : 0;
	size_t reference_available;
	size_t observed_available;
	size_t count;

	if (reference_start > reference_count || observed_start > observed_count)
		return;
	reference_available = reference_count - reference_start;
	observed_available = observed_count - observed_start;
	count = reference_available < observed_available ?
			reference_available : observed_available;
	if (count < minimum_samples)
		return;
	if (channel_bench_compare_window(
		    reference, reference_count, reference_start,
		    observed, observed_count, observed_start, count,
		    &candidate) != CHANNEL_BENCH_OK)
		return;
	candidate.lag_samples = lag;
	if (!*found || comparison_is_better(&candidate, best)) {
		*best = candidate;
		*found = true;
	}
}

enum channel_bench_result channel_bench_find_alignment(
	const int16_t *reference, size_t reference_count,
	const int16_t *observed, size_t observed_count,
	size_t maximum_lag, size_t minimum_samples,
	struct channel_bench_comparison *comparison)
{
	struct channel_bench_comparison best = { 0 };
	bool found = false;

	if (reference == NULL || observed == NULL || comparison == NULL ||
	    reference_count == 0 || observed_count == 0 ||
	    minimum_samples == 0 || maximum_lag > PTRDIFF_MAX)
		return CHANNEL_BENCH_INVALID_ARGUMENT;
	for (size_t magnitude = maximum_lag; magnitude != 0; --magnitude) {
		consider_alignment(
			reference, reference_count, observed, observed_count,
			-(ptrdiff_t)magnitude, minimum_samples, &best, &found);
	}
	consider_alignment(
		reference, reference_count, observed, observed_count,
		0, minimum_samples, &best, &found);
	for (size_t magnitude = 1; magnitude <= maximum_lag; ++magnitude) {
		consider_alignment(
			reference, reference_count, observed, observed_count,
			(ptrdiff_t)magnitude, minimum_samples, &best, &found);
	}
	if (!found)
		return CHANNEL_BENCH_NO_COMPARABLE_WINDOW;
	*comparison = best;
	return CHANNEL_BENCH_OK;
}

const char *channel_bench_result_name(enum channel_bench_result result)
{
	switch (result) {
	case CHANNEL_BENCH_OK:
		return "ok";
	case CHANNEL_BENCH_INVALID_ARGUMENT:
		return "invalid channel bench argument";
	case CHANNEL_BENCH_NO_MEMORY:
		return "channel bench allocation failed";
	case CHANNEL_BENCH_RECORDING_MISSING:
		return "optional recording is missing";
	case CHANNEL_BENCH_RECORDING_TOO_LARGE:
		return "recording exceeds the configured sample limit";
	case CHANNEL_BENCH_RECORDING_MALFORMED:
		return "recording is not canonical signed 16-bit PCM";
	case CHANNEL_BENCH_IO_ERROR:
		return "recording read failed";
	case CHANNEL_BENCH_NO_COMPARABLE_WINDOW:
		return "recordings have no sufficiently large comparison window";
	}
	return "unknown channel bench result";
}
