// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2_probe_acquisition.h"

#include "memory_region.h"

#include <stddef.h>

enum {
	V34_PHASE2_PROBE_INITIAL_GAIN = 0x0600,
};

_Static_assert(
	(int)V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES <
		(int)V34_PHASE2_PROBE_L1_SAMPLES,
	"Probe synchronization replay must end within the L1 window");

static bool spectrum_is_empty(
	const struct v34_phase2_probe_spectrum *spectrum)
{
	if (spectrum->samples_observed != 0)
		return false;
	for (size_t index = 0;
	     index < V34_PHASE2_PROBE_HIGHEST_HARMONIC; ++index)
		if (spectrum->harmonics[index].in_phase_sum != 0.0 ||
		    spectrum->harmonics[index].quadrature_sum != 0.0)
			return false;
	return true;
}

static bool receiver_is_empty(
	const struct v34_phase2_probe_receiver *receiver)
{
	return receiver->phase ==
		       V34_PHASE2_PROBE_RECEIVER_MEASURE_L1 &&
	       receiver->l2_samples == 0 &&
	       spectrum_is_empty(&receiver->l1_spectrum) &&
	       spectrum_is_empty(&receiver->l2_spectrum);
}

bool v34_phase2_probe_acquisition_is_valid(
	const struct v34_phase2_probe_acquisition *acquisition)
{
	uint32_t l1_samples;
	bool gain_ready;

	if (acquisition == NULL ||
	    !v34_phase2_probe_synchronizer_is_valid(
		    &acquisition->synchronizer) ||
	    !v34_phase2_probe_receiver_is_valid(
		    &acquisition->receiver) ||
	    !v34_analytic_filter_is_valid(&acquisition->gain_filter) ||
	    !v34_gain_control_is_valid(&acquisition->gain) ||
	    (unsigned int)acquisition->phase >
		    V34_PHASE2_PROBE_ACQUISITION_LIMIT_REACHED)
		return false;
	l1_samples = acquisition->receiver.l1_spectrum.samples_observed;
	gain_ready = l1_samples == V34_PHASE2_PROBE_L1_SAMPLES;
	if (acquisition->gain_samples != l1_samples ||
	    acquisition->gain.mode !=
		    (gain_ready ? V34_GAIN_FROZEN : V34_GAIN_TRACKING))
		return false;

	switch (acquisition->phase) {
	case V34_PHASE2_PROBE_ACQUISITION_SEARCHING:
		return acquisition->synchronizer.phase ==
			       V34_PHASE2_PROBE_SYNCHRONIZER_COLLECTING &&
		       receiver_is_empty(&acquisition->receiver);
	case V34_PHASE2_PROBE_ACQUISITION_RECEIVING:
		return acquisition->synchronizer.phase ==
			       V34_PHASE2_PROBE_SYNCHRONIZER_ACQUIRED &&
		       acquisition->receiver.l1_spectrum.samples_observed >=
			       V34_PHASE2_PROBE_PERIOD_SAMPLES &&
		       acquisition->receiver.phase !=
			       V34_PHASE2_PROBE_RECEIVER_COMPLETE &&
		       acquisition->receiver.phase !=
			       V34_PHASE2_PROBE_RECEIVER_LIMIT_REACHED;
	case V34_PHASE2_PROBE_ACQUISITION_NOT_FOUND:
		return acquisition->synchronizer.phase ==
			       V34_PHASE2_PROBE_SYNCHRONIZER_NOT_FOUND &&
		       receiver_is_empty(&acquisition->receiver);
	case V34_PHASE2_PROBE_ACQUISITION_COMPLETE:
		return acquisition->synchronizer.phase ==
			       V34_PHASE2_PROBE_SYNCHRONIZER_ACQUIRED &&
		       acquisition->receiver.phase ==
			       V34_PHASE2_PROBE_RECEIVER_COMPLETE;
	case V34_PHASE2_PROBE_ACQUISITION_LIMIT_REACHED:
		return acquisition->synchronizer.phase ==
			       V34_PHASE2_PROBE_SYNCHRONIZER_ACQUIRED &&
		       acquisition->receiver.phase ==
			       V34_PHASE2_PROBE_RECEIVER_LIMIT_REACHED;
	}
	return false;
}

enum v34_phase2_probe_acquisition_result
v34_phase2_probe_acquisition_init(
	struct v34_phase2_probe_acquisition *acquisition)
{
	struct v34_phase2_probe_acquisition initialized = {
		.phase = V34_PHASE2_PROBE_ACQUISITION_SEARCHING,
	};

