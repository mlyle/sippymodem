// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2_probe_receiver.h"

#include <math.h>
#include <stddef.h>

_Static_assert(
	V34_PHASE2_PROBE_L1_SAMPLES %
		V34_PHASE2_PROBE_PERIOD_SAMPLES ==
		0,
	"V.34 probe measurement must span whole waveform periods");
_Static_assert(
	V34_PHASE2_PROBE_L2_RECEIVE_LIMIT_SAMPLES >
		V34_PHASE2_PROBE_L1_SAMPLES,
	"V.34 L2 limit must allow a complete measurement window");

static bool l2_window_is_valid(uint32_t samples)
{
	return samples >= V34_PHASE2_PROBE_PERIOD_SAMPLES &&
	       samples <= V34_PHASE2_PROBE_L1_SAMPLES &&
	       samples % V34_PHASE2_PROBE_PERIOD_SAMPLES == 0;
}

bool v34_phase2_probe_measurement_is_valid(
	const struct v34_phase2_probe_receiver_measurement *measurement)
{
	if (measurement == NULL ||
	    measurement->l1.sample_count != V34_PHASE2_PROBE_L1_SAMPLES ||
	    !l2_window_is_valid(measurement->l2.sample_count) ||
	    measurement->l2_samples < measurement->l2.sample_count ||
	    measurement->l2_samples >=
		    V34_PHASE2_PROBE_L2_RECEIVE_LIMIT_SAMPLES)
		return false;
	for (size_t index = 0; index < V34_PHASE2_PROBE_HIGHEST_HARMONIC;
	     ++index) {
		const struct v34_phase2_probe_spectrum_bin *l1 =
			&measurement->l1.harmonics[index];
		const struct v34_phase2_probe_spectrum_bin *l2 =
			&measurement->l2.harmonics[index];

		if (!isfinite(l1->in_phase_peak) ||
		    !isfinite(l1->quadrature_peak) ||
		    !isfinite(l1->mean_square) || l1->mean_square < 0.0 ||
		    !isfinite(l2->in_phase_peak) ||
		    !isfinite(l2->quadrature_peak) ||
		    !isfinite(l2->mean_square) || l2->mean_square < 0.0)
			return false;
	}
	return true;
}

bool v34_phase2_probe_receiver_is_valid(
	const struct v34_phase2_probe_receiver *receiver)
{
	uint32_t l1_samples;
	uint32_t l2_measured_samples;
	uint32_t l2_limit;

	if (receiver == NULL ||
	    !v34_phase2_probe_spectrum_is_valid(
		    &receiver->l1_spectrum) ||
	    !v34_phase2_probe_spectrum_is_valid(
		    &receiver->l2_spectrum) ||
	    (unsigned int)receiver->phase >
		    V34_PHASE2_PROBE_RECEIVER_LIMIT_REACHED)
		return false;
	l1_samples = receiver->l1_spectrum.samples_observed;
	l2_measured_samples = receiver->l2_spectrum.samples_observed;
	l2_limit = V34_PHASE2_PROBE_L2_RECEIVE_LIMIT_SAMPLES;
	if (l1_samples > V34_PHASE2_PROBE_L1_SAMPLES ||
	    l2_measured_samples > V34_PHASE2_PROBE_L1_SAMPLES ||
	    receiver->l2_samples < l2_measured_samples ||
	    receiver->l2_samples > l2_limit)
		return false;

	switch (receiver->phase) {
	case V34_PHASE2_PROBE_RECEIVER_MEASURE_L1:
		return l1_samples < V34_PHASE2_PROBE_L1_SAMPLES &&
		       l2_measured_samples == 0 && receiver->l2_samples == 0;
	case V34_PHASE2_PROBE_RECEIVER_MEASURE_L2:
		return l1_samples == V34_PHASE2_PROBE_L1_SAMPLES &&
		       l2_measured_samples <
			       V34_PHASE2_PROBE_L1_SAMPLES &&
		       receiver->l2_samples == l2_measured_samples;
	case V34_PHASE2_PROBE_RECEIVER_WAIT_L2_END:
		return l1_samples == V34_PHASE2_PROBE_L1_SAMPLES &&
		       l2_measured_samples ==
			       V34_PHASE2_PROBE_L1_SAMPLES &&
		       receiver->l2_samples >=
			       V34_PHASE2_PROBE_L1_SAMPLES &&
		       receiver->l2_samples < l2_limit;
	case V34_PHASE2_PROBE_RECEIVER_COMPLETE:
		return l1_samples == V34_PHASE2_PROBE_L1_SAMPLES &&
		       l2_window_is_valid(l2_measured_samples) &&
		       (l2_measured_samples == V34_PHASE2_PROBE_L1_SAMPLES ||
			receiver->l2_samples == l2_measured_samples) &&
		       receiver->l2_samples < l2_limit;
	case V34_PHASE2_PROBE_RECEIVER_LIMIT_REACHED:
		return l1_samples == V34_PHASE2_PROBE_L1_SAMPLES &&
		       l2_measured_samples ==
			       V34_PHASE2_PROBE_L1_SAMPLES &&
		       receiver->l2_samples == l2_limit;
	}
	return false;
}

