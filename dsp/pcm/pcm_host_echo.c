// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_host_echo.h"

#include "integer_math.h"
#include "memory_region.h"
#include "sample_math.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#define PCM_HOST_ECHO_ADAPTATION 0.35
#define PCM_HOST_ECHO_ENERGY_FLOOR 1048576.0
#define PCM_HOST_ECHO_COEFFICIENT_MIN_SUM 0.010
/* A delay candidate sees one return, not the complete multipath model.
 * Require r-squared >= 1/32 to move an unqualified window; only the later
 * independent whole-filter prediction can authorize subtraction. */
#define PCM_HOST_ECHO_SCAN_CORRELATION_PPM 31250u
#define PCM_HOST_ECHO_SCAN_IMPROVEMENT 4u
/* Allow a bounded local return path beyond the Phase-2 reversal interval. */
#define PCM_HOST_ECHO_PHASE2_DELAY_MARGIN_SAMPLES \
	(PCM_ECHO_REFERENCE_SAMPLE_RATE / 20)

struct training_step {
	float coefficients[PCM_HOST_ECHO_FILTER_TAPS];
	uint64_t prediction_energy;
	uint64_t input_energy;
	uint64_t error_energy;
	int64_t prediction_cross_energy;
	uint32_t train_samples;
	uint16_t train_peak;
	bool available;
};

static bool mode_is_valid(enum pcm_host_echo_mode mode)
{
	return mode == PCM_HOST_ECHO_BYPASS ||
	       mode == PCM_HOST_ECHO_TRAIN ||
	       mode == PCM_HOST_ECHO_FILTER;
}

static uint32_t scan_current_delay(
	const struct pcm_echo_reference *reference)
{
	return reference->scan_start +
	       reference->scan_current_index * reference->scan_step;
}

static bool header_is_valid(const struct pcm_host_echo *echo)
{
	uint64_t last_age;
	uint32_t ready_min;

	if (echo == NULL || !pcm_echo_reference_is_valid(&echo->reference) ||
	    echo->timing.write >= PCM_HOST_ECHO_TIMING_BINS ||
	    echo->timing.fill > PCM_HOST_ECHO_TIMING_BINS ||
	    echo->timing.count >= PCM_HOST_ECHO_TIMING_WINDOW ||
	    echo->timing.candidate >= PCM_HOST_ECHO_TIMING_BINS ||
	    echo->reference.scan_count == 0 ||
	    echo->window_delay_samples == 0 ||
	    echo->window_delay_samples >
		    PCM_ECHO_REFERENCE_MAX_WINDOW_DELAY_SAMPLES ||
	    scan_current_delay(&echo->reference) != echo->window_delay_samples)
		return false;
	last_age = (uint64_t)echo->window_delay_samples +
		   PCM_HOST_ECHO_FILTER_TAPS - 1u;
	if (last_age > PCM_ECHO_REFERENCE_HISTORY_SAMPLES)
		return false;
	if (echo->retained_delay_samples &&
	    (echo->filter_ready || !echo->delay_acquired ||
	     echo->retained_delay_samples > PCM_ECHO_REFERENCE_MAX_WINDOW_DELAY_SAMPLES ||
	     echo->reference.fill < echo->retained_delay_samples + PCM_HOST_ECHO_FILTER_TAPS - 1u))
		return false;
	ready_min = echo->delay_acquired ?
		PCM_HOST_ECHO_RETIMED_TRAIN_MIN_SAMPLES :
		PCM_HOST_ECHO_TRAIN_MIN_SAMPLES;
	return (!echo->weak_margin || echo->filter_ready) &&
	       (!echo->filter_ready ||
		(echo->train_samples >= ready_min &&
		 echo->train_peak >= PCM_HOST_ECHO_READY_MIN_PEAK &&
		 echo->reference.fill >= last_age));
}

