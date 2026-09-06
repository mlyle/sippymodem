// SPDX-License-Identifier: BSD-3-Clause

#include "v90_host_echo.h"

#include "integer_math.h"
#include "memory_region.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#define V90_HOST_ECHO_ADAPTATION 0.35
#define V90_HOST_ECHO_ENERGY_FLOOR 1048576.0
#define V90_HOST_ECHO_COEFFICIENT_MIN_SUM 0.010
#define V90_HOST_ECHO_READY_CORRELATION_PPM 125000u
#define V90_HOST_ECHO_SCAN_IMPROVEMENT 4u
/* Allow a bounded local return path beyond the Phase-2 reversal interval. */
#define V90_HOST_ECHO_PHASE2_DELAY_MARGIN_SAMPLES \
	(PCM_ECHO_REFERENCE_SAMPLE_RATE / 20)

struct training_step {
	float coefficients[V90_HOST_ECHO_FILTER_TAPS];
	uint64_t reference_energy;
	uint64_t input_energy;
	uint64_t error_energy;
	int64_t cross_energy;
	uint32_t train_samples;
	uint16_t train_peak;
	bool available;
};

static bool mode_is_valid(enum v90_host_echo_mode mode)
{
	return mode == V90_HOST_ECHO_BYPASS ||
	       mode == V90_HOST_ECHO_TRAIN ||
	       mode == V90_HOST_ECHO_FILTER;
}

static uint32_t scan_current_delay(
	const struct pcm_echo_reference *reference)
{
	return reference->scan_start +
	       reference->scan_current_index * reference->scan_step;
}

static bool header_is_valid(const struct v90_host_echo *echo)
{
	uint64_t last_age;
	uint32_t ready_min;

	if (echo == NULL || !pcm_echo_reference_is_valid(&echo->reference) ||
	    echo->reference.scan_count == 0 ||
	    echo->window_delay_samples == 0 ||
	    echo->window_delay_samples >
		    PCM_ECHO_REFERENCE_MAX_WINDOW_DELAY_SAMPLES ||
	    scan_current_delay(&echo->reference) != echo->window_delay_samples)
		return false;
	last_age = (uint64_t)echo->window_delay_samples +
		   V90_HOST_ECHO_FILTER_TAPS - 1u;
	if (last_age > PCM_ECHO_REFERENCE_HISTORY_SAMPLES)
		return false;
	ready_min = echo->delay_acquired ?
		V90_HOST_ECHO_RETIMED_TRAIN_MIN_SAMPLES :
		V90_HOST_ECHO_TRAIN_MIN_SAMPLES;
	return (!echo->weak_margin || echo->filter_ready) &&
	       (!echo->filter_ready ||
		(echo->train_samples >= ready_min &&
		 echo->train_peak >= V90_HOST_ECHO_READY_MIN_PEAK &&
		 echo->reference.fill >= last_age));
}

bool v90_host_echo_is_valid(const struct v90_host_echo *echo)
{
	if (!header_is_valid(echo))
		return false;
	for (size_t tap = 0; tap < V90_HOST_ECHO_FILTER_TAPS; ++tap) {
		if (!isfinite(echo->coefficients[tap]))
			return false;
	}
	return true;
}

static enum v90_host_echo_result restart_training(struct v90_host_echo *echo)
{
	memset(echo->coefficients, 0, sizeof(echo->coefficients));
	echo->reference_energy = 0;
	echo->input_energy = 0;
	echo->error_energy = 0;
	echo->cross_energy = 0;
	echo->quiet_samples = 0;
	echo->train_samples = 0;
	echo->train_peak = 0;
	echo->filter_ready = false;
	echo->weak_margin = false;
	/* A rejected window can contain the peer's preceding signal. Its
	 * energy must not dilute later acquisition after the peer becomes quiet.
	 * Retain TX history and the delay hypothesis, but restart both sets of
	 * evidence together, whether retrying this delay or acquiring a new one. */
	return pcm_echo_reference_start_scan(
		       &echo->reference, echo->window_delay_samples,
		       V90_HOST_ECHO_FILTER_PRE_SAMPLES) == PCM_ECHO_REFERENCE_OK ?
		V90_HOST_ECHO_OK : V90_HOST_ECHO_INVALID_STATE;
}

