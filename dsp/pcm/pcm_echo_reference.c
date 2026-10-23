// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_echo_reference.h"

#include "integer_math.h"
#include "memory_region.h"
#include "ring_history.h"
#include "wide_uint.h"

#include <string.h>

static bool scan_state_is_valid(const struct pcm_echo_reference *reference)
{
	if (reference->scan_count == 0)
		return reference->scan_current_index == 0 &&
		       reference->scan_pre_samples == 0 &&
		       reference->scan_samples == 0 &&
		       reference->scan_epoch == 0 &&
		       reference->scan_input_energy == 0 &&
		       reference->scan_input_sum == 0;
	if (reference->scan_count >
		    PCM_ECHO_REFERENCE_SCAN_MAX_CANDIDATES ||
	    reference->scan_current_index >= reference->scan_count ||
	    reference->scan_pre_samples >=
		    PCM_ECHO_REFERENCE_MAX_WINDOW_SAMPLES ||
	    reference->scan_epoch > reference->scan_samples)
		return false;
	return true;
}

bool pcm_echo_reference_is_valid(
	const struct pcm_echo_reference *reference)
{
	return reference != NULL &&
	       ring_history_state_is_valid(
		reference->write, reference->fill,
		PCM_ECHO_REFERENCE_HISTORY_SAMPLES) &&
	       scan_state_is_valid(reference);
}

enum pcm_echo_reference_result pcm_echo_reference_push(
	struct pcm_echo_reference *reference, int16_t sample)
{
	if (!pcm_echo_reference_is_valid(reference))
		return PCM_ECHO_REFERENCE_INVALID_STATE;
	reference->samples[reference->write] = sample;
	ring_history_advance(&reference->write, &reference->fill,
		PCM_ECHO_REFERENCE_HISTORY_SAMPLES);
	return PCM_ECHO_REFERENCE_OK;
}

static int16_t sample_at_age(
	const struct pcm_echo_reference *reference, uint32_t age)
{
	return reference->samples[ring_index_at_age(
		reference->write, PCM_ECHO_REFERENCE_HISTORY_SAMPLES, age)];
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
	memset(reference->scan_reference_sum, 0,
	       sizeof(reference->scan_reference_sum));
	reference->scan_input_sum = 0;
	reference->scan_input_energy = 0;
	reference->scan_count = 0;
	reference->scan_current_index = 0;
	reference->scan_pre_samples = 0;
	reference->scan_samples = 0;
	reference->scan_epoch = 0;
}

enum pcm_echo_reference_result pcm_echo_reference_start_scan(
	struct pcm_echo_reference *reference,
	uint32_t current_window_delay_samples,
	uint32_t pre_samples)
{
	uint32_t stop;
	uint32_t required_stop;

	if (!pcm_echo_reference_is_valid(reference))
		return PCM_ECHO_REFERENCE_INVALID_STATE;
	if (current_window_delay_samples == 0 ||
	    current_window_delay_samples >
		    PCM_ECHO_REFERENCE_MAX_WINDOW_DELAY_SAMPLES ||
	    pre_samples >= PCM_ECHO_REFERENCE_MAX_WINDOW_SAMPLES)
		return PCM_ECHO_REFERENCE_INVALID_ARGUMENT;
	/* Scan every sample delay from the front of the ring. Adjacent-delay
	 * averaging can cancel a QAM echo's correlation. */
	stop = current_window_delay_samples + PCM_ECHO_REFERENCE_SCAN_SPAN;
	if (stop > PCM_ECHO_REFERENCE_MAX_WINDOW_DELAY_SAMPLES)
		stop = PCM_ECHO_REFERENCE_MAX_WINDOW_DELAY_SAMPLES;
	/* A short initial hypothesis must not narrow away the documented
	 * returned-echo delay.  The scan identifies the first sample of a
	 * window, so account for its known reference point here. */
	required_stop = PCM_ECHO_REFERENCE_REQUIRED_DELAY_SAMPLES > pre_samples ?
		PCM_ECHO_REFERENCE_REQUIRED_DELAY_SAMPLES - pre_samples : 1u;
	if (stop < required_stop)
		stop = required_stop;
	clear_scan(reference);
	reference->scan_count = stop;
	reference->scan_current_index = current_window_delay_samples - 1u;
	reference->scan_pre_samples = pre_samples;
	return PCM_ECHO_REFERENCE_OK;
}

/* Signed moments use two's-complement wide arithmetic. With at most
 * UINT32_MAX signed-sample observations, each centered moment fits 96 bits.
 * Keep centering exact before conversion: on targets with a binary64 long
 * double, subtracting rounded large energies can invent variance in silence. */
static struct wide_uint signed_moment(int64_t value)
{
	return (struct wide_uint){ value < 0 ? UINT64_MAX : 0, (uint64_t)value };
}

static long double moment_value(struct wide_uint value)
{
	return (long double)value.high * 18446744073709551616.0L + value.low;
}