enum v34_phase2_probe_receiver_result
v34_phase2_probe_receiver_init(
	struct v34_phase2_probe_receiver *receiver)
{
	struct v34_phase2_probe_receiver initialized = {
		.phase = V34_PHASE2_PROBE_RECEIVER_MEASURE_L1,
	};

	if (receiver == NULL ||
	    v34_phase2_probe_spectrum_init(
		    &initialized.l1_spectrum) !=
		    V34_PHASE2_PROBE_SPECTRUM_OK ||
	    v34_phase2_probe_spectrum_init(
		    &initialized.l2_spectrum) !=
		    V34_PHASE2_PROBE_SPECTRUM_OK ||
	    !v34_phase2_probe_receiver_is_valid(&initialized))
		return V34_PHASE2_PROBE_RECEIVER_INVALID_ARGUMENT;
	*receiver = initialized;
	return V34_PHASE2_PROBE_RECEIVER_OK;
}

static enum v34_phase2_probe_receiver_result measure_l1_sample(
	struct v34_phase2_probe_receiver *receiver,
	int16_t received_sample,
	struct v34_phase2_probe_receiver_event *event)
{
	if (v34_phase2_probe_spectrum_sample(
		    &receiver->l1_spectrum, received_sample) !=
	    V34_PHASE2_PROBE_SPECTRUM_OK)
		return V34_PHASE2_PROBE_RECEIVER_INVALID_ARGUMENT;
	if (receiver->l1_spectrum.samples_observed ==
	    V34_PHASE2_PROBE_L1_SAMPLES) {
		receiver->phase =
			V34_PHASE2_PROBE_RECEIVER_MEASURE_L2;
		event->kind =
			V34_PHASE2_PROBE_RECEIVER_EVENT_L2_STARTED;
	}
	return V34_PHASE2_PROBE_RECEIVER_OK;
}

static enum v34_phase2_probe_receiver_result measure_l2_sample(
	struct v34_phase2_probe_receiver *receiver,
	int16_t received_sample,
	struct v34_phase2_probe_receiver_event *event)
{
	if (v34_phase2_probe_spectrum_sample(
		    &receiver->l2_spectrum, received_sample) !=
	    V34_PHASE2_PROBE_SPECTRUM_OK)
		return V34_PHASE2_PROBE_RECEIVER_INVALID_ARGUMENT;
	receiver->l2_samples++;
	if (receiver->l2_samples ==
	    V34_PHASE2_PROBE_L1_SAMPLES) {
		receiver->phase =
			V34_PHASE2_PROBE_RECEIVER_WAIT_L2_END;
		event->kind =
			V34_PHASE2_PROBE_RECEIVER_EVENT_MEASUREMENT_READY;
		event->l2_samples = receiver->l2_samples;
	}
	return V34_PHASE2_PROBE_RECEIVER_OK;
}

