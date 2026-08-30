// SPDX-License-Identifier: BSD-3-Clause

#include "binary_equalizer.h"

#include <math.h>
#include <stddef.h>

bool binary_equalizer_is_valid(
	const struct binary_equalizer *receiver)
{
	return receiver != NULL &&
	       qam_equalizer_is_valid(&receiver->equalizer) &&
	       receiver->equalizer.fill ==
		       receiver->equalizer.decision_delay + 1u &&
	       !receiver->equalizer.can_adapt &&
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
	struct binary_equalizer *receiver, int16_t sample,
	uint8_t *sign)
{
	struct binary_equalizer next;
	struct qam_equalizer_observation observation = { 0 };
	struct qam_point input;
	struct qam_point target;
	uint8_t decision;
	bool ready;

	if (receiver == NULL || sign == NULL)
		return BINARY_EQUALIZER_INVALID_ARGUMENT;
	if (!binary_equalizer_is_valid(receiver))
		return BINARY_EQUALIZER_INVALID_STATE;
	next = *receiver;
	input = (struct qam_point){
		.in_phase = next.polarity > 0 ? sample : -(double)sample,
	};
	if (qam_equalizer_observe(
		    &next.equalizer, &input, &observation, &ready) !=
		    QAM_EQUALIZER_OK ||
	    !ready)
		return BINARY_EQUALIZER_INTERNAL_ERROR;
	decision = observation.output.in_phase < 0.0 ? 1u : 0u;
	target = (struct qam_point){
		.in_phase = decision != 0u ? -next.magnitude : next.magnitude,
	};
	if (qam_equalizer_adapt_observation(
		    &next.equalizer, &observation, &target,
		    next.adaptation) != QAM_EQUALIZER_OK ||
	    !binary_equalizer_is_valid(&next))
		return BINARY_EQUALIZER_INTERNAL_ERROR;
	*receiver = next;
	*sign = decision;
	return BINARY_EQUALIZER_OK;
}

const char *binary_equalizer_result_name(
	enum binary_equalizer_result result)
{
	switch (result) {
	case BINARY_EQUALIZER_OK:
		return "ok";
	case BINARY_EQUALIZER_INVALID_ARGUMENT:
		return "invalid argument";
	case BINARY_EQUALIZER_INVALID_STATE:
		return "invalid state";
	case BINARY_EQUALIZER_INTERNAL_ERROR:
		return "internal error";
	}
	return "unknown";
}
