// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_v34_phase2_probe_acquisition.h"

#include <stddef.h>

_Static_assert(
	(int)SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES <
		(int)SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES,
	"Probe synchronization replay must end within the L1 window");

static bool spectrum_is_empty(
	const struct sippymodem_v34_phase2_probe_spectrum *spectrum)
{
	if (spectrum->samples_observed != 0 ||
	    spectrum->window_start_sample != 0)
		return false;
	for (size_t index = 0;
	     index < SIPPYMODEM_V34_PHASE2_PROBE_HIGHEST_HARMONIC; ++index)
		if (spectrum->harmonics[index].in_phase_sum != 0.0 ||
		    spectrum->harmonics[index].quadrature_sum != 0.0)
			return false;
	return true;
}

static bool receiver_is_empty(
	const struct sippymodem_v34_phase2_probe_receiver *receiver)
{
	return receiver->phase ==
		       SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_MEASURE_L1 &&
	       receiver->l2_samples == 0 &&
	       spectrum_is_empty(&receiver->l1_spectrum) &&
	       spectrum_is_empty(&receiver->l2_spectrum);
}

bool sippymodem_v34_phase2_probe_acquisition_is_valid(
	const struct sippymodem_v34_phase2_probe_acquisition *acquisition)
{
	if (acquisition == NULL ||
	    !sippymodem_v34_phase2_probe_synchronizer_is_valid(
		    &acquisition->synchronizer) ||
	    !sippymodem_v34_phase2_probe_receiver_is_valid(
		    &acquisition->receiver) ||
	    (unsigned int)acquisition->phase >
		    SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_LIMIT_REACHED)
		return false;

	switch (acquisition->phase) {
	case SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_SEARCHING:
		return acquisition->synchronizer.phase ==
			       SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_COLLECTING &&
		       receiver_is_empty(&acquisition->receiver);
	case SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_RECEIVING:
		return acquisition->synchronizer.phase ==
			       SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_ACQUIRED &&
		       acquisition->receiver.l1_spectrum.samples_observed >=
			       SIPPYMODEM_V34_PHASE2_PROBE_PERIOD_SAMPLES &&
		       acquisition->receiver.phase !=
			       SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_COMPLETE &&
		       acquisition->receiver.phase !=
			       SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_LIMIT_REACHED;
	case SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_NOT_FOUND:
		return acquisition->synchronizer.phase ==
			       SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_NOT_FOUND &&
		       receiver_is_empty(&acquisition->receiver);
	case SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_COMPLETE:
		return acquisition->synchronizer.phase ==
			       SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_ACQUIRED &&
		       acquisition->receiver.phase ==
			       SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_COMPLETE;
	case SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_LIMIT_REACHED:
		return acquisition->synchronizer.phase ==
			       SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_ACQUIRED &&
		       acquisition->receiver.phase ==
			       SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_LIMIT_REACHED;
	}
	return false;
}

enum sippymodem_v34_phase2_probe_acquisition_result
sippymodem_v34_phase2_probe_acquisition_init(
	struct sippymodem_v34_phase2_probe_acquisition *acquisition)
{
	struct sippymodem_v34_phase2_probe_acquisition initialized = {
		.phase = SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_SEARCHING,
	};

	if (acquisition == NULL ||
	    sippymodem_v34_phase2_probe_synchronizer_init(
		    &initialized.synchronizer) !=
		    SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_OK ||
	    sippymodem_v34_phase2_probe_receiver_init(&initialized.receiver) !=
		    SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_OK ||
	    !sippymodem_v34_phase2_probe_acquisition_is_valid(&initialized))
		return SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_INVALID_ARGUMENT;
	*acquisition = initialized;
	return SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_OK;
}

static bool replay_synchronized_prefix(
	struct sippymodem_v34_phase2_probe_acquisition *acquisition,
	const struct sippymodem_v34_phase2_probe_synchronizer_event *sync_event,
	struct sippymodem_v34_phase2_probe_acquisition_event *event)
{
	uint16_t replay_samples =
		sippymodem_v34_phase2_probe_synchronizer_replay_count(
			&acquisition->synchronizer);

	if (replay_samples != sync_event->replay_samples)
		return false;
	for (uint16_t index = 0; index < replay_samples; ++index) {
		struct sippymodem_v34_phase2_probe_receiver_event receiver_event;
		int16_t sample;

		if (sippymodem_v34_phase2_probe_synchronizer_replay(
			    &acquisition->synchronizer, index, &sample) !=
			    SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_OK ||
		    sippymodem_v34_phase2_probe_receiver_sample(
			    &acquisition->receiver, sample, false,
			    &receiver_event) !=
			    SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_OK ||
		    receiver_event.kind !=
			    SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_EVENT_NONE)
			return false;
	}
	acquisition->phase =
		SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_RECEIVING;
	event->kind = SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_EVENT_ACQUIRED;
	event->leading_samples = sync_event->leading_samples;
	event->replay_samples = replay_samples;
	event->normalized_correlation = sync_event->normalized_correlation;
	return true;
}

