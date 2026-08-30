// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_echo_reference.h"

#include "memory_region.h"

#include <limits.h>
#include <string.h>

static uint64_t add_u64_saturating(uint64_t left, uint64_t right)
{
	return UINT64_MAX - left < right ? UINT64_MAX : left + right;
}

static int64_t add_i64_saturating(int64_t left, int64_t right)
{
	if (right > 0 && left > INT64_MAX - right)
		return INT64_MAX;
	if (right < 0 && left < INT64_MIN - right)
		return INT64_MIN;
	return left + right;
}

static uint64_t sample_energy(int16_t sample)
{
	int64_t value = sample;

	return (uint64_t)(value * value);
}

static uint64_t i64_magnitude(int64_t value)
{
	return value >= 0 ? (uint64_t)value :
		(uint64_t)(-(value + 1)) + 1u;
}

static bool scan_state_is_valid(const struct pcm_echo_reference *reference)
{
	uint64_t last;

	if (reference->scan_count == 0)
		return reference->scan_start == 0 && reference->scan_step == 0 &&
		       reference->scan_current_index == 0 &&
		       reference->scan_pre_samples == 0 &&
		       reference->scan_required_fill == 0 &&
		       reference->scan_samples == 0 &&
		       reference->scan_epoch == 0 &&
		       reference->scan_input_energy == 0;
	if (reference->scan_count >
		    PCM_ECHO_REFERENCE_SCAN_MAX_CANDIDATES ||
	    reference->scan_start == 0 || reference->scan_step == 0 ||
	    reference->scan_current_index >= reference->scan_count ||
	    reference->scan_pre_samples >=
		    PCM_ECHO_REFERENCE_MAX_WINDOW_SAMPLES ||
	    reference->scan_required_fill >
		    PCM_ECHO_REFERENCE_HISTORY_SAMPLES ||
	    reference->scan_epoch > reference->scan_samples)
		return false;
	last = (uint64_t)reference->scan_start +
	       (uint64_t)(reference->scan_count - 1u) * reference->scan_step;
	return last <= PCM_ECHO_REFERENCE_MAX_WINDOW_DELAY_SAMPLES &&
	       last + reference->scan_pre_samples ==
		       reference->scan_required_fill;
}

bool pcm_echo_reference_is_valid(
	const struct pcm_echo_reference *reference)
{
	return reference != NULL &&
	       reference->write < PCM_ECHO_REFERENCE_HISTORY_SAMPLES &&
	       reference->fill <= PCM_ECHO_REFERENCE_HISTORY_SAMPLES &&
	       (reference->fill == PCM_ECHO_REFERENCE_HISTORY_SAMPLES ||
		reference->write == reference->fill) &&
	       scan_state_is_valid(reference);
}

enum pcm_echo_reference_result pcm_echo_reference_init(
	struct pcm_echo_reference *reference)
{
	struct pcm_echo_reference initialized;

	if (reference == NULL)
		return PCM_ECHO_REFERENCE_INVALID_ARGUMENT;
	memset(&initialized, 0, sizeof(initialized));
	if (!pcm_echo_reference_is_valid(&initialized))
		return PCM_ECHO_REFERENCE_INVALID_STATE;
	*reference = initialized;
	return PCM_ECHO_REFERENCE_OK;
}

enum pcm_echo_reference_result pcm_echo_reference_push(
	struct pcm_echo_reference *reference, int16_t sample)
{
	if (!pcm_echo_reference_is_valid(reference))
		return PCM_ECHO_REFERENCE_INVALID_STATE;
	reference->samples[reference->write] = sample;
	reference->write++;
	if (reference->write == PCM_ECHO_REFERENCE_HISTORY_SAMPLES)
		reference->write = 0;
	if (reference->fill < PCM_ECHO_REFERENCE_HISTORY_SAMPLES)
		reference->fill++;
	return PCM_ECHO_REFERENCE_OK;
}

static int16_t sample_at_age(
	const struct pcm_echo_reference *reference, uint32_t age)
{
	size_t index = reference->write +
		PCM_ECHO_REFERENCE_HISTORY_SAMPLES - age;

	if (index >= PCM_ECHO_REFERENCE_HISTORY_SAMPLES)
		index -= PCM_ECHO_REFERENCE_HISTORY_SAMPLES;
	return reference->samples[index];
}