static void wait_for_l2_end(
	struct v34_phase2_probe_receiver *receiver,
	bool finish_l2,
	struct v34_phase2_probe_receiver_event *event)
{
	if (finish_l2) {
		receiver->phase = V34_PHASE2_PROBE_RECEIVER_COMPLETE;
		event->kind =
			V34_PHASE2_PROBE_RECEIVER_EVENT_COMPLETED;
		event->l2_samples = receiver->l2_samples;
	} else if (++receiver->l2_samples ==
		   V34_PHASE2_PROBE_L2_RECEIVE_LIMIT_SAMPLES) {
		receiver->phase =
			V34_PHASE2_PROBE_RECEIVER_LIMIT_REACHED;
		event->kind =
			V34_PHASE2_PROBE_RECEIVER_EVENT_LIMIT_REACHED;
		event->l2_samples = receiver->l2_samples;
	}
}

enum v34_phase2_probe_receiver_result
v34_phase2_probe_receiver_sample(
	struct v34_phase2_probe_receiver *receiver,
	int16_t received_sample, bool finish_l2,
	struct v34_phase2_probe_receiver_event *event)
{
	struct v34_phase2_probe_receiver next;
	struct v34_phase2_probe_receiver_event emitted = { 0 };
	enum v34_phase2_probe_receiver_result result =
		V34_PHASE2_PROBE_RECEIVER_OK;

	if (!v34_phase2_probe_receiver_is_valid(receiver) ||
	    event == NULL)
		return V34_PHASE2_PROBE_RECEIVER_INVALID_ARGUMENT;
	next = *receiver;
	switch (next.phase) {
	case V34_PHASE2_PROBE_RECEIVER_MEASURE_L1:
		if (finish_l2)
			return V34_PHASE2_PROBE_RECEIVER_INVALID_ARGUMENT;
		result = measure_l1_sample(&next, received_sample, &emitted);
		break;
	case V34_PHASE2_PROBE_RECEIVER_MEASURE_L2:
		if (finish_l2) {
			if (!l2_window_is_valid(next.l2_samples))
				return V34_PHASE2_PROBE_RECEIVER_INVALID_ARGUMENT;
			wait_for_l2_end(&next, true, &emitted);
		} else {
			result = measure_l2_sample(&next, received_sample, &emitted);
		}
		break;
	case V34_PHASE2_PROBE_RECEIVER_WAIT_L2_END:
		wait_for_l2_end(&next, finish_l2, &emitted);
		break;
	case V34_PHASE2_PROBE_RECEIVER_COMPLETE:
	case V34_PHASE2_PROBE_RECEIVER_LIMIT_REACHED:
		break;
	}
	if (result != V34_PHASE2_PROBE_RECEIVER_OK ||
	    !v34_phase2_probe_receiver_is_valid(&next))
		return V34_PHASE2_PROBE_RECEIVER_INVALID_ARGUMENT;
	*receiver = next;
	*event = emitted;
	return V34_PHASE2_PROBE_RECEIVER_OK;
}

enum v34_phase2_probe_receiver_result
v34_phase2_probe_receiver_measure(
	const struct v34_phase2_probe_receiver *receiver,
	struct v34_phase2_probe_receiver_measurement *measurement)
{
	struct v34_phase2_probe_receiver_measurement measured = { 0 };

	if (!v34_phase2_probe_receiver_is_valid(receiver) ||
	    measurement == NULL ||
	    receiver->phase ==
		    V34_PHASE2_PROBE_RECEIVER_MEASURE_L1 ||
	    receiver->phase ==
		    V34_PHASE2_PROBE_RECEIVER_MEASURE_L2 ||
	    v34_phase2_probe_spectrum_measure(
		    &receiver->l1_spectrum, &measured.l1) !=
		    V34_PHASE2_PROBE_SPECTRUM_OK ||
	    v34_phase2_probe_spectrum_measure(
		    &receiver->l2_spectrum, &measured.l2) !=
		    V34_PHASE2_PROBE_SPECTRUM_OK)
		return V34_PHASE2_PROBE_RECEIVER_INVALID_ARGUMENT;
	measured.l2_samples = receiver->l2_samples;
	*measurement = measured;
	return V34_PHASE2_PROBE_RECEIVER_OK;
}
