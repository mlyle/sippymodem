// SPDX-License-Identifier: BSD-3-Clause

#include "binary_equalizer.h"
#include "memory_region.h"
#include "qam_equalizer_undo.h"

#include <math.h>

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

static enum binary_equalizer_result receive(
	struct binary_equalizer *receiver, int32_t sample, bool received_available,
	uint8_t *sign, bool owned)
{
	struct qam_equalizer *equalizer;
	struct qam_equalizer_push_undo undo;
	struct qam_point output;
	struct qam_point input;
	struct qam_point target;
	uint8_t decision;
	bool ready;

	if (receiver == NULL || sign == NULL || memory_regions_overlap(
		    receiver, sizeof(*receiver), sign, sizeof(*sign)))
		return BINARY_EQUALIZER_INVALID_ARGUMENT;
	if (!owned && !binary_equalizer_is_valid(receiver))
		return BINARY_EQUALIZER_INVALID_STATE;
	equalizer = &receiver->equalizer;
	qam_equalizer_save_push(equalizer, &undo);
	input = (struct qam_point){
		.in_phase = receiver->polarity > 0 ? sample : -(double)sample,
	};
	enum qam_equalizer_result pushed = received_available ?
		qam_equalizer_push_owned(equalizer, &input, &output, &ready) :
		qam_equalizer_push_erasure_owned(equalizer, &input, &output, &ready);
	if (pushed != QAM_EQUALIZER_OK)
		return BINARY_EQUALIZER_INTERNAL_ERROR;
	if (!ready)
		goto undo_push;
	decision = output.in_phase < 0.0 ? 1u : 0u;
	target = (struct qam_point){
		.in_phase = decision != 0u ? -receiver->magnitude : receiver->magnitude,
	};
	if (qam_equalizer_adapt(equalizer, &target, receiver->adaptation) !=
	    QAM_EQUALIZER_OK)
		goto undo_push;
	*sign = decision;
	return BINARY_EQUALIZER_OK;

undo_push:
	qam_equalizer_undo_push(equalizer, &undo);
	return BINARY_EQUALIZER_INTERNAL_ERROR;
}

enum binary_equalizer_result binary_equalizer_receive(
	struct binary_equalizer *receiver, int32_t sample, bool received_available,
	uint8_t *sign)
{
	return receive(receiver, sample, received_available, sign, false);
}

enum binary_equalizer_result binary_equalizer_receive_owned(
	struct binary_equalizer *receiver, int32_t sample, bool received_available,
	uint8_t *sign)
{
	return receive(receiver, sample, received_available, sign, true);
}
