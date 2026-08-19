// SPDX-License-Identifier: BSD-3-Clause

#include "line_receiver.h"
#include "memory_region.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

static const struct agc_config V32_RECEIVE_GAIN = {
	.target_level_q15 = 0x2365,
	.acquisition_threshold = 0x0002,
	.tracking_threshold = 0x0050,
	.block_samples = V32_LINE_RECEIVER_EPOCH_SAMPLES,
	.history_weight_q15 = 0x4000,
	.input_weight_q15 = 0x4000,
};

static int16_t quantize_sample(double sample)
{
	if (sample >= 32766.5)
		return INT16_MAX;
	if (sample <= -32767.5)
		return INT16_MIN;
	return (int16_t)(int32_t)(sample >= 0.0 ? sample + 0.5 :
						      sample - 0.5);
}

bool v32_line_receiver_is_valid(
	const struct v32_line_receiver *receiver)
{
	return receiver != NULL && agc_is_valid(&receiver->gain) &&
	       receiver->epoch_fill < V32_LINE_RECEIVER_EPOCH_SAMPLES &&
	       (!receiver->echo_adapting || receiver->epoch_fill == 0);
}

enum v32_line_receiver_result v32_line_receiver_init(
	struct v32_line_receiver *receiver)
{
	struct v32_line_receiver initialized;

	if (receiver == NULL)
		return V32_LINE_RECEIVER_INVALID_ARGUMENT;
	memset(&initialized, 0, sizeof(initialized));
	if (agc_init(&initialized.gain, &V32_RECEIVE_GAIN) != AGC_OK ||
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

static bool process_arguments_are_valid(
	const struct v32_line_receiver *receiver,
	const double *cleaned, size_t count,
	int16_t *normalized, size_t produced,
	size_t *normalized_count)
{
	size_t input_bytes;
	size_t output_bytes;

	if ((cleaned == NULL && count != 0) || normalized_count == NULL ||
	    (normalized == NULL && produced != 0))
		return false;
	input_bytes = count * sizeof(*cleaned);
	output_bytes = produced * sizeof(*normalized);
	return !memory_regions_overlap(cleaned, input_bytes,
			       receiver, sizeof(*receiver)) &&
	       !memory_regions_overlap(normalized, output_bytes,
			       receiver, sizeof(*receiver)) &&
	       !memory_regions_overlap(
		       normalized_count, sizeof(*normalized_count),
			       receiver, sizeof(*receiver)) &&
	       !memory_regions_overlap(cleaned, input_bytes,
			       normalized, output_bytes) &&
	       !memory_regions_overlap(cleaned, input_bytes,
			       normalized_count, sizeof(*normalized_count)) &&
	       !memory_regions_overlap(normalized, output_bytes,
			       normalized_count, sizeof(*normalized_count));
}

enum v32_line_receiver_result v32_line_receiver_process(
	struct v32_line_receiver *receiver,
	const double *cleaned, size_t count, bool echo_adapting,
	int16_t *normalized, size_t normalized_capacity,
	size_t *normalized_count)
{
	struct v32_line_receiver next;
	size_t initial_fill;
	size_t produced;

	if (!v32_line_receiver_is_valid(receiver))
		return V32_LINE_RECEIVER_INVALID_STATE;
	if (count > V32_LINE_RECEIVER_EPOCH_SAMPLES)
		return V32_LINE_RECEIVER_INVALID_ARGUMENT;
	initial_fill = receiver->echo_adapting || echo_adapting ?
			       0 : receiver->epoch_fill;
	produced = !echo_adapting &&
		   initial_fill + count >= V32_LINE_RECEIVER_EPOCH_SAMPLES ?
			   V32_LINE_RECEIVER_EPOCH_SAMPLES : 0;
	if (produced > normalized_capacity ||
	    !process_arguments_are_valid(
		    receiver, cleaned, count, normalized, produced,
		    normalized_count))
		return V32_LINE_RECEIVER_INVALID_ARGUMENT;
	for (size_t index = 0; index < count; ++index) {
		if (!isfinite(cleaned[index]))
			return V32_LINE_RECEIVER_INVALID_ARGUMENT;
	}

	next = *receiver;
	if (echo_adapting) {
		memset(next.epoch, 0, sizeof(next.epoch));
		next.epoch_fill = 0;
		next.echo_adapting = true;
		*receiver = next;
		*normalized_count = 0;
		return V32_LINE_RECEIVER_OK;
	}
	if (next.echo_adapting) {
		memset(next.epoch, 0, sizeof(next.epoch));
		next.epoch_fill = 0;
		next.echo_adapting = false;
	}

	produced = 0;
	for (size_t index = 0; index < count; ++index) {
		next.epoch[next.epoch_fill++] = quantize_sample(cleaned[index]);
		if (next.epoch_fill == V32_LINE_RECEIVER_EPOCH_SAMPLES) {
			enum agc_result result = agc_process(
				&next.gain, next.epoch,
				V32_LINE_RECEIVER_EPOCH_SAMPLES,
				&normalized[produced]);

			if (result != AGC_OK)
				return V32_LINE_RECEIVER_INVALID_STATE;
			produced += V32_LINE_RECEIVER_EPOCH_SAMPLES;
			memset(next.epoch, 0, sizeof(next.epoch));
			next.epoch_fill = 0;
		}
	}
	if (!v32_line_receiver_is_valid(&next))
		return V32_LINE_RECEIVER_INVALID_STATE;
	*receiver = next;
	*normalized_count = produced;
	return V32_LINE_RECEIVER_OK;
}

const char *v32_line_receiver_result_name(
	enum v32_line_receiver_result result)
{
	switch (result) {
	case V32_LINE_RECEIVER_OK:
		return "ok";
	case V32_LINE_RECEIVER_INVALID_ARGUMENT:
		return "invalid argument";
	case V32_LINE_RECEIVER_INVALID_STATE:
		return "invalid state";
	}
	return "unknown V.32 line receiver result";
}