bool pcm_host_echo_is_valid(const struct pcm_host_echo *echo)
{
	if (!header_is_valid(echo))
		return false;
	for (size_t tap = 0; tap < PCM_HOST_ECHO_FILTER_TAPS; ++tap) {
		if (!isfinite(echo->coefficients[tap]) ||
		    !isfinite(echo->retained_coefficients[tap]) ||
		    (!echo->retained_delay_samples && echo->retained_coefficients[tap] != 0.0f))
			return false;
	}
	return true;
}

static enum pcm_host_echo_result restart_training(struct pcm_host_echo *echo)
{
	memset(&echo->timing, 0, sizeof(echo->timing));
	memset(echo->coefficients, 0, sizeof(echo->coefficients));
	echo->prediction_energy = 0;
	echo->input_energy = 0;
	echo->error_energy = 0;
	echo->prediction_cross_energy = 0;
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
		       PCM_HOST_ECHO_FILTER_PRE_SAMPLES) == PCM_ECHO_REFERENCE_OK ?
		PCM_HOST_ECHO_OK : PCM_HOST_ECHO_INVALID_STATE;
}

enum pcm_host_echo_result pcm_host_echo_init(
	struct pcm_host_echo *echo)
{
	if (echo == NULL)
		return PCM_HOST_ECHO_INVALID_ARGUMENT;
	memset(echo, 0, sizeof(*echo));
	echo->window_delay_samples =
		PCM_HOST_ECHO_DEFAULT_WINDOW_DELAY_SAMPLES;
	if (pcm_echo_reference_start_scan(
		    &echo->reference, echo->window_delay_samples,
		    PCM_HOST_ECHO_FILTER_PRE_SAMPLES) !=
		    PCM_ECHO_REFERENCE_OK ||
	    !pcm_host_echo_is_valid(echo))
		return PCM_HOST_ECHO_INVALID_STATE;
	return PCM_HOST_ECHO_OK;
}

static bool training_is_pristine(const struct pcm_host_echo *echo)
{
	if (echo->prediction_energy != 0 || echo->input_energy != 0 ||
	    echo->error_energy != 0 || echo->prediction_cross_energy != 0 ||
	    echo->quiet_samples != 0 || echo->train_samples != 0 ||
	    echo->train_peak != 0 || echo->delay_acquired ||
	    echo->filter_ready || echo->weak_margin || echo->retained_delay_samples)
		return false;
	for (size_t tap = 0; tap < PCM_HOST_ECHO_FILTER_TAPS; ++tap) {
		if (echo->coefficients[tap] != 0.0f ||
		    echo->retained_coefficients[tap] != 0.0f)
			return false;
	}
	return true;
}

enum pcm_host_echo_result pcm_host_echo_seed_phase2_delay(
	struct pcm_host_echo *echo, uint32_t round_trip_delay_samples)
{
	uint64_t return_delay;
	uint32_t window_delay;

	if (!header_is_valid(echo) || !training_is_pristine(echo))
		return PCM_HOST_ECHO_INVALID_STATE;
	if (round_trip_delay_samples == 0)
		return PCM_HOST_ECHO_INVALID_ARGUMENT;
	return_delay = (uint64_t)round_trip_delay_samples +
		PCM_HOST_ECHO_PHASE2_DELAY_MARGIN_SAMPLES;
	if (return_delay > PCM_HOST_ECHO_DEFAULT_RETURN_DELAY_SAMPLES)
		return_delay = PCM_HOST_ECHO_DEFAULT_RETURN_DELAY_SAMPLES;
	/* A representable measured delay is a better starting hypothesis than
	 * the short default. Keep the scan and filter-readiness checks: ranging
	 * is not proof of the echo path, and out-of-history estimates still need
	 * the ordinary bounded search. */
	if (round_trip_delay_samples > return_delay &&
	    round_trip_delay_samples <=
		    PCM_ECHO_REFERENCE_MAX_WINDOW_DELAY_SAMPLES +
			    PCM_HOST_ECHO_FILTER_PRE_SAMPLES)
		return_delay = round_trip_delay_samples;
	window_delay = return_delay > PCM_HOST_ECHO_FILTER_PRE_SAMPLES ?
		(uint32_t)return_delay - PCM_HOST_ECHO_FILTER_PRE_SAMPLES : 1u;
	if (pcm_echo_reference_start_scan(
		    &echo->reference, window_delay,
		    PCM_HOST_ECHO_FILTER_PRE_SAMPLES) != PCM_ECHO_REFERENCE_OK)
		return PCM_HOST_ECHO_INVALID_STATE;
	echo->window_delay_samples = window_delay;
	return PCM_HOST_ECHO_OK;
}