enum pcm_echo_reference_result pcm_echo_reference_read_at_age(
	const struct pcm_echo_reference *reference, uint32_t age,
	int16_t *sample)
{
	if (!pcm_echo_reference_is_valid(reference))
		return PCM_ECHO_REFERENCE_INVALID_STATE;
	if (sample == NULL || age == 0 ||
	    age > PCM_ECHO_REFERENCE_HISTORY_SAMPLES ||
	    memory_regions_overlap(
		    reference, sizeof(*reference), sample, sizeof(*sample)))
		return PCM_ECHO_REFERENCE_INVALID_ARGUMENT;
	if (age > reference->fill)
		return PCM_ECHO_REFERENCE_NOT_READY;
	*sample = sample_at_age(reference, age);
	return PCM_ECHO_REFERENCE_OK;
}

enum pcm_echo_reference_result pcm_echo_reference_read_window(
	const struct pcm_echo_reference *reference, uint32_t first_age,
	int16_t *samples, size_t count)
{
	uint64_t last_age;
	size_t bytes;

	if (!pcm_echo_reference_is_valid(reference))
		return PCM_ECHO_REFERENCE_INVALID_STATE;
	if (samples == NULL || first_age == 0 || count == 0 ||
	    count > PCM_ECHO_REFERENCE_MAX_WINDOW_SAMPLES)
		return PCM_ECHO_REFERENCE_INVALID_ARGUMENT;
	last_age = (uint64_t)first_age + count - 1u;
	bytes = count * sizeof(*samples);
	if (last_age > PCM_ECHO_REFERENCE_HISTORY_SAMPLES ||
	    memory_regions_overlap(reference, sizeof(*reference),
			   samples, bytes))
		return PCM_ECHO_REFERENCE_INVALID_ARGUMENT;
	if (last_age > reference->fill)
		return PCM_ECHO_REFERENCE_NOT_READY;
	for (size_t index = 0; index < count; ++index)
		samples[index] = sample_at_age(
			reference, first_age + (uint32_t)index);
	return PCM_ECHO_REFERENCE_OK;
}

static void clear_scan(struct pcm_echo_reference *reference)
{
	memset(reference->scan_reference_energy, 0,
	       sizeof(reference->scan_reference_energy));
	memset(reference->scan_cross_energy, 0,
	       sizeof(reference->scan_cross_energy));
	reference->scan_input_energy = 0;
	reference->scan_start = 0;
	reference->scan_step = 0;
	reference->scan_count = 0;
	reference->scan_current_index = 0;
	reference->scan_pre_samples = 0;
	reference->scan_required_fill = 0;
	reference->scan_samples = 0;
	reference->scan_epoch = 0;
}

enum pcm_echo_reference_result pcm_echo_reference_start_scan(
	struct pcm_echo_reference *reference,
	uint32_t current_window_delay_samples,
	uint32_t pre_samples)
{
	uint32_t start;
	uint32_t stop;
	uint32_t step = PCM_ECHO_REFERENCE_SCAN_STEP;
	uint32_t count;
	uint32_t remainder;

	if (!pcm_echo_reference_is_valid(reference))
		return PCM_ECHO_REFERENCE_INVALID_STATE;
	if (current_window_delay_samples == 0 ||
	    current_window_delay_samples >
		    PCM_ECHO_REFERENCE_MAX_WINDOW_DELAY_SAMPLES ||
	    pre_samples >= PCM_ECHO_REFERENCE_MAX_WINDOW_SAMPLES)
		return PCM_ECHO_REFERENCE_INVALID_ARGUMENT;
	/* Always include the front of the ring on the first acquisition pass. */
	start = 1u;
	stop = current_window_delay_samples + PCM_ECHO_REFERENCE_SCAN_SPAN;
	if (stop > PCM_ECHO_REFERENCE_MAX_WINDOW_DELAY_SAMPLES)
		stop = PCM_ECHO_REFERENCE_MAX_WINDOW_DELAY_SAMPLES;
	if (stop > start) {
		uint32_t minimum_step =
			(stop - start) /
			PCM_ECHO_REFERENCE_SCAN_MAX_CANDIDATES + 1u;

		if (step < minimum_step)
			step = minimum_step;
	}
	remainder = (current_window_delay_samples - start) % step;
	start += remainder;
	count = (stop - start) / step + 1u;
	if (count == 0 || count > PCM_ECHO_REFERENCE_SCAN_MAX_CANDIDATES)
		return PCM_ECHO_REFERENCE_INVALID_STATE;

	clear_scan(reference);
	reference->scan_start = start;
	reference->scan_step = step;
	reference->scan_count = count;
	reference->scan_current_index =
		(current_window_delay_samples - start) / step;
	reference->scan_pre_samples = pre_samples;
	reference->scan_required_fill =
		start + (count - 1u) * step + pre_samples;
	return PCM_ECHO_REFERENCE_OK;
}