enum v90_host_echo_result v90_host_echo_init(
	struct v90_host_echo *echo)
{
	struct v90_host_echo initialized;

	if (echo == NULL)
		return V90_HOST_ECHO_INVALID_ARGUMENT;
	memset(&initialized, 0, sizeof(initialized));
	if (pcm_echo_reference_init(&initialized.reference) !=
		    PCM_ECHO_REFERENCE_OK)
		return V90_HOST_ECHO_INVALID_STATE;
	initialized.window_delay_samples =
		V90_HOST_ECHO_DEFAULT_WINDOW_DELAY_SAMPLES;
	if (pcm_echo_reference_start_scan(
		    &initialized.reference, initialized.window_delay_samples,
		    V90_HOST_ECHO_FILTER_PRE_SAMPLES) !=
		    PCM_ECHO_REFERENCE_OK ||
	    !v90_host_echo_is_valid(&initialized))
		return V90_HOST_ECHO_INVALID_STATE;
	*echo = initialized;
	return V90_HOST_ECHO_OK;
}

static bool training_is_pristine(const struct v90_host_echo *echo)
{
	if (echo->reference_energy != 0 || echo->input_energy != 0 ||
	    echo->error_energy != 0 || echo->cross_energy != 0 ||
	    echo->quiet_samples != 0 || echo->train_samples != 0 ||
	    echo->train_peak != 0 || echo->delay_acquired ||
	    echo->filter_ready || echo->weak_margin)
		return false;
	for (size_t tap = 0; tap < V90_HOST_ECHO_FILTER_TAPS; ++tap) {
		if (echo->coefficients[tap] != 0.0f)
			return false;
	}
	return true;
}

enum v90_host_echo_result v90_host_echo_seed_phase2_delay(
	struct v90_host_echo *echo, uint32_t round_trip_delay_samples)
{
	uint64_t return_delay;
	uint32_t window_delay;

	if (!header_is_valid(echo) || !training_is_pristine(echo))
		return V90_HOST_ECHO_INVALID_STATE;
	if (round_trip_delay_samples == 0)
		return V90_HOST_ECHO_INVALID_ARGUMENT;
	return_delay = (uint64_t)round_trip_delay_samples +
		V90_HOST_ECHO_PHASE2_DELAY_MARGIN_SAMPLES;
	if (return_delay > V90_HOST_ECHO_DEFAULT_RETURN_DELAY_SAMPLES)
		return_delay = V90_HOST_ECHO_DEFAULT_RETURN_DELAY_SAMPLES;
	/* A representable measured delay is a better starting hypothesis than
	 * the short default. Keep the scan and filter-readiness checks: ranging
	 * is not proof of the echo path, and out-of-history estimates still need
	 * the ordinary bounded search. */
	if (round_trip_delay_samples > return_delay &&
	    round_trip_delay_samples <=
		    PCM_ECHO_REFERENCE_MAX_WINDOW_DELAY_SAMPLES +
			    V90_HOST_ECHO_FILTER_PRE_SAMPLES)
		return_delay = round_trip_delay_samples;
	window_delay = return_delay > V90_HOST_ECHO_FILTER_PRE_SAMPLES ?
		(uint32_t)return_delay - V90_HOST_ECHO_FILTER_PRE_SAMPLES : 1u;
	if (pcm_echo_reference_start_scan(
		    &echo->reference, window_delay,
		    V90_HOST_ECHO_FILTER_PRE_SAMPLES) != PCM_ECHO_REFERENCE_OK)
		return V90_HOST_ECHO_INVALID_STATE;
	echo->window_delay_samples = window_delay;
	return V90_HOST_ECHO_OK;
}

bool v90_host_echo_is_ready(const struct v90_host_echo *echo)
{
	return header_is_valid(echo) && echo->filter_ready;
}

bool v90_host_echo_has_weak_margin(const struct v90_host_echo *echo)
{
	return v90_host_echo_is_ready(echo) && echo->weak_margin;
}

uint32_t v90_host_echo_window_delay_samples(
	const struct v90_host_echo *echo)
{
	return header_is_valid(echo) ? echo->window_delay_samples : 0;
}