	if (acquisition == NULL ||
	    v34_phase2_probe_synchronizer_init(
		    &initialized.synchronizer) !=
		    V34_PHASE2_PROBE_SYNCHRONIZER_OK ||
	    v34_phase2_probe_receiver_init(&initialized.receiver) !=
		    V34_PHASE2_PROBE_RECEIVER_OK ||
	    v34_analytic_filter_init(&initialized.gain_filter) !=
		    V34_ANALYTIC_FILTER_OK ||
	    v34_gain_control_init(
		    &initialized.gain, V34_PHASE2_PROBE_INITIAL_GAIN) !=
		    V34_GAIN_CONTROL_OK ||
	    !v34_phase2_probe_acquisition_is_valid(&initialized))
		return V34_PHASE2_PROBE_ACQUISITION_INVALID_ARGUMENT;
	*acquisition = initialized;
	return V34_PHASE2_PROBE_ACQUISITION_OK;
}

static bool observe_gain(
	struct v34_phase2_probe_acquisition *acquisition, int16_t sample)
{
	struct v34_analytic_sample analytic;
	struct v34_analytic_sample gained;

	if (v34_analytic_filter_process(
		    &acquisition->gain_filter, sample, &analytic) !=
		    V34_ANALYTIC_FILTER_OK ||
	    v34_gain_control_process(
		    &acquisition->gain, analytic, &gained) !=
		    V34_GAIN_CONTROL_OK ||
	    acquisition->gain_samples == UINT32_MAX)
		return false;
	acquisition->gain_samples++;
	return true;
}

static bool replay_synchronized_prefix(
	struct v34_phase2_probe_acquisition *acquisition,
	const struct v34_phase2_probe_synchronizer_event *sync_event,
	struct v34_phase2_probe_acquisition_event *event)
{
	uint16_t replay_samples =
		v34_phase2_probe_synchronizer_replay_count(
			&acquisition->synchronizer);

	if (replay_samples != sync_event->replay_samples)
		return false;
	for (uint16_t index = 0; index < replay_samples; ++index) {
		struct v34_phase2_probe_receiver_event receiver_event;
		int16_t sample;

		if (v34_phase2_probe_synchronizer_replay(
			    &acquisition->synchronizer, index, &sample) !=
			    V34_PHASE2_PROBE_SYNCHRONIZER_OK ||
		    !observe_gain(acquisition, sample) ||
		    v34_phase2_probe_receiver_sample(
			    &acquisition->receiver, sample, false,
			    &receiver_event) !=
			    V34_PHASE2_PROBE_RECEIVER_OK ||
		    receiver_event.kind !=
			    V34_PHASE2_PROBE_RECEIVER_EVENT_NONE)
			return false;
	}
	acquisition->phase =
		V34_PHASE2_PROBE_ACQUISITION_RECEIVING;
	event->kind = V34_PHASE2_PROBE_ACQUISITION_EVENT_ACQUIRED;
	event->leading_samples = sync_event->leading_samples;
	event->replay_samples = replay_samples;
	event->probe_energy_fraction = sync_event->probe_energy_fraction;
	return true;
}

static bool search_for_probe(
	struct v34_phase2_probe_acquisition *acquisition,
	int16_t received_sample,
	struct v34_phase2_probe_acquisition_event *event)
{
	struct v34_phase2_probe_synchronizer_event sync_event;

	if (v34_phase2_probe_synchronizer_sample(
		    &acquisition->synchronizer, received_sample, &sync_event) !=
	    V34_PHASE2_PROBE_SYNCHRONIZER_OK)
		return false;
	switch (sync_event.kind) {
	case V34_PHASE2_PROBE_SYNCHRONIZER_EVENT_NONE:
		return true;
	case V34_PHASE2_PROBE_SYNCHRONIZER_EVENT_ACQUIRED:
		return replay_synchronized_prefix(
			acquisition, &sync_event, event);
	case V34_PHASE2_PROBE_SYNCHRONIZER_EVENT_NOT_FOUND:
		acquisition->phase =
			V34_PHASE2_PROBE_ACQUISITION_NOT_FOUND;
		event->kind =
			V34_PHASE2_PROBE_ACQUISITION_EVENT_NOT_FOUND;
		event->leading_samples = sync_event.leading_samples;
		event->probe_energy_fraction =
			sync_event.probe_energy_fraction;
		return true;
	}
	return false;
}

static bool receive_probe_sample(
	struct v34_phase2_probe_acquisition *acquisition,
	int16_t received_sample, bool finish_l2,
	struct v34_phase2_probe_acquisition_event *event)
{
	struct v34_phase2_probe_receiver_event receiver_event;
	bool measuring_l1 = acquisition->receiver.phase ==
				    V34_PHASE2_PROBE_RECEIVER_MEASURE_L1;

