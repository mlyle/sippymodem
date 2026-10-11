// SPDX-License-Identifier: BSD-3-Clause

#include "line_receiver.h"
#include "memory_region.h"
#include "sample_math.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

static const struct agc_config V32_RECEIVE_GAIN = {
	.target_level_q15 = 0x2365,
	/* Do not amplify one quiet-line noise block into timing acquisition
	 * before the tracking floor suppresses the following blocks. */
	.acquisition_threshold = 0x0050,
	.tracking_threshold = 0x0050,
	.block_samples = V32_LINE_RECEIVER_EPOCH_SAMPLES,
	.history_weight_q15 = 0x4000,
	.input_weight_q15 = 0x4000,
};

bool v32_line_receiver_is_valid(
	const struct v32_line_receiver *receiver)
{
	return receiver != NULL && agc_is_valid(&receiver->gain) &&
	       v32_symbol_timing_is_valid(&receiver->timing) &&
	       receiver->epoch_fill < V32_LINE_RECEIVER_EPOCH_SAMPLES &&
	       (!receiver->input_paused || receiver->epoch_fill == 0);
}

enum v32_line_receiver_result v32_line_receiver_init(
	struct v32_line_receiver *receiver)
{
	struct v32_line_receiver initialized;

	if (receiver == NULL)
		return V32_LINE_RECEIVER_INVALID_ARGUMENT;
	memset(&initialized, 0, sizeof(initialized));
	if (agc_init(&initialized.gain, &V32_RECEIVE_GAIN) != AGC_OK ||
	    v32_symbol_timing_init(&initialized.timing) !=
		    V32_SYMBOL_TIMING_OK ||
	    !v32_line_receiver_is_valid(&initialized))
		return V32_LINE_RECEIVER_INVALID_STATE;
	*receiver = initialized;
	return V32_LINE_RECEIVER_OK;
}

enum v32_line_receiver_result v32_line_receiver_freeze_gain(
	struct v32_line_receiver *receiver)
{
	if (!v32_line_receiver_is_valid(receiver))
		return V32_LINE_RECEIVER_INVALID_STATE;
	if (receiver->epoch_fill != 0)
		return V32_LINE_RECEIVER_INVALID_STATE;
	if (agc_freeze(&receiver->gain) != AGC_OK)
		return V32_LINE_RECEIVER_INVALID_STATE;
	return V32_LINE_RECEIVER_OK;
}

enum v32_line_receiver_result v32_line_receiver_reacquire_gain(
	struct v32_line_receiver *receiver)
{
	struct v32_line_receiver next;

	if (!v32_line_receiver_is_valid(receiver))
		return V32_LINE_RECEIVER_INVALID_STATE;
	if (receiver->epoch_fill != 0)
		return V32_LINE_RECEIVER_INVALID_STATE;
	next = *receiver;
	memset(next.epoch, 0, sizeof(next.epoch));
	next.epoch_fill = 0;
	if (agc_reacquire(&next.gain) != AGC_OK ||
	    !v32_line_receiver_is_valid(&next))
		return V32_LINE_RECEIVER_INVALID_STATE;
	*receiver = next;
	return V32_LINE_RECEIVER_OK;
}

enum v32_line_receiver_result v32_line_receiver_reacquire_timing(
	struct v32_line_receiver *receiver)
{
	struct v32_line_receiver next;

	if (!v32_line_receiver_is_valid(receiver))
		return V32_LINE_RECEIVER_INVALID_STATE;
	if (receiver->epoch_fill != 0)
		return V32_LINE_RECEIVER_INVALID_STATE;
	next = *receiver;
	if (v32_symbol_timing_init(&next.timing) != V32_SYMBOL_TIMING_OK ||
	    !v32_line_receiver_is_valid(&next))
		return V32_LINE_RECEIVER_INVALID_STATE;
	*receiver = next;
	return V32_LINE_RECEIVER_OK;
}

enum v32_line_receiver_result v32_line_receiver_set_timing_tracking(
	struct v32_line_receiver *receiver, bool enabled)
{
	struct v32_line_receiver next;

	if (!v32_line_receiver_is_valid(receiver))
		return V32_LINE_RECEIVER_INVALID_STATE;
	if (receiver->epoch_fill != 0 || receiver->input_paused)
		return V32_LINE_RECEIVER_INVALID_STATE;
	next = *receiver;
	if (v32_symbol_timing_set_tracking(&next.timing, enabled) !=
		    V32_SYMBOL_TIMING_OK ||
	    !v32_line_receiver_is_valid(&next))
		return V32_LINE_RECEIVER_INVALID_STATE;
	*receiver = next;
	return V32_LINE_RECEIVER_OK;
}