static enum v90_host_echo_result read_filter_window(
	const struct v90_host_echo *echo,
	int16_t references[V90_HOST_ECHO_FILTER_TAPS])
{
	enum pcm_echo_reference_result result =
		pcm_echo_reference_read_window(
			&echo->reference, echo->window_delay_samples,
			references, V90_HOST_ECHO_FILTER_TAPS);

	return result == PCM_ECHO_REFERENCE_OK ?
		V90_HOST_ECHO_OK : V90_HOST_ECHO_INVALID_STATE;
}

static bool estimate_echo(
	const float coefficients[V90_HOST_ECHO_FILTER_TAPS],
	const int16_t references[V90_HOST_ECHO_FILTER_TAPS],
	double *estimate, double *energy)
{
	double predicted = 0.0;
	double accumulated_energy = 0.0;

	for (size_t tap = 0; tap < V90_HOST_ECHO_FILTER_TAPS; ++tap) {
		double coefficient = coefficients[tap];
		double reference = references[tap];

		if (!isfinite(coefficient))
			return false;
		predicted += coefficient * reference;
		accumulated_energy += reference * reference;
	}
	if (!isfinite(predicted) || !isfinite(accumulated_energy))
		return false;
	*estimate = predicted;
	*energy = accumulated_energy;
	return true;
}

static int16_t round_s16(double value)
{
	if (value >= INT16_MAX)
		return INT16_MAX;
	if (value <= INT16_MIN)
		return INT16_MIN;
	return (int16_t)(value >= 0.0 ? value + 0.5 : value - 0.5);
}

enum v90_host_echo_result v90_host_echo_predict(
	const struct v90_host_echo *echo, enum v90_host_echo_mode mode,
	int16_t received_sample, int16_t *cleaned_sample)
{
	int16_t references[V90_HOST_ECHO_FILTER_TAPS];
	int16_t generated = received_sample;
	double estimate;
	double energy;
	enum v90_host_echo_result result;

	if (!header_is_valid(echo))
		return V90_HOST_ECHO_INVALID_STATE;
	if (!mode_is_valid(mode) || cleaned_sample == NULL ||
	    memory_regions_overlap(
		    echo, sizeof(*echo), cleaned_sample, sizeof(*cleaned_sample)))
		return V90_HOST_ECHO_INVALID_ARGUMENT;
	if (mode == V90_HOST_ECHO_FILTER && echo->filter_ready) {
		result = read_filter_window(echo, references);
		if (result != V90_HOST_ECHO_OK)
			return result;
		if (!estimate_echo(
			    echo->coefficients, references, &estimate, &energy))
			return V90_HOST_ECHO_NUMERIC_FAILURE;
		generated = round_s16((double)received_sample - estimate);
	}
	*cleaned_sample = generated;
	return V90_HOST_ECHO_OK;
}

static uint64_t double_square_u64(double value)
{
	long double square = (long double)value * value;

	return square >= UINT64_MAX ? UINT64_MAX : (uint64_t)(square + 0.5L);
}

static enum v90_host_echo_result prepare_training_step(
	const struct v90_host_echo *echo, int16_t received_sample,
	struct training_step *step)
{
	int16_t references[V90_HOST_ECHO_FILTER_TAPS];
	double estimate;
	double energy;
	double error;
	double scale;
	double post_estimate = 0.0;
	enum pcm_echo_reference_result window_result;

