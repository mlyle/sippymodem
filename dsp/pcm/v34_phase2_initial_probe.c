// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2_initial_probe.h"

#include <stddef.h>

_Static_assert(
	V34_PHASE2_INITIAL_PROBE_TONE_WAIT_SAMPLES <= UINT16_MAX,
	"Initial-probe tone deadline must fit its sample counter");

static enum v34_station_role local_role(
	const struct v34_phase2_initial_probe *probe)
{
	return probe->ranging.info0_exchange.local_role;
}

static int16_t nominal_amplitude(
	const struct v34_phase2_ranging *ranging)
{
	const struct v34_phase2_tone_transmitter *tone =
		&ranging->info0_exchange.tone_transmitter;

	return ranging->info0_exchange.local_role ==
		       V34_CALLING_STATION ?
		       tone->carrier_amplitude :
		       tone->guard_amplitude;
}

static bool caller_path_is_valid(
	const struct v34_phase2_initial_probe *probe)
{
	const struct v34_phase2_probe_acquisition *receive =
		&probe->path.receive;

	if (!v34_phase2_probe_acquisition_is_valid(receive))
		return false;
	switch (probe->phase) {
	case V34_PHASE2_INITIAL_PROBE_CALLER_RECEIVING:
		return probe->tone_wait_samples == 0 &&
		       probe->failure ==
			       V34_PHASE2_INITIAL_PROBE_NO_FAILURE &&
		       (receive->phase ==
				V34_PHASE2_PROBE_ACQUISITION_SEARCHING ||
			receive->phase ==
				V34_PHASE2_PROBE_ACQUISITION_RECEIVING);
	case V34_PHASE2_INITIAL_PROBE_CALLER_TONE_B:
		return probe->tone_wait_samples > 0 &&
		       probe->tone_wait_samples <
			       V34_PHASE2_INITIAL_PROBE_TONE_WAIT_SAMPLES &&
		       probe->failure ==
			       V34_PHASE2_INITIAL_PROBE_NO_FAILURE &&
		       receive->phase ==
			       V34_PHASE2_PROBE_ACQUISITION_COMPLETE;
	case V34_PHASE2_INITIAL_PROBE_CALLER_TONE_PAIR_ACTIVE:
		return probe->tone_wait_samples > 0 &&
		       probe->tone_wait_samples <=
			       V34_PHASE2_INITIAL_PROBE_TONE_WAIT_SAMPLES &&
		       probe->failure ==
			       V34_PHASE2_INITIAL_PROBE_NO_FAILURE &&
		       receive->phase ==
			       V34_PHASE2_PROBE_ACQUISITION_COMPLETE;
	case V34_PHASE2_INITIAL_PROBE_FAILED:
		switch (probe->failure) {
		case V34_PHASE2_INITIAL_PROBE_REMOTE_NOT_FOUND:
			return probe->tone_wait_samples == 0 &&
			       receive->phase ==
				       V34_PHASE2_PROBE_ACQUISITION_NOT_FOUND;
		case V34_PHASE2_INITIAL_PROBE_REMOTE_TONE_TIMEOUT:
			return probe->tone_wait_samples ==
				       V34_PHASE2_INITIAL_PROBE_TONE_WAIT_SAMPLES &&
			       receive->phase ==
				       V34_PHASE2_PROBE_ACQUISITION_COMPLETE;
		case V34_PHASE2_INITIAL_PROBE_NO_FAILURE:
		case V34_PHASE2_INITIAL_PROBE_LOCAL_TRANSMIT_LIMIT:
			return false;
		}
		return false;
	case V34_PHASE2_INITIAL_PROBE_ANSWERER_TRANSMITTING:
	case V34_PHASE2_INITIAL_PROBE_ANSWERER_TONE_A:
		return false;
	}
	return false;
}

