// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2_probe_receiver.h"

#include <stddef.h>

_Static_assert(
	SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES %
		SIPPYMODEM_V34_PHASE2_PROBE_PERIOD_SAMPLES ==
		0,
	"V.34 probe measurement must span whole waveform periods");
_Static_assert(
	SIPPYMODEM_V34_PHASE2_PROBE_L2_RECEIVE_LIMIT_SAMPLES >
		SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES,
	"V.34 L2 limit must allow a complete measurement window");

bool sippymodem_v34_phase2_probe_receiver_is_valid(
	const struct sippymodem_v34_phase2_probe_receiver *receiver)
{
	uint32_t l1_samples;
	uint32_t l2_measured_samples;
	uint32_t l2_limit;

	if (receiver == NULL ||
	    !sippymodem_v34_phase2_probe_spectrum_is_valid(
		    &receiver->l1_spectrum) ||
	    !sippymodem_v34_phase2_probe_spectrum_is_valid(
		    &receiver->l2_spectrum) ||
	    receiver->l1_spectrum.window_start_sample != 0 ||
	    receiver->l2_spectrum.window_start_sample != 0 ||
	    (unsigned int)receiver->phase >
		    SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_LIMIT_REACHED)
		return false;
	l1_samples = receiver->l1_spectrum.samples_observed;
	l2_measured_samples = receiver->l2_spectrum.samples_observed;
	l2_limit = SIPPYMODEM_V34_PHASE2_PROBE_L2_RECEIVE_LIMIT_SAMPLES;
	if (l1_samples > SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES ||
	    l2_measured_samples > SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES ||
	    receiver->l2_samples < l2_measured_samples ||
	    receiver->l2_samples > l2_limit)
		return false;

	switch (receiver->phase) {
	case SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_MEASURE_L1:
		return l1_samples < SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES &&
		       l2_measured_samples == 0 && receiver->l2_samples == 0;
	case SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_MEASURE_L2:
		return l1_samples == SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES &&
		       l2_measured_samples <
			       SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES &&
		       receiver->l2_samples == l2_measured_samples;
	case SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_WAIT_L2_END:
	case SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_COMPLETE:
		return l1_samples == SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES &&
		       l2_measured_samples ==
			       SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES &&
		       receiver->l2_samples >=
			       SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES &&
		       receiver->l2_samples < l2_limit;
	case SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_LIMIT_REACHED:
		return l1_samples == SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES &&
		       l2_measured_samples ==
			       SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES &&
		       receiver->l2_samples == l2_limit;
	}
	return false;
}

enum sippymodem_v34_phase2_probe_receiver_result
sippymodem_v34_phase2_probe_receiver_init(
	struct sippymodem_v34_phase2_probe_receiver *receiver)
{
	struct sippymodem_v34_phase2_probe_receiver initialized = {
		.phase = SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_MEASURE_L1,
	};

	if (receiver == NULL ||
	    sippymodem_v34_phase2_probe_spectrum_init(
		    &initialized.l1_spectrum) !=
		    SIPPYMODEM_V34_PHASE2_PROBE_SPECTRUM_OK ||
	    sippymodem_v34_phase2_probe_spectrum_init(
		    &initialized.l2_spectrum) !=
		    SIPPYMODEM_V34_PHASE2_PROBE_SPECTRUM_OK ||
	    !sippymodem_v34_phase2_probe_receiver_is_valid(&initialized))
		return SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_OK;
}

static enum sippymodem_v34_phase2_probe_receiver_result measure_l1_sample(
	struct sippymodem_v34_phase2_probe_receiver *receiver,
	int16_t received_sample,
	struct sippymodem_v34_phase2_probe_receiver_event *event)
{
	if (sippymodem_v34_phase2_probe_spectrum_sample(
		    &receiver->l1_spectrum, received_sample) !=
	    SIPPYMODEM_V34_PHASE2_PROBE_SPECTRUM_OK)
		return SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_INVALID_ARGUMENT;
	if (receiver->l1_spectrum.samples_observed ==
	    SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES) {
		receiver->phase =
			SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_MEASURE_L2;
		event->kind =
			SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_EVENT_L2_STARTED;
	}
	return SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_OK;
}