static bool process_arguments_are_valid(
	const struct v32_line_receiver *receiver,
	const double *cleaned, size_t count,
	int16_t *recovered, size_t recovered_capacity,
	size_t *recovered_count, bool completes_epoch)
{
	size_t input_bytes;
	size_t output_bytes = completes_epoch ?
		V32_SYMBOL_TIMING_MAX_OUTPUT_SAMPLES * sizeof(*recovered) : 0;

	if ((cleaned == NULL && count != 0) || recovered_count == NULL)
		return false;
	if (completes_epoch &&
	    (recovered == NULL ||
	     recovered_capacity < V32_SYMBOL_TIMING_MAX_OUTPUT_SAMPLES))
		return false;
	input_bytes = count * sizeof(*cleaned);
	return !memory_regions_overlap(cleaned, input_bytes,
				       receiver, sizeof(*receiver)) &&
	       !memory_regions_overlap(recovered, output_bytes,
				       receiver, sizeof(*receiver)) &&
	       !memory_regions_overlap(
		       recovered_count, sizeof(*recovered_count),
			       receiver, sizeof(*receiver)) &&
	       !memory_regions_overlap(cleaned, input_bytes,
				       recovered, output_bytes) &&
	       !memory_regions_overlap(cleaned, input_bytes,
				       recovered_count, sizeof(*recovered_count)) &&
	       !memory_regions_overlap(recovered, output_bytes,
				       recovered_count, sizeof(*recovered_count));
}

enum v32_line_receiver_result v32_line_receiver_process(
	struct v32_line_receiver *receiver,
	const double *cleaned, size_t count, bool input_paused,
	int16_t *recovered, size_t recovered_capacity,
	size_t *recovered_count)
{
	struct v32_line_receiver next;
	int16_t normalized[V32_LINE_RECEIVER_EPOCH_SAMPLES];
	int16_t pending[V32_SYMBOL_TIMING_MAX_OUTPUT_SAMPLES];
	size_t produced = 0;
	size_t initial_fill;
	bool completes_epoch;

	if (!v32_line_receiver_is_valid(receiver))
		return V32_LINE_RECEIVER_INVALID_STATE;
	if (count > V32_LINE_RECEIVER_EPOCH_SAMPLES)
		return V32_LINE_RECEIVER_INVALID_ARGUMENT;
	initial_fill = receiver->input_paused || input_paused ?
		0 : receiver->epoch_fill;
	completes_epoch = !input_paused &&
		initial_fill + count >= V32_LINE_RECEIVER_EPOCH_SAMPLES;
	if (!process_arguments_are_valid(
		    receiver, cleaned, count, recovered, recovered_capacity,
		    recovered_count, completes_epoch))
		return V32_LINE_RECEIVER_INVALID_ARGUMENT;
	for (size_t index = 0; index < count; ++index) {
		if (!isfinite(cleaned[index]))
			return V32_LINE_RECEIVER_INVALID_ARGUMENT;
	}

	next = *receiver;
	if (input_paused) {
		memset(next.epoch, 0, sizeof(next.epoch));
		next.epoch_fill = 0;
		next.input_paused = true;
		*receiver = next;
		*recovered_count = 0;
		return V32_LINE_RECEIVER_OK;
	}
	if (next.input_paused) {
		memset(next.epoch, 0, sizeof(next.epoch));
		next.epoch_fill = 0;
		next.input_paused = false;
	}

	for (size_t index = 0; index < count; ++index) {
		next.epoch[next.epoch_fill++] = sample_round_s16(cleaned[index]);
		if (next.epoch_fill == V32_LINE_RECEIVER_EPOCH_SAMPLES) {
			enum agc_result result = agc_process(
				&next.gain, next.epoch,
				V32_LINE_RECEIVER_EPOCH_SAMPLES,
				normalized);

			if (result != AGC_OK)
				return V32_LINE_RECEIVER_INVALID_STATE;
			if (v32_symbol_timing_process(
				    &next.timing, normalized,
				    V32_LINE_RECEIVER_EPOCH_SAMPLES,
				    pending,
				    V32_SYMBOL_TIMING_MAX_OUTPUT_SAMPLES,
				    &produced) !=
				    V32_SYMBOL_TIMING_OK)
				return V32_LINE_RECEIVER_INVALID_STATE;
			if (!next.timing.signal_detected &&
			    next.timing.mode == V32_SYMBOL_TIMING_ACQUISITION)
				produced = 0;
			memset(next.epoch, 0, sizeof(next.epoch));
			next.epoch_fill = 0;
		}
	}
	if (!v32_line_receiver_is_valid(&next))
		return V32_LINE_RECEIVER_INVALID_STATE;
	if (produced > V32_SYMBOL_TIMING_MAX_OUTPUT_SAMPLES)
		return V32_LINE_RECEIVER_INVALID_ARGUMENT;
	if (produced != 0)
		memcpy(recovered, pending, produced * sizeof(*recovered));
	*receiver = next;
	*recovered_count = produced;
	return V32_LINE_RECEIVER_OK;
}