static bool answerer_path_is_valid(
	const struct v34_phase2_initial_probe *probe)
{
	const struct v34_phase2_probe_sequence *transmit =
		&probe->path.transmit;

	if (!v34_phase2_probe_sequence_is_valid(transmit) ||
	    probe->tone_wait_samples != 0)
		return false;
	switch (probe->phase) {
	case V34_PHASE2_INITIAL_PROBE_ANSWERER_TRANSMITTING:
		return probe->failure ==
			       V34_PHASE2_INITIAL_PROBE_NO_FAILURE &&
		       (transmit->phase ==
				V34_PHASE2_PROBE_SEQUENCE_PHASE_L1 ||
			transmit->phase ==
				V34_PHASE2_PROBE_SEQUENCE_PHASE_L2);
	case V34_PHASE2_INITIAL_PROBE_ANSWERER_TONE_A:
		return probe->failure ==
			       V34_PHASE2_INITIAL_PROBE_NO_FAILURE &&
		       transmit->phase ==
			       V34_PHASE2_PROBE_SEQUENCE_PHASE_COMPLETE;
	case V34_PHASE2_INITIAL_PROBE_FAILED:
		return probe->failure ==
			       V34_PHASE2_INITIAL_PROBE_LOCAL_TRANSMIT_LIMIT &&
		       transmit->phase ==
			       V34_PHASE2_PROBE_SEQUENCE_PHASE_LIMIT_REACHED;
	case V34_PHASE2_INITIAL_PROBE_CALLER_RECEIVING:
	case V34_PHASE2_INITIAL_PROBE_CALLER_TONE_B:
	case V34_PHASE2_INITIAL_PROBE_CALLER_TONE_PAIR_ACTIVE:
		return false;
	}
	return false;
}

bool v34_phase2_initial_probe_is_valid(
	const struct v34_phase2_initial_probe *probe)
{
	if (probe == NULL ||
	    !v34_phase2_ranging_is_valid(&probe->ranging) ||
	    probe->ranging.phase != V34_PHASE2_PROBE_READY ||
	    (unsigned int)probe->phase >
		    V34_PHASE2_INITIAL_PROBE_FAILED ||
	    (unsigned int)probe->failure >
		    V34_PHASE2_INITIAL_PROBE_REMOTE_TONE_TIMEOUT)
		return false;
	return local_role(probe) == V34_CALLING_STATION ?
		       caller_path_is_valid(probe) :
		       answerer_path_is_valid(probe);
}

enum v34_phase2_initial_probe_result
v34_phase2_initial_probe_init(
	struct v34_phase2_initial_probe *probe,
	const struct v34_phase2_ranging *completed_ranging)
{
	struct v34_phase2_initial_probe initialized = { 0 };
	enum v34_station_role role;

	if (probe == NULL ||
	    !v34_phase2_ranging_is_valid(completed_ranging) ||
	    completed_ranging->phase != V34_PHASE2_PROBE_READY)
		return V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	initialized.ranging = *completed_ranging;
	role = completed_ranging->info0_exchange.local_role;
	if (role == V34_CALLING_STATION) {
		initialized.phase =
			V34_PHASE2_INITIAL_PROBE_CALLER_RECEIVING;
		if (v34_phase2_probe_acquisition_init(
			    &initialized.path.receive) !=
		    V34_PHASE2_PROBE_ACQUISITION_OK)
			return V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	} else {
		initialized.phase =
			V34_PHASE2_INITIAL_PROBE_ANSWERER_TRANSMITTING;
		if (v34_phase2_probe_sequence_init(
			    &initialized.path.transmit,
			    nominal_amplitude(completed_ranging),
			    completed_ranging->round_trip_delay_samples) !=
		    V34_PHASE2_PROBE_SEQUENCE_OK)
			return V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	}
	if (!v34_phase2_initial_probe_is_valid(&initialized))
		return V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	*probe = initialized;
	return V34_PHASE2_INITIAL_PROBE_OK;
}

static bool observe_remote_tone(
	struct v34_phase2_initial_probe *probe,
	int16_t received_sample,
	struct v34_phase2_initial_probe_event *event)
{
	return v34_phase2_tone_detector_sample(
		       &probe->ranging.info0_exchange.tone_detector,
		       received_sample, &event->tone_event) ==
	       V34_PHASE2_TONE_OK;
}

static bool reset_remote_tone_detector(
	struct v34_phase2_initial_probe *probe)
{
	return v34_phase2_tone_detector_reset(
		       &probe->ranging.info0_exchange.tone_detector) ==
	       V34_PHASE2_TONE_OK;
}