bool pcm_host_echo_is_ready(const struct pcm_host_echo *echo)
{
	return header_is_valid(echo) && (echo->filter_ready || echo->retained_delay_samples);
}

bool pcm_host_echo_has_weak_margin(const struct pcm_host_echo *echo)
{
	return pcm_host_echo_is_ready(echo) && (echo->weak_margin || echo->retained_delay_samples);
}

uint32_t pcm_host_echo_window_delay_samples(
	const struct pcm_host_echo *echo)
{
	return header_is_valid(echo) ? echo->window_delay_samples : 0;
}

static enum pcm_host_echo_result read_filter_window(
	const struct pcm_host_echo *echo,
	int16_t references[PCM_HOST_ECHO_FILTER_TAPS])
{
	uint32_t delay = echo->retained_delay_samples ?
		echo->retained_delay_samples : echo->window_delay_samples;
	enum pcm_echo_reference_result result =
		pcm_echo_reference_read_window(
			&echo->reference, delay,
			references, PCM_HOST_ECHO_FILTER_TAPS);

	return result == PCM_ECHO_REFERENCE_OK ?
		PCM_HOST_ECHO_OK : PCM_HOST_ECHO_INVALID_STATE;
}

static bool estimate_echo(
	const float coefficients[PCM_HOST_ECHO_FILTER_TAPS],
	const int16_t references[PCM_HOST_ECHO_FILTER_TAPS],
	double *estimate, double *energy)
{
	double predicted = 0.0;
	double accumulated_energy = 0.0;

	for (size_t tap = 0; tap < PCM_HOST_ECHO_FILTER_TAPS; ++tap) {
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

enum pcm_host_echo_result pcm_host_echo_predict(
	const struct pcm_host_echo *echo, enum pcm_host_echo_mode mode,
	int16_t received_sample, int16_t *cleaned_sample)
{
	int16_t references[PCM_HOST_ECHO_FILTER_TAPS];
	int16_t generated = received_sample;
	double estimate;
	double energy;
	enum pcm_host_echo_result result;

	if (!header_is_valid(echo))
		return PCM_HOST_ECHO_INVALID_STATE;
	if (!mode_is_valid(mode) || cleaned_sample == NULL ||
	    memory_regions_overlap(
		    echo, sizeof(*echo), cleaned_sample, sizeof(*cleaned_sample)))
		return PCM_HOST_ECHO_INVALID_ARGUMENT;
	if (mode == PCM_HOST_ECHO_FILTER && (echo->filter_ready || echo->retained_delay_samples)) {
		const float *coefficients = echo->retained_delay_samples ?
			echo->retained_coefficients : echo->coefficients;

		result = read_filter_window(echo, references);
		if (result != PCM_HOST_ECHO_OK)
			return result;
		if (!estimate_echo(
			    coefficients, references, &estimate, &energy))
			return PCM_HOST_ECHO_NUMERIC_FAILURE;
		generated = sample_round_s16((double)received_sample - estimate);
	}
	*cleaned_sample = generated;
	return PCM_HOST_ECHO_OK;
}

static uint64_t double_square_u64(double value)
{
	long double square = (long double)value * value;

	return square >= UINT64_MAX ? UINT64_MAX : (uint64_t)(square + 0.5L);
}

static int64_t round_s64(double value)
{
	if (value >= (double)INT64_MAX)
		return INT64_MAX;
	if (value <= (double)INT64_MIN)
		return INT64_MIN;
	return (int64_t)(value >= 0.0 ? value + 0.5 : value - 0.5);
}

static enum pcm_host_echo_result prepare_training_step(
	const struct pcm_host_echo *echo, int16_t received_sample,
	struct training_step *step)
{
	int16_t references[PCM_HOST_ECHO_FILTER_TAPS];
	double estimate;
	double energy;
	double error;
	double scale;
	double post_estimate = 0.0;
	enum pcm_echo_reference_result window_result;

	memset(step, 0, sizeof(*step));
	window_result = pcm_echo_reference_read_window(
		&echo->reference, echo->window_delay_samples, references,
		PCM_HOST_ECHO_FILTER_TAPS);
	if (window_result == PCM_ECHO_REFERENCE_NOT_READY)
		return PCM_HOST_ECHO_OK;
	if (window_result != PCM_ECHO_REFERENCE_OK)
		return PCM_HOST_ECHO_INVALID_STATE;
	if (!estimate_echo(
		    echo->coefficients, references, &estimate, &energy))
		return PCM_HOST_ECHO_NUMERIC_FAILURE;
	if (energy <= 0.0)
		return PCM_HOST_ECHO_OK;
	error = (double)received_sample - estimate;
	scale = PCM_HOST_ECHO_ADAPTATION * error /
		(energy + PCM_HOST_ECHO_ENERGY_FLOOR);
	if (!isfinite(scale))
		return PCM_HOST_ECHO_NUMERIC_FAILURE;
	for (size_t tap = 0; tap < PCM_HOST_ECHO_FILTER_TAPS; ++tap) {
		double coefficient = echo->coefficients[tap] +
			scale * references[tap];

		if (!isfinite(coefficient) || coefficient > FLT_MAX ||
		    coefficient < -FLT_MAX)
			return PCM_HOST_ECHO_NUMERIC_FAILURE;
		step->coefficients[tap] = (float)coefficient;
		post_estimate += step->coefficients[tap] * references[tap];
	}
	if (!isfinite(post_estimate))
		return PCM_HOST_ECHO_NUMERIC_FAILURE;
	/* A spread-out return can have no strongly correlated individual delay
	 * bin. Qualify the full model instead, using its prediction BEFORE the
	 * current input adapts it; a post-update fit can correlate with noise. */
	step->prediction_energy = add_u64_saturating(
		echo->prediction_energy,
		double_square_u64(estimate));
	step->input_energy = add_u64_saturating(
		echo->input_energy, square_s16(received_sample));
	step->prediction_cross_energy = add_s64_saturating(
		echo->prediction_cross_energy,
		round_s64((double)received_sample * estimate));
	step->error_energy = add_u64_saturating(
		echo->error_energy,
		double_square_u64((double)received_sample - post_estimate));
	step->train_samples = echo->train_samples == UINT32_MAX ? UINT32_MAX :
		echo->train_samples + 1u;
	step->train_peak = echo->train_peak;
	if (step->train_peak < (uint32_t)magnitude_s16(received_sample))
		step->train_peak = (uint16_t)magnitude_s16(received_sample);
	step->available = true;
	return PCM_HOST_ECHO_OK;
}

static bool correlation_at_least(
	uint64_t prediction_energy, uint64_t input_energy,
	int64_t cross_energy)
{
	uint64_t cross;
	__uint128_t left;
	__uint128_t right;

	if (prediction_energy == 0 || input_energy == 0)
		return false;
	cross = magnitude_s64(cross_energy);
	left = (__uint128_t)cross * cross;
	right = (__uint128_t)prediction_energy * input_energy;
	return left >= right / 8u + (right % 8u != 0);
}

static double coefficient_sum(const struct pcm_host_echo *echo)
{
	double sum = 0.0;

	for (size_t tap = 0; tap < PCM_HOST_ECHO_FILTER_TAPS; ++tap)
		sum += fabs((double)echo->coefficients[tap]);
	return sum;
}

static void maybe_mark_ready(struct pcm_host_echo *echo)
{
	uint32_t train_min = echo->delay_acquired ?
		PCM_HOST_ECHO_RETIMED_TRAIN_MIN_SAMPLES :
		PCM_HOST_ECHO_TRAIN_MIN_SAMPLES;

	if (echo->filter_ready || echo->train_samples < train_min ||
	    echo->train_peak < PCM_HOST_ECHO_READY_MIN_PEAK ||
	    coefficient_sum(echo) < PCM_HOST_ECHO_COEFFICIENT_MIN_SUM ||
	    !correlation_at_least(echo->prediction_energy, echo->input_energy,
				  echo->prediction_cross_energy) ||
	    (__uint128_t)echo->error_energy * 2u > echo->input_energy)
		return;
	bool weak = (__uint128_t)echo->error_energy * 6u >= echo->input_energy;

	/* A newly acquired partial model is not an improvement over the
	 * qualified one retained for short training. Require stronger margin. */
	if (echo->retained_delay_samples && weak)
		return;
	echo->weak_margin = weak;
	echo->filter_ready = true;
	echo->retained_delay_samples = 0;
	memset(echo->retained_coefficients, 0, sizeof(echo->retained_coefficients));
}

static bool scan_should_retime(
	const struct pcm_host_echo *echo,
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
		PCM_HOST_ECHO_SCAN_IMPROVEMENT;

	/* A weak model may cover only later returns. Allow an earlier peak
	 * outside its window to widen coverage without discarding that model. */
	bool extend_weak_window = echo->filter_ready && echo->weak_margin &&
		scan->best_window_delay_samples + PCM_HOST_ECHO_FILTER_PRE_SAMPLES +
			echo->reference.scan_step < scan->current_window_delay_samples;

	return (!echo->filter_ready || extend_weak_window) &&
	       echo->train_peak >= PCM_HOST_ECHO_READY_MIN_PEAK &&
	       delta > echo->reference.scan_step &&
	       scan->best_window_delay_samples != 0 &&
	       scan->best_window_delay_samples !=
		       scan->current_window_delay_samples &&
	       scan->best_correlation_ppm >=
		       PCM_HOST_ECHO_SCAN_CORRELATION_PPM &&
	       (extend_weak_window || scan->best_correlation_ppm >= improvement);
}

static void commit_training_step(
	struct pcm_host_echo *echo, const struct training_step *step)
{
	memcpy(echo->coefficients, step->coefficients,
	       sizeof(echo->coefficients));
	echo->prediction_energy = step->prediction_energy;
	echo->input_energy = step->input_energy;
	echo->error_energy = step->error_energy;
	echo->prediction_cross_energy = step->prediction_cross_energy;
	echo->train_samples = step->train_samples;
	echo->train_peak = step->train_peak;
}

static bool timing_is_qualified(
	const struct pcm_host_echo_timing_evidence *e,
	unsigned samples, unsigned strength)
{
	/* Require useful cancellation as well as correlation. Compare against
	 * the current delay on the same input, including the peer's signal.
	 * Strength is sample_count * r-squared: require 9 independently in each
	 * window and 36 together. These are timing-confidence gates, not SNR
	 * estimates; agreement and a half-prediction-energy improvement are also
	 * required so unrelated input cannot win just by choosing another bin. */
	__int128 improvement = 2 * ((__int128)e->cross - e->current_cross) -
		((__int128)e->energy - e->current_energy);

	return e->cross > 0 && e->energy != 0 &&
	       (__uint128_t)e->cross * 2 >= e->energy &&
	       (__uint128_t)e->cross * e->cross * samples >=
		       (__uint128_t)strength * e->input * e->energy &&
	       improvement > 0 && (__uint128_t)improvement * 2 >= e->energy;
}

static void track_timing(
	struct pcm_host_echo *echo, int16_t received, int16_t cleaned)
{
	struct pcm_host_echo_timing *t = &echo->timing;
	const unsigned bins = PCM_HOST_ECHO_TIMING_BINS;
	const unsigned radius = PCM_HOST_ECHO_TIMING_RADIUS;
	unsigned index = t->write;
	int16_t input;

	/* Clipped subtraction does not describe the frozen filter prediction. */
	if (cleaned == INT16_MIN || cleaned == INT16_MAX ||
	    received == INT16_MIN || received == INT16_MAX) {
		memset(t, 0, sizeof(*t));
		return;
	}
	t->prediction[index] = (int32_t)received - cleaned;
	t->received[index] = received;
	t->write = (index + 1) % bins;
	if (t->fill < bins && ++t->fill < bins)
		return;
	/* Delay the observation so earlier AND later predictions are available.
	 * Reuse already computed predictions; do not run one FIR per candidate. */
	input = t->received[(index + bins - radius) % bins];
	t->input += square_s16(input);
	for (unsigned candidate = 0; candidate < bins; ++candidate) {
		int32_t predicted = t->prediction[index];

		t->cross[candidate] += (int64_t)input * predicted;
		t->energy[candidate] += (int64_t)predicted * predicted;
		index = index == 0 ? bins - 1 : index - 1;
	}
	if (++t->count != PCM_HOST_ECHO_TIMING_WINDOW)
		return;
	unsigned best = radius;
	for (unsigned candidate = 0; candidate < bins; ++candidate)
		if (2 * t->cross[candidate] - (int64_t)t->energy[candidate] >
		    2 * t->cross[best] - (int64_t)t->energy[best])
			best = candidate;
	struct pcm_host_echo_timing_evidence evidence = {
		.cross = t->cross[best], .current_cross = t->cross[radius],
		.energy = t->energy[best], .current_energy = t->energy[radius],
		.input = t->input,
	};
	bool qualified = timing_is_qualified(
		&evidence, PCM_HOST_ECHO_TIMING_WINDOW, 9);
	if (t->second && t->first_qualified && qualified && best == t->candidate) {
		struct pcm_host_echo_timing_evidence combined = {
			.cross = evidence.cross + t->first.cross,
			.current_cross = evidence.current_cross + t->first.current_cross,
			.energy = evidence.energy + t->first.energy,
			.current_energy = evidence.current_energy + t->first.current_energy,
			.input = evidence.input + t->first.input,
		};
		int64_t delay = (int64_t)echo->window_delay_samples + best - radius;

		/* Two disjoint windows must choose the same useful delay, and
		 * the combined normalized correlation must be stronger still. */
		if (timing_is_qualified(&combined, 2 * PCM_HOST_ECHO_TIMING_WINDOW, 36) &&
		    delay > 0 && delay <= PCM_ECHO_REFERENCE_MAX_WINDOW_DELAY_SAMPLES &&
		    (uint64_t)delay + PCM_HOST_ECHO_FILTER_TAPS - 1 <= echo->reference.fill &&
		    pcm_echo_reference_start_scan(&echo->reference, (uint32_t)delay,
					  PCM_HOST_ECHO_FILTER_PRE_SAMPLES) == PCM_ECHO_REFERENCE_OK) {
			echo->window_delay_samples = (uint32_t)delay;
			memset(t, 0, sizeof(*t));
			return;
		}
	}
	t->second = !t->second;
	t->first = evidence;
	t->candidate = best;
	t->first_qualified = qualified;
	t->count = 0;
	t->input = 0;
	memset(t->cross, 0, sizeof(t->cross));
	memset(t->energy, 0, sizeof(t->energy));
}

static enum pcm_host_echo_result advance_predicted(
	struct pcm_host_echo *echo, enum pcm_host_echo_mode mode,
	int16_t received_sample, int16_t cleaned_sample,
	int16_t transmitted_sample)
{
	struct pcm_echo_reference_scan_result scan;
	struct training_step training;
	uint32_t next_quiet;
	enum pcm_host_echo_result result;
	enum pcm_echo_reference_result reference_result;

	if (mode != PCM_HOST_ECHO_FILTER && echo->timing.fill != 0)
		memset(&echo->timing, 0, sizeof(echo->timing));
	if (mode == PCM_HOST_ECHO_FILTER && echo->filter_ready)
		track_timing(echo, received_sample, cleaned_sample);

	if (mode == PCM_HOST_ECHO_TRAIN) {
		next_quiet = echo->quiet_samples == UINT32_MAX ? UINT32_MAX :
			echo->quiet_samples + 1u;
		if (next_quiet >= PCM_HOST_ECHO_QUIET_SAMPLES) {
			result = prepare_training_step(
				echo, received_sample, &training);
			if (result != PCM_HOST_ECHO_OK)
				return result;
			reference_result = PCM_ECHO_REFERENCE_NOT_READY;
			if (training.available &&
			    training.train_samples %
				    PCM_HOST_ECHO_SCAN_INTERVAL_SAMPLES == 0)
				reference_result =
					pcm_echo_reference_scan_observe(
						&echo->reference,
						received_sample, &scan);
			if (reference_result != PCM_ECHO_REFERENCE_OK &&
			    reference_result != PCM_ECHO_REFERENCE_NOT_READY &&
			    reference_result !=
				    PCM_ECHO_REFERENCE_SCAN_READY)
				return PCM_HOST_ECHO_INVALID_STATE;
			/* Qualify the current model before a completed scan considers
			 * discarding it just to recenter the same covered echo. */
			if (training.available) {
				commit_training_step(echo, &training);
				maybe_mark_ready(echo);
			}
			if (reference_result ==
				    PCM_ECHO_REFERENCE_SCAN_READY &&
			    scan_should_retime(echo, &scan)) {
				if (echo->filter_ready) {
					memcpy(echo->retained_coefficients, echo->coefficients,
					       sizeof(echo->coefficients));
					echo->retained_delay_samples = echo->window_delay_samples;
				}
				echo->window_delay_samples =
					scan.best_window_delay_samples;
				echo->delay_acquired = true;
				if (restart_training(echo) != PCM_HOST_ECHO_OK)
					return PCM_HOST_ECHO_INVALID_STATE;
				goto push_reference;
			}
		}
		echo->quiet_samples = next_quiet;
		if (!echo->filter_ready &&
		    echo->train_samples >= PCM_HOST_ECHO_TRAIN_WINDOW_SAMPLES &&
		    restart_training(echo) != PCM_HOST_ECHO_OK)
			return PCM_HOST_ECHO_INVALID_STATE;
	}

push_reference:
	if (pcm_echo_reference_push(&echo->reference, transmitted_sample) !=
		    PCM_ECHO_REFERENCE_OK)
		return PCM_HOST_ECHO_INVALID_STATE;
	return PCM_HOST_ECHO_OK;
}

enum pcm_host_echo_result pcm_host_echo_advance_predicted(
	struct pcm_host_echo *echo, enum pcm_host_echo_mode mode,
	int16_t received_sample, int16_t cleaned_sample,
	int16_t transmitted_sample)
{
	if (!header_is_valid(echo))
		return PCM_HOST_ECHO_INVALID_STATE;
	if (!mode_is_valid(mode))
		return PCM_HOST_ECHO_INVALID_ARGUMENT;
	return advance_predicted(echo, mode, received_sample, cleaned_sample,
				 transmitted_sample);
}

enum pcm_host_echo_result pcm_host_echo_advance(
	struct pcm_host_echo *echo, enum pcm_host_echo_mode mode,
	int16_t received_sample, int16_t cleaned_sample,
	int16_t transmitted_sample)
{
	int16_t expected;
	enum pcm_host_echo_result result = pcm_host_echo_predict(
		echo, mode, received_sample, &expected);

	if (result != PCM_HOST_ECHO_OK)
		return result;
	if (expected != cleaned_sample)
		return PCM_HOST_ECHO_SEQUENCE_MISMATCH;
	return advance_predicted(echo, mode, received_sample, cleaned_sample,
				 transmitted_sample);
}