static long double correlation_score(
	uint64_t reference_energy, uint64_t input_energy, int64_t cross_energy,
	int64_t reference_sum, int64_t input_sum, uint32_t samples)
{
	struct wide_uint count = wide_from_u64(samples);
	struct wide_uint reference = wide_multiply(wide_from_u64(reference_energy), count);
	struct wide_uint input = wide_multiply(wide_from_u64(input_energy), count);
	struct wide_uint reference_mean = wide_product_u64(
		magnitude_s64(reference_sum), magnitude_s64(reference_sum));
	struct wide_uint input_mean = wide_product_u64(
		magnitude_s64(input_sum), magnitude_s64(input_sum));
	uint64_t sum_bound = (uint64_t)samples * 32768u;

	if (!samples || magnitude_s64(reference_sum) > sum_bound ||
	    magnitude_s64(input_sum) > sum_bound ||
	    wide_compare(reference, reference_mean) <= 0 ||
	    wide_compare(input, input_mean) <= 0)
		return -1.0L;
	reference = wide_subtract(reference, reference_mean);
	input = wide_subtract(input, input_mean);
	struct wide_uint cross = wide_subtract(
		wide_multiply(signed_moment(cross_energy), count),
		wide_multiply(signed_moment(reference_sum), signed_moment(input_sum)));
	if (cross.high >> 63)
		cross = wide_subtract(wide_from_u64(0), cross);
	long double value = moment_value(cross);

	return value * value / (moment_value(reference) * moment_value(input));
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
			reference->scan_cross_energy[index],
			reference->scan_reference_sum[index],
			reference->scan_input_sum, reference->scan_samples);

		if (index == reference->scan_current_index)
			*current_score = score;
		if (score >= 0.0L && (!found || score > *best_score)) {
			found = true;
			*best_index = index;
			*best_score = score;
		}
	}

	if (found && *best_score > 0.0L) {
		/* A multipath return need not put its largest correlation first.
		 * Position the filter to cover the strongest cluster around that
		 * peak, rather than cutting off comparable earlier returns.
		 * Ignore bins below one quarter of the peak's squared correlation
		 * to keep weak scan-noise bins from dominating the placement.
		 * This is acquisition evidence only, not cancellation readiness. */
		uint32_t bins = PCM_ECHO_REFERENCE_MAX_WINDOW_SAMPLES -
			reference->scan_pre_samples;
		uint32_t peak = *best_index;
		uint32_t first = peak >= bins ? peak - bins + 1u : 0u;
		long double mass = 0.0L, best_mass = -1.0L;
		for (uint32_t index = first; index < peak + bins; ++index) {
			long double score = index < reference->scan_count ? correlation_score(
				reference->scan_reference_energy[index],
				reference->scan_input_energy,
				reference->scan_cross_energy[index],
				reference->scan_reference_sum[index],
				reference->scan_input_sum, reference->scan_samples) : -1.0L;
			if (score >= *best_score / 4.0L)
				mass += score;
			if (index >= first + bins) {
				uint32_t expired = index - bins;
				long double removed = correlation_score(
					reference->scan_reference_energy[expired],
					reference->scan_input_energy,
					reference->scan_cross_energy[expired],
					reference->scan_reference_sum[expired],
					reference->scan_input_sum, reference->scan_samples);
				if (removed >= *best_score / 4.0L)
					mass -= removed;
			}
			/* Ties favor the original peak placement, preserving single
			 * returns and avoiding unnecessary shifts into earlier noise. */
			if (index + 1u >= first + bins && mass >= best_mass) {
				best_mass = mass;
				*best_index = index + 1u - bins;
			}
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
	if (reference->fill < reference->scan_count + reference->scan_pre_samples)
		return PCM_ECHO_REFERENCE_NOT_READY;

	/* The bounded observation count also bounds first moments to 47 bits.
	 * A saturated scan cannot report another window; retain its evidence. */
	if (reference->scan_samples == UINT32_MAX)
		return PCM_ECHO_REFERENCE_OK;
	reference->scan_input_sum = wrap_s64_from_u64(
		(uint64_t)reference->scan_input_sum + (uint64_t)(int64_t)received_sample);
	input_energy = square_s16(received_sample);
	reference->scan_input_energy = add_u64_saturating(
		reference->scan_input_energy, input_energy);
	for (uint32_t index = 0; index < reference->scan_count; ++index) {
		uint32_t age = index + 1u + reference->scan_pre_samples;
		int16_t sample = sample_at_age(reference, age);
		int64_t cross;
		cross = (int64_t)sample * received_sample;
		/* Valid first moments cannot overflow their 47-bit bound. Keep
		 * corrupt state defined too; scoring rejects out-of-bound sums. */
		reference->scan_reference_sum[index] = wrap_s64_from_u64(
			(uint64_t)reference->scan_reference_sum[index] + (uint64_t)(int64_t)sample);

		reference->scan_reference_energy[index] = add_u64_saturating(
			reference->scan_reference_energy[index],
			square_s16(sample));
		reference->scan_cross_energy[index] = add_s64_saturating(
			reference->scan_cross_energy[index], cross);
	}
	reference->scan_samples++;
	if (!scan_report_is_due(reference) ||
	    !find_best_scan(reference, &best_index, &best_score,
			    &current_score))
		return PCM_ECHO_REFERENCE_OK;

	generated.best_window_delay_samples = best_index + 1u;
	generated.current_window_delay_samples = reference->scan_current_index + 1u;
	generated.sample_count = reference->scan_samples;
	generated.best_correlation_ppm = correlation_ppm(best_score);
	generated.current_correlation_ppm =
		correlation_ppm(current_score);
	reference->scan_epoch = reference->scan_samples;
	*result = generated;
	return PCM_ECHO_REFERENCE_SCAN_READY;
}