static bool generate_local_tone(
	struct v34_phase2_initial_probe *probe, int16_t *sample)
{
	return v34_phase2_tone_transmitter_sample(
		       &probe->ranging.info0_exchange.tone_transmitter, sample) ==
	       V34_PHASE2_TONE_OK;
}

static void fail_probe(
	struct v34_phase2_initial_probe *probe,
	enum v34_phase2_initial_probe_failure failure,
	struct v34_phase2_initial_probe_event *event)
{
	probe->phase = V34_PHASE2_INITIAL_PROBE_FAILED;
	probe->failure = failure;
	event->kind = V34_PHASE2_INITIAL_PROBE_EVENT_FAILED;
	event->failure = failure;
}

static bool receive_remote_probe(
	struct v34_phase2_initial_probe *probe,
	int16_t received_sample, int16_t *transmit_sample,
	struct v34_phase2_initial_probe_event *event)
{
	struct v34_phase2_probe_acquisition *receive =
		&probe->path.receive;

	*transmit_sample = 0;
	if (receive->receiver.phase ==
	    V34_PHASE2_PROBE_RECEIVER_WAIT_L2_END) {
		if (v34_phase2_probe_acquisition_sample(
			    receive, received_sample, true,
			    &event->receive_event) !=
			    V34_PHASE2_PROBE_ACQUISITION_OK ||
		    !reset_remote_tone_detector(probe) ||
		    !observe_remote_tone(probe, received_sample, event) ||
		    !generate_local_tone(probe, transmit_sample))
			return false;
		probe->phase =
			V34_PHASE2_INITIAL_PROBE_CALLER_TONE_B;
		probe->tone_wait_samples = 1;
		event->kind =
			V34_PHASE2_INITIAL_PROBE_EVENT_REMOTE_PROBE_COMPLETE;
		return true;
	}
	if (v34_phase2_probe_acquisition_sample(
		    receive, received_sample, false, &event->receive_event) !=
	    V34_PHASE2_PROBE_ACQUISITION_OK)
		return false;
	switch (event->receive_event.kind) {
	case V34_PHASE2_PROBE_ACQUISITION_EVENT_NOT_FOUND:
		fail_probe(
			probe,
			V34_PHASE2_INITIAL_PROBE_REMOTE_NOT_FOUND,
			event);
		break;
	case V34_PHASE2_PROBE_ACQUISITION_EVENT_NONE:
	case V34_PHASE2_PROBE_ACQUISITION_EVENT_ACQUIRED:
	case V34_PHASE2_PROBE_ACQUISITION_EVENT_L2_STARTED:
	case V34_PHASE2_PROBE_ACQUISITION_EVENT_MEASUREMENT_READY:
		break;
	case V34_PHASE2_PROBE_ACQUISITION_EVENT_COMPLETED:
	case V34_PHASE2_PROBE_ACQUISITION_EVENT_LIMIT_REACHED:
		return false;
	}
	return true;
}

static bool transmit_local_probe(
	struct v34_phase2_initial_probe *probe,
	int16_t received_sample, int16_t *transmit_sample,
	struct v34_phase2_initial_probe_event *event)
{
	struct v34_phase2_probe_sequence *transmit =
		&probe->path.transmit;
	bool remote_tone_started;

	if (!observe_remote_tone(probe, received_sample, event))
		return false;
	remote_tone_started = event->tone_event.kind ==
			      V34_PHASE2_TONE_STARTED;
	if (remote_tone_started &&
	    transmit->phase == V34_PHASE2_PROBE_SEQUENCE_PHASE_L2 &&
	    transmit->samples_emitted > V34_PHASE2_PROBE_L1_SAMPLES) {
		if (v34_phase2_probe_sequence_sample(
			    transmit, true, transmit_sample,
			    &event->transmit_event) !=
		    V34_PHASE2_PROBE_SEQUENCE_OK)
			return false;
		probe->phase =
			V34_PHASE2_INITIAL_PROBE_ANSWERER_TONE_A;
		event->kind =
			V34_PHASE2_INITIAL_PROBE_EVENT_LOCAL_PROBE_COMPLETE;
		return true;
	}
	if (v34_phase2_probe_sequence_sample(
		    transmit, false, transmit_sample, &event->transmit_event) !=
	    V34_PHASE2_PROBE_SEQUENCE_OK)
		return false;
	if (event->transmit_event.kind ==
	    V34_PHASE2_PROBE_SEQUENCE_EVENT_LIMIT_REACHED)
		fail_probe(
			probe,
			V34_PHASE2_INITIAL_PROBE_LOCAL_TRANSMIT_LIMIT,
			event);
	return true;
}