static enum sippymodem_v34_phase2_probe_receiver_result measure_l2_sample(
	struct sippymodem_v34_phase2_probe_receiver *receiver,
	int16_t received_sample,
	struct sippymodem_v34_phase2_probe_receiver_event *event)
{
	if (sippymodem_v34_phase2_probe_spectrum_sample(
		    &receiver->l2_spectrum, received_sample) !=
	    SIPPYMODEM_V34_PHASE2_PROBE_SPECTRUM_OK)
		return SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_INVALID_ARGUMENT;
	receiver->l2_samples++;
	if (receiver->l2_samples ==
	    SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES) {
		receiver->phase =
			SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_WAIT_L2_END;
		event->kind =
			SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_EVENT_MEASUREMENT_READY;
		event->l2_samples = receiver->l2_samples;
	}
	return SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_OK;
}

static void wait_for_l2_end(
	struct sippymodem_v34_phase2_probe_receiver *receiver,
	bool finish_l2,
	struct sippymodem_v34_phase2_probe_receiver_event *event)
{
	if (finish_l2) {
		receiver->phase = SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_COMPLETE;
		event->kind =
			SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_EVENT_COMPLETED;
		event->l2_samples = receiver->l2_samples;
	} else if (++receiver->l2_samples ==
		   SIPPYMODEM_V34_PHASE2_PROBE_L2_RECEIVE_LIMIT_SAMPLES) {
		receiver->phase =
			SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_LIMIT_REACHED;
		event->kind =
			SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_EVENT_LIMIT_REACHED;
		event->l2_samples = receiver->l2_samples;
	}
}

enum sippymodem_v34_phase2_probe_receiver_result
sippymodem_v34_phase2_probe_receiver_sample(
	struct sippymodem_v34_phase2_probe_receiver *receiver,
	int16_t received_sample, bool finish_l2,
	struct sippymodem_v34_phase2_probe_receiver_event *event)
{
	struct sippymodem_v34_phase2_probe_receiver next;
	struct sippymodem_v34_phase2_probe_receiver_event emitted = { 0 };
	enum sippymodem_v34_phase2_probe_receiver_result result =
		SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_OK;

	if (!sippymodem_v34_phase2_probe_receiver_is_valid(receiver) ||
	    event == NULL)
		return SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_INVALID_ARGUMENT;
	next = *receiver;
	switch (next.phase) {
	case SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_MEASURE_L1:
		if (finish_l2)
			return SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_INVALID_ARGUMENT;
		result = measure_l1_sample(&next, received_sample, &emitted);
		break;
	case SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_MEASURE_L2:
		if (finish_l2)
			return SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_INVALID_ARGUMENT;
		result = measure_l2_sample(&next, received_sample, &emitted);
		break;
	case SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_WAIT_L2_END:
		wait_for_l2_end(&next, finish_l2, &emitted);
		break;
	case SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_COMPLETE:
	case SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_LIMIT_REACHED:
		break;
	}
	if (result != SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_OK ||
	    !sippymodem_v34_phase2_probe_receiver_is_valid(&next))
		return SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
	*event = emitted;
	return SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_OK;
}

enum sippymodem_v34_phase2_probe_receiver_result
sippymodem_v34_phase2_probe_receiver_measure(
	const struct sippymodem_v34_phase2_probe_receiver *receiver,
	struct sippymodem_v34_phase2_probe_receiver_measurement *measurement)
{
	struct sippymodem_v34_phase2_probe_receiver_measurement measured = { 0 };

	if (!sippymodem_v34_phase2_probe_receiver_is_valid(receiver) ||
	    measurement == NULL ||
	    receiver->phase ==
		    SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_MEASURE_L1 ||
	    receiver->phase ==
		    SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_MEASURE_L2 ||
	    sippymodem_v34_phase2_probe_spectrum_measure(
		    &receiver->l1_spectrum, &measured.l1) !=
		    SIPPYMODEM_V34_PHASE2_PROBE_SPECTRUM_OK ||
	    sippymodem_v34_phase2_probe_spectrum_measure(
		    &receiver->l2_spectrum, &measured.l2) !=
		    SIPPYMODEM_V34_PHASE2_PROBE_SPECTRUM_OK)
		return SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_INVALID_ARGUMENT;
	measured.l2_samples = receiver->l2_samples;
	*measurement = measured;
	return SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_OK;
}

const char *sippymodem_v34_phase2_probe_receiver_result_name(
	enum sippymodem_v34_phase2_probe_receiver_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_OK:
		return "ok";
	case SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