	if ((measuring_l1 && !finish_l2 &&
	     !observe_gain(acquisition, received_sample)) ||
	    v34_phase2_probe_receiver_sample(
		    &acquisition->receiver, received_sample, finish_l2,
		    &receiver_event) != V34_PHASE2_PROBE_RECEIVER_OK)
		return false;
	event->l2_samples = receiver_event.l2_samples;
	switch (receiver_event.kind) {
	case V34_PHASE2_PROBE_RECEIVER_EVENT_NONE:
		return true;
	case V34_PHASE2_PROBE_RECEIVER_EVENT_L2_STARTED:
		if (v34_gain_control_set_mode(
			    &acquisition->gain, V34_GAIN_FROZEN) !=
		    V34_GAIN_CONTROL_OK)
			return false;
		event->kind =
			V34_PHASE2_PROBE_ACQUISITION_EVENT_L2_STARTED;
		return true;
	case V34_PHASE2_PROBE_RECEIVER_EVENT_MEASUREMENT_READY:
		event->kind =
			V34_PHASE2_PROBE_ACQUISITION_EVENT_MEASUREMENT_READY;
		return true;
	case V34_PHASE2_PROBE_RECEIVER_EVENT_COMPLETED:
		acquisition->phase =
			V34_PHASE2_PROBE_ACQUISITION_COMPLETE;
		event->kind =
			V34_PHASE2_PROBE_ACQUISITION_EVENT_COMPLETED;
		return true;
	case V34_PHASE2_PROBE_RECEIVER_EVENT_LIMIT_REACHED:
		acquisition->phase =
			V34_PHASE2_PROBE_ACQUISITION_LIMIT_REACHED;
		event->kind =
			V34_PHASE2_PROBE_ACQUISITION_EVENT_LIMIT_REACHED;
		return true;
	}
	return false;
}

enum v34_phase2_probe_acquisition_result
v34_phase2_probe_acquisition_receive_gain(
	const struct v34_phase2_probe_acquisition *acquisition,
	int16_t *receive_gain)
{
	if (!v34_phase2_probe_acquisition_is_valid(acquisition) ||
	    receive_gain == NULL ||
	    memory_regions_overlap(
		    acquisition, sizeof(*acquisition),
		    receive_gain, sizeof(*receive_gain)) ||
	    acquisition->gain.mode != V34_GAIN_FROZEN)
		return V34_PHASE2_PROBE_ACQUISITION_INVALID_ARGUMENT;
	*receive_gain = acquisition->gain.gain;
	return V34_PHASE2_PROBE_ACQUISITION_OK;
}

enum v34_phase2_probe_acquisition_result
v34_phase2_probe_acquisition_sample(
	struct v34_phase2_probe_acquisition *acquisition,
	int16_t received_sample, bool finish_l2,
	struct v34_phase2_probe_acquisition_event *event)
{
	struct v34_phase2_probe_acquisition next;
	struct v34_phase2_probe_acquisition_event emitted = { 0 };
	bool accepted = true;

	if (!v34_phase2_probe_acquisition_is_valid(acquisition) ||
	    event == NULL)
		return V34_PHASE2_PROBE_ACQUISITION_INVALID_ARGUMENT;
	next = *acquisition;
	switch (next.phase) {
	case V34_PHASE2_PROBE_ACQUISITION_SEARCHING:
		accepted = !finish_l2 &&
			   search_for_probe(&next, received_sample, &emitted);
		break;
	case V34_PHASE2_PROBE_ACQUISITION_RECEIVING:
		accepted = receive_probe_sample(
			&next, received_sample, finish_l2, &emitted);
		break;
	case V34_PHASE2_PROBE_ACQUISITION_NOT_FOUND:
	case V34_PHASE2_PROBE_ACQUISITION_COMPLETE:
	case V34_PHASE2_PROBE_ACQUISITION_LIMIT_REACHED:
		break;
	}
	if (!accepted ||
	    !v34_phase2_probe_acquisition_is_valid(&next))
		return V34_PHASE2_PROBE_ACQUISITION_INVALID_ARGUMENT;
	*acquisition = next;
	*event = emitted;
	return V34_PHASE2_PROBE_ACQUISITION_OK;
}

enum v34_phase2_probe_acquisition_result
v34_phase2_probe_acquisition_measure(
	const struct v34_phase2_probe_acquisition *acquisition,
	struct v34_phase2_probe_receiver_measurement *measurement)
{
	if (!v34_phase2_probe_acquisition_is_valid(acquisition) ||
	    measurement == NULL ||
	    v34_phase2_probe_receiver_measure(
		    &acquisition->receiver, measurement) !=
		    V34_PHASE2_PROBE_RECEIVER_OK)
		return V34_PHASE2_PROBE_ACQUISITION_INVALID_ARGUMENT;
	return V34_PHASE2_PROBE_ACQUISITION_OK;
}