	memset(step, 0, sizeof(*step));
	window_result = pcm_echo_reference_read_window(
		&echo->reference, echo->window_delay_samples, references,
		V90_HOST_ECHO_FILTER_TAPS);
	if (window_result == PCM_ECHO_REFERENCE_NOT_READY)
		return V90_HOST_ECHO_OK;
	if (window_result != PCM_ECHO_REFERENCE_OK)
		return V90_HOST_ECHO_INVALID_STATE;
	if (!estimate_echo(
		    echo->coefficients, references, &estimate, &energy))
		return V90_HOST_ECHO_NUMERIC_FAILURE;
	if (energy <= 0.0)
		return V90_HOST_ECHO_OK;
	error = (double)received_sample - estimate;
	scale = V90_HOST_ECHO_ADAPTATION * error /
		(energy + V90_HOST_ECHO_ENERGY_FLOOR);
	if (!isfinite(scale))
		return V90_HOST_ECHO_NUMERIC_FAILURE;
	for (size_t tap = 0; tap < V90_HOST_ECHO_FILTER_TAPS; ++tap) {
		double coefficient = echo->coefficients[tap] +
			scale * references[tap];

		if (!isfinite(coefficient) || coefficient > FLT_MAX ||
		    coefficient < -FLT_MAX)
			return V90_HOST_ECHO_NUMERIC_FAILURE;
		step->coefficients[tap] = (float)coefficient;
		post_estimate += step->coefficients[tap] * references[tap];
	}
	if (!isfinite(post_estimate))
		return V90_HOST_ECHO_NUMERIC_FAILURE;
	step->reference_energy = add_u64_saturating(
		echo->reference_energy,
		square_s16(references[V90_HOST_ECHO_FILTER_PRE_SAMPLES]));
	step->input_energy = add_u64_saturating(
		echo->input_energy, square_s16(received_sample));
	step->cross_energy = add_s64_saturating(
		echo->cross_energy,
		(int64_t)references[V90_HOST_ECHO_FILTER_PRE_SAMPLES] *
			received_sample);
	step->error_energy = add_u64_saturating(
		echo->error_energy,
		double_square_u64((double)received_sample - post_estimate));
	step->train_samples = echo->train_samples == UINT32_MAX ? UINT32_MAX :
		echo->train_samples + 1u;
	step->train_peak = echo->train_peak;
	if (step->train_peak < (uint32_t)magnitude_s16(received_sample))
		step->train_peak = (uint16_t)magnitude_s16(received_sample);
	step->available = true;
	return V90_HOST_ECHO_OK;
}

static bool correlation_at_least(
	uint64_t reference_energy, uint64_t input_energy,
	int64_t cross_energy)
{
	uint64_t cross;
	__uint128_t left;
	__uint128_t right;

	if (reference_energy == 0 || input_energy == 0)
		return false;
	cross = magnitude_s64(cross_energy);
	left = (__uint128_t)cross * cross;
	right = (__uint128_t)reference_energy * input_energy;
	return left >= right / 8u + (right % 8u != 0);
}

static bool correlation_is_ready(const struct v90_host_echo *echo)
{
	const struct pcm_echo_reference *reference = &echo->reference;

	if (correlation_at_least(
		    echo->reference_energy, echo->input_energy,
		    echo->cross_energy))
		return true;
	for (uint32_t index = 0; index < reference->scan_count; ++index) {
		uint32_t delay = reference->scan_start +
			index * reference->scan_step;
		uint32_t delta = delay > echo->window_delay_samples ?
			delay - echo->window_delay_samples :
			echo->window_delay_samples - delay;
		uint64_t scanned_training_samples =
			(uint64_t)reference->scan_samples *
			V90_HOST_ECHO_SCAN_INTERVAL_SAMPLES;

		if (delta > reference->scan_step ||
		    scanned_training_samples < echo->train_samples)
			continue;
		if (correlation_at_least(
			    reference->scan_reference_energy[index],
			    reference->scan_input_energy,
			    reference->scan_cross_energy[index]))
			return true;
	}
	return false;
}

static double coefficient_sum(const struct v90_host_echo *echo)
{
	double sum = 0.0;

	for (size_t tap = 0; tap < V90_HOST_ECHO_FILTER_TAPS; ++tap)
		sum += fabs((double)echo->coefficients[tap]);
	return sum;
}

static void maybe_mark_ready(struct v90_host_echo *echo)
{
	uint32_t train_min = echo->delay_acquired ?
		V90_HOST_ECHO_RETIMED_TRAIN_MIN_SAMPLES :
		V90_HOST_ECHO_TRAIN_MIN_SAMPLES;

	if (echo->filter_ready || echo->train_samples < train_min ||
	    echo->train_peak < V90_HOST_ECHO_READY_MIN_PEAK ||
	    coefficient_sum(echo) < V90_HOST_ECHO_COEFFICIENT_MIN_SUM ||
	    !correlation_is_ready(echo) || echo->input_energy == 0 ||
	    (__uint128_t)echo->error_energy * 2u > echo->input_energy)
		return;
	echo->weak_margin =
		(__uint128_t)echo->error_energy * 6u >= echo->input_energy;
	echo->filter_ready = true;
}