static bool continue_caller_tone(
	struct v34_phase2_initial_probe *probe,
	int16_t received_sample, int16_t *transmit_sample,
	struct v34_phase2_initial_probe_event *event)
{
	if (!observe_remote_tone(probe, received_sample, event) ||
	    !generate_local_tone(probe, transmit_sample))
		return false;
	if (probe->phase ==
	    V34_PHASE2_INITIAL_PROBE_CALLER_TONE_PAIR_ACTIVE)
		return true;
	probe->tone_wait_samples++;
	if (event->tone_event.kind == V34_PHASE2_TONE_STARTED) {
		probe->phase =
			V34_PHASE2_INITIAL_PROBE_CALLER_TONE_PAIR_ACTIVE;
		event->kind =
			V34_PHASE2_INITIAL_PROBE_EVENT_TONE_PAIR_ACTIVE;
	} else if (probe->tone_wait_samples ==
		   V34_PHASE2_INITIAL_PROBE_TONE_WAIT_SAMPLES) {
		fail_probe(
			probe,
			V34_PHASE2_INITIAL_PROBE_REMOTE_TONE_TIMEOUT,
			event);
	}
	return true;
}

enum v34_phase2_initial_probe_result
v34_phase2_initial_probe_sample(
	struct v34_phase2_initial_probe *probe,
	int16_t received_sample, int16_t *transmit_sample,
	struct v34_phase2_initial_probe_event *event)
{
	struct v34_phase2_initial_probe next;
	struct v34_phase2_initial_probe_event emitted = { 0 };
	int16_t generated = 0;
	bool accepted = true;

	if (!v34_phase2_initial_probe_is_valid(probe) ||
	    transmit_sample == NULL || event == NULL)
		return V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	next = *probe;
	switch (next.phase) {
	case V34_PHASE2_INITIAL_PROBE_CALLER_RECEIVING:
		accepted = receive_remote_probe(
			&next, received_sample, &generated, &emitted);
		break;
	case V34_PHASE2_INITIAL_PROBE_CALLER_TONE_B:
	case V34_PHASE2_INITIAL_PROBE_CALLER_TONE_PAIR_ACTIVE:
		accepted = continue_caller_tone(
			&next, received_sample, &generated, &emitted);
		break;
	case V34_PHASE2_INITIAL_PROBE_ANSWERER_TRANSMITTING:
		accepted = transmit_local_probe(
			&next, received_sample, &generated, &emitted);
		break;
	case V34_PHASE2_INITIAL_PROBE_ANSWERER_TONE_A:
		accepted = observe_remote_tone(
				   &next, received_sample, &emitted) &&
			   generate_local_tone(&next, &generated);
		break;
	case V34_PHASE2_INITIAL_PROBE_FAILED:
		break;
	}
	if (!accepted || !v34_phase2_initial_probe_is_valid(&next))
		return V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	*probe = next;
	*transmit_sample = generated;
	*event = emitted;
	return V34_PHASE2_INITIAL_PROBE_OK;
}

enum v34_phase2_initial_probe_result
v34_phase2_initial_probe_measure(
	const struct v34_phase2_initial_probe *probe,
	struct v34_phase2_probe_receiver_measurement *measurement)
{
	if (!v34_phase2_initial_probe_is_valid(probe) ||
	    local_role(probe) != V34_CALLING_STATION ||
	    measurement == NULL ||
	    v34_phase2_probe_acquisition_measure(
		    &probe->path.receive, measurement) !=
		    V34_PHASE2_PROBE_ACQUISITION_OK)
		return V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	return V34_PHASE2_INITIAL_PROBE_OK;
}

const char *v34_phase2_initial_probe_result_name(
	enum v34_phase2_initial_probe_result result)
{
	switch (result) {
	case V34_PHASE2_INITIAL_PROBE_OK:
		return "ok";
	case V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
