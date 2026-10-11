// SPDX-License-Identifier: BSD-3-Clause

#include "binary_equalizer.h"
#include "memory_region.h"

#include <math.h>
#include <stddef.h>

bool binary_equalizer_is_valid(
	const struct binary_equalizer *receiver)
{
	return receiver != NULL &&
	       qam_equalizer_is_handoff_ready(&receiver->equalizer) &&
	       isfinite(receiver->magnitude) && receiver->magnitude > 0.0 &&
	       receiver->magnitude <= 32768.0 &&
	       qam_adaptation_is_valid(receiver->adaptation) &&
	       (receiver->polarity == -1 || receiver->polarity == 1);
}

enum binary_equalizer_result binary_equalizer_init(
	struct binary_equalizer *receiver,
	const struct qam_equalizer *trained_equalizer,
	double magnitude, double adaptation, int8_t polarity)
{
	struct binary_equalizer initialized;

	if (receiver == NULL || trained_equalizer == NULL)
		return BINARY_EQUALIZER_INVALID_ARGUMENT;
	initialized = (struct binary_equalizer){
		.equalizer = *trained_equalizer,
		.magnitude = magnitude,
		.adaptation = adaptation,
		.polarity = polarity,
	};
	if (!binary_equalizer_is_valid(&initialized))
		return BINARY_EQUALIZER_INVALID_ARGUMENT;
	*receiver = initialized;
	return BINARY_EQUALIZER_OK;
}

enum binary_equalizer_result binary_equalizer_receive(
	struct binary_equalizer *receiver, int32_t sample,
	uint8_t *sign)
{
	struct binary_equalizer next;
	struct qam_point output;
	struct qam_point input;
	struct qam_point target;
	uint8_t decision;
	bool ready;

	if (receiver == NULL || sign == NULL || memory_regions_overlap(
		    receiver, sizeof(*receiver), sign, sizeof(*sign)))
		return BINARY_EQUALIZER_INVALID_ARGUMENT;
	if (!binary_equalizer_is_valid(receiver))
		return BINARY_EQUALIZER_INVALID_STATE;
	next = *receiver;
	input = (struct qam_point){
		.in_phase = next.polarity > 0 ? sample : -(double)sample,
	};
	if (qam_equalizer_push(
		    &next.equalizer, &input, &output, &ready) !=
		    QAM_EQUALIZER_OK ||
	    !ready)
		return BINARY_EQUALIZER_INTERNAL_ERROR;
	decision = output.in_phase < 0.0 ? 1u : 0u;
	target = (struct qam_point){
		.in_phase = decision != 0u ? -next.magnitude : next.magnitude,
	};
	if (qam_equalizer_adapt(
		    &next.equalizer, &target,
		    next.adaptation) != QAM_EQUALIZER_OK)
		return BINARY_EQUALIZER_INTERNAL_ERROR;
	*receiver = next;
	*sign = decision;
	return BINARY_EQUALIZER_OK;
}