static bool search_for_probe(
	struct sippymodem_v34_phase2_probe_acquisition *acquisition,
	int16_t received_sample,
	struct sippymodem_v34_phase2_probe_acquisition_event *event)
{
	struct sippymodem_v34_phase2_probe_synchronizer_event sync_event;

	if (sippymodem_v34_phase2_probe_synchronizer_sample(
		    &acquisition->synchronizer, received_sample, &sync_event) !=
	    SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_OK)
		return false;
	switch (sync_event.kind) {
	case SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_EVENT_NONE:
		return true;
	case SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_EVENT_ACQUIRED:
		return replay_synchronized_prefix(
			acquisition, &sync_event, event);
	case SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_EVENT_NOT_FOUND:
		acquisition->phase =
			SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_NOT_FOUND;
		event->kind =
			SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_EVENT_NOT_FOUND;
		event->leading_samples = sync_event.leading_samples;
		event->normalized_correlation =
			sync_event.normalized_correlation;
		return true;
	}
	return false;
}

static bool receive_probe_sample(
	struct sippymodem_v34_phase2_probe_acquisition *acquisition,
	int16_t received_sample, bool finish_l2,
	struct sippymodem_v34_phase2_probe_acquisition_event *event)
{
	struct sippymodem_v34_phase2_probe_receiver_event receiver_event;

	if (sippymodem_v34_phase2_probe_receiver_sample(
		    &acquisition->receiver, received_sample, finish_l2,
		    &receiver_event) != SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_OK)
		return false;
	event->l2_samples = receiver_event.l2_samples;
	switch (receiver_event.kind) {
	case SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_EVENT_NONE:
		return true;
	case SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_EVENT_L2_STARTED:
		event->kind =
			SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_EVENT_L2_STARTED;
		return true;
	case SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_EVENT_MEASUREMENT_READY:
		event->kind =
			SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_EVENT_MEASUREMENT_READY;
		return true;
	case SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_EVENT_COMPLETED:
		acquisition->phase =
			SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_COMPLETE;
		event->kind =
			SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_EVENT_COMPLETED;
		return true;
	case SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_EVENT_LIMIT_REACHED:
		acquisition->phase =
			SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_LIMIT_REACHED;
		event->kind =
			SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_EVENT_LIMIT_REACHED;
		return true;
	}
	return false;
}

enum sippymodem_v34_phase2_probe_acquisition_result
sippymodem_v34_phase2_probe_acquisition_sample(
	struct sippymodem_v34_phase2_probe_acquisition *acquisition,
	int16_t received_sample, bool finish_l2,
	struct sippymodem_v34_phase2_probe_acquisition_event *event)
{
	struct sippymodem_v34_phase2_probe_acquisition next;
	struct sippymodem_v34_phase2_probe_acquisition_event emitted = { 0 };
	bool accepted = true;

	if (!sippymodem_v34_phase2_probe_acquisition_is_valid(acquisition) ||
	    event == NULL)
		return SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_INVALID_ARGUMENT;
	next = *acquisition;
	switch (next.phase) {
	case SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_SEARCHING:
		accepted = !finish_l2 &&
			   search_for_probe(&next, received_sample, &emitted);
		break;
	case SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_RECEIVING:
		accepted = receive_probe_sample(
			&next, received_sample, finish_l2, &emitted);
		break;
	case SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_NOT_FOUND:
	case SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_COMPLETE:
	case SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_LIMIT_REACHED:
		break;
	}
	if (!accepted ||
	    !sippymodem_v34_phase2_probe_acquisition_is_valid(&next))
		return SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_INVALID_ARGUMENT;
	*acquisition = next;
	*event = emitted;
	return SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_OK;
}

enum sippymodem_v34_phase2_probe_acquisition_result
sippymodem_v34_phase2_probe_acquisition_measure(
	const struct sippymodem_v34_phase2_probe_acquisition *acquisition,
	struct sippymodem_v34_phase2_probe_receiver_measurement *measurement)
{
	if (!sippymodem_v34_phase2_probe_acquisition_is_valid(acquisition) ||
	    measurement == NULL ||
	    sippymodem_v34_phase2_probe_receiver_measure(
		    &acquisition->receiver, measurement) !=
		    SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_OK)
		return SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_INVALID_ARGUMENT;
	return SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_OK;
}

const char *sippymodem_v34_phase2_probe_acquisition_result_name(
	enum sippymodem_v34_phase2_probe_acquisition_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_OK:
		return "ok";
	case SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