static long double correlation_score(
	uint64_t reference_energy, uint64_t input_energy,
	int64_t cross_energy)
{
	long double cross = (long double)i64_magnitude(cross_energy);

	if (reference_energy == 0 || input_energy == 0)
		return -1.0L;
	return cross * cross /
	       ((long double)reference_energy * input_energy);
}

static uint32_t correlation_ppm(long double score)
{
	long double scaled;

	if (score <= 0.0L)
		return 0;
	scaled = score * 1000000.0L;
	return scaled >= UINT32_MAX ? UINT32_MAX : (uint32_t)scaled;
}

static bool scan_report_is_due(const struct pcm_echo_reference *reference)
{
	uint32_t count = reference->scan_samples;

	if (count < PCM_ECHO_REFERENCE_SCAN_MIN_SAMPLES ||
	    reference->scan_epoch == count)
		return false;
	return count == PCM_ECHO_REFERENCE_SCAN_MIN_SAMPLES ||
	       count % PCM_ECHO_REFERENCE_SCAN_CADENCE == 0;
}

static bool find_best_scan(
	const struct pcm_echo_reference *reference, uint32_t *best_index,
	long double *best_score, long double *current_score)
{
	bool found = false;

	*best_index = 0;
	*best_score = -1.0L;
	*current_score = -1.0L;
	for (uint32_t index = 0; index < reference->scan_count; ++index) {
		long double score = correlation_score(
			reference->scan_reference_energy[index],
			reference->scan_input_energy,
			reference->scan_cross_energy[index]);

		if (index == reference->scan_current_index)
			*current_score = score;
		if (score >= 0.0L && (!found || score > *best_score)) {
			found = true;
			*best_index = index;
			*best_score = score;
		}
	}
	return found;
}

enum pcm_echo_reference_result pcm_echo_reference_scan_observe(
	struct pcm_echo_reference *reference, int16_t received_sample,
	struct pcm_echo_reference_scan_result *result)
{
	struct pcm_echo_reference_scan_result generated;
	uint32_t best_index;
	long double best_score;
	long double current_score;
	uint64_t input_energy;

	if (!pcm_echo_reference_is_valid(reference))
		return PCM_ECHO_REFERENCE_INVALID_STATE;
	if (reference->scan_count == 0)
		return PCM_ECHO_REFERENCE_INVALID_STATE;
	if (result == NULL ||
	    memory_regions_overlap(
		    reference, sizeof(*reference), result, sizeof(*result)))
		return PCM_ECHO_REFERENCE_INVALID_ARGUMENT;
	if (reference->fill < reference->scan_required_fill)
		return PCM_ECHO_REFERENCE_NOT_READY;

	input_energy = sample_energy(received_sample);
	reference->scan_input_energy = add_u64_saturating(
		reference->scan_input_energy, input_energy);
	for (uint32_t index = 0; index < reference->scan_count; ++index) {
		uint32_t age = reference->scan_start +
			index * reference->scan_step +
			reference->scan_pre_samples;
		int16_t sample = sample_at_age(reference, age);
		int64_t cross = (int64_t)sample * received_sample;

		reference->scan_reference_energy[index] = add_u64_saturating(
			reference->scan_reference_energy[index],
			sample_energy(sample));
		reference->scan_cross_energy[index] = add_i64_saturating(
			reference->scan_cross_energy[index], cross);
	}
	if (reference->scan_samples < UINT32_MAX)
		reference->scan_samples++;
	if (!scan_report_is_due(reference) ||
	    !find_best_scan(reference, &best_index, &best_score,
			    &current_score))
		return PCM_ECHO_REFERENCE_OK;

	generated.best_window_delay_samples = reference->scan_start +
		best_index * reference->scan_step;
	generated.current_window_delay_samples = reference->scan_start +
		reference->scan_current_index * reference->scan_step;
	generated.sample_count = reference->scan_samples;
	generated.best_correlation_ppm = correlation_ppm(best_score);
	generated.current_correlation_ppm =
		correlation_ppm(current_score);
	reference->scan_epoch = reference->scan_samples;
	*result = generated;
	return PCM_ECHO_REFERENCE_SCAN_READY;
}