static bool scan_should_retime(
	const struct v90_host_echo *echo,
	const struct pcm_echo_reference_scan_result *scan)
{
	uint32_t delta =
		scan->best_window_delay_samples >
				scan->current_window_delay_samples ?
			scan->best_window_delay_samples -
				scan->current_window_delay_samples :
			scan->current_window_delay_samples -
				scan->best_window_delay_samples;
	uint64_t improvement =
		(uint64_t)scan->current_correlation_ppm *
		V90_HOST_ECHO_SCAN_IMPROVEMENT;

	return !echo->filter_ready &&
	       echo->train_peak >= V90_HOST_ECHO_READY_MIN_PEAK &&
	       delta > echo->reference.scan_step &&
	       scan->best_window_delay_samples != 0 &&
	       scan->best_window_delay_samples !=
		       scan->current_window_delay_samples &&
	       scan->best_correlation_ppm >=
		       V90_HOST_ECHO_READY_CORRELATION_PPM &&
	       scan->best_correlation_ppm >= improvement;
}

static void commit_training_step(
	struct v90_host_echo *echo, const struct training_step *step)
{
	memcpy(echo->coefficients, step->coefficients,
	       sizeof(echo->coefficients));
	echo->reference_energy = step->reference_energy;
	echo->input_energy = step->input_energy;
	echo->error_energy = step->error_energy;
	echo->cross_energy = step->cross_energy;
	echo->train_samples = step->train_samples;
	echo->train_peak = step->train_peak;
}

enum v90_host_echo_result v90_host_echo_advance(
	struct v90_host_echo *echo, enum v90_host_echo_mode mode,
	int16_t received_sample, int16_t cleaned_sample,
	int16_t transmitted_sample)
{
	struct pcm_echo_reference_scan_result scan;
	struct training_step training;
	uint32_t next_quiet;
	int16_t expected;
	enum v90_host_echo_result result;
	enum pcm_echo_reference_result reference_result;

	if (!header_is_valid(echo))
		return V90_HOST_ECHO_INVALID_STATE;
	if (!mode_is_valid(mode))
		return V90_HOST_ECHO_INVALID_ARGUMENT;
	result = v90_host_echo_predict(
		echo, mode, received_sample, &expected);
	if (result != V90_HOST_ECHO_OK)
		return result;
	if (expected != cleaned_sample)
		return V90_HOST_ECHO_SEQUENCE_MISMATCH;

	if (mode == V90_HOST_ECHO_TRAIN) {
		next_quiet = echo->quiet_samples == UINT32_MAX ? UINT32_MAX :
			echo->quiet_samples + 1u;
		memset(&training, 0, sizeof(training));
		if (next_quiet >= V90_HOST_ECHO_QUIET_SAMPLES) {
			result = prepare_training_step(
				echo, received_sample, &training);
			if (result != V90_HOST_ECHO_OK)
				return result;
			reference_result = PCM_ECHO_REFERENCE_NOT_READY;
			if (training.available &&
			    training.train_samples %
				    V90_HOST_ECHO_SCAN_INTERVAL_SAMPLES == 0)
				reference_result =
					pcm_echo_reference_scan_observe(
						&echo->reference,
						received_sample, &scan);
			if (reference_result != PCM_ECHO_REFERENCE_OK &&
			    reference_result != PCM_ECHO_REFERENCE_NOT_READY &&
			    reference_result !=
				    PCM_ECHO_REFERENCE_SCAN_READY)
				return V90_HOST_ECHO_INVALID_STATE;
			if (reference_result ==
				    PCM_ECHO_REFERENCE_SCAN_READY &&
			    scan_should_retime(echo, &scan)) {
				echo->window_delay_samples =
					scan.best_window_delay_samples;
				echo->delay_acquired = true;
				if (restart_training(echo) != V90_HOST_ECHO_OK)
					return V90_HOST_ECHO_INVALID_STATE;
				goto push_reference;
			}
		}
		echo->quiet_samples = next_quiet;
		if (training.available) {
			commit_training_step(echo, &training);
			maybe_mark_ready(echo);
		}
		if (!echo->filter_ready &&
		    echo->train_samples >= V90_HOST_ECHO_TRAIN_WINDOW_SAMPLES &&
		    restart_training(echo) != V90_HOST_ECHO_OK)
			return V90_HOST_ECHO_INVALID_STATE;
	}

push_reference:
	if (pcm_echo_reference_push(&echo->reference, transmitted_sample) !=
		    PCM_ECHO_REFERENCE_OK)
		return V90_HOST_ECHO_INVALID_STATE;
	return V90_HOST_ECHO_OK;
}
