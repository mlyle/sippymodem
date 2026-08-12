// SPDX-License-Identifier: BSD-3-Clause

#include "phase2_reciprocal_probe.h"

#include <stddef.h>

static bool signal_is_b(const struct phase2_info0_exchange *exchange)
{
	return exchange->signal_role == PHASE2_SIGNAL_B;
}

static bool completed_turnaround_matches_role(
	const struct phase2_probe_turnaround *turnaround,
	const struct phase2_info0_exchange *exchange)
{
	return signal_is_b(exchange) ?
		       turnaround->phase ==
			       PHASE2_PROBE_TURNAROUND_SIGNAL_B_PROBE_READY :
		       turnaround->phase ==
			       PHASE2_PROBE_TURNAROUND_SIGNAL_A_PROBE_READY;
}

static bool signal_b_path_is_valid(
	const struct phase2_reciprocal_probe *probe)
{
	const struct v34_phase2_probe_sequence *transmit =
		&probe->path.transmit;

	if (!v34_phase2_probe_sequence_is_valid(transmit))
		return false;
	switch (probe->phase) {
	case PHASE2_RECIPROCAL_PROBE_SIGNAL_B_TRANSMITTING:
		return probe->failure ==
			       PHASE2_RECIPROCAL_PROBE_NO_FAILURE &&
		       (transmit->phase ==
				V34_PHASE2_PROBE_SEQUENCE_PHASE_L1 ||
			transmit->phase ==
				V34_PHASE2_PROBE_SEQUENCE_PHASE_L2);
	case PHASE2_RECIPROCAL_PROBE_SIGNAL_B_COMPLETE:
		return probe->failure ==
			       PHASE2_RECIPROCAL_PROBE_NO_FAILURE &&
		       transmit->phase ==
			       V34_PHASE2_PROBE_SEQUENCE_PHASE_COMPLETE;
	case PHASE2_RECIPROCAL_PROBE_FAILED:
		return probe->failure ==
			       PHASE2_RECIPROCAL_PROBE_LOCAL_TRANSMIT_LIMIT &&
		       transmit->phase ==
			       V34_PHASE2_PROBE_SEQUENCE_PHASE_LIMIT_REACHED;
	case PHASE2_RECIPROCAL_PROBE_SIGNAL_A_RECEIVING:
	case PHASE2_RECIPROCAL_PROBE_SIGNAL_A_TONE:
		return false;
	}
	return false;
}

static bool signal_a_path_is_valid(
	const struct phase2_reciprocal_probe *probe)
{
	const struct v34_phase2_probe_acquisition *receive =
		&probe->path.receive;

	if (!v34_phase2_probe_acquisition_is_valid(receive))
		return false;
	switch (probe->phase) {
	case PHASE2_RECIPROCAL_PROBE_SIGNAL_A_RECEIVING:
		return probe->failure ==
			       PHASE2_RECIPROCAL_PROBE_NO_FAILURE &&
		       (receive->phase ==
				V34_PHASE2_PROBE_ACQUISITION_SEARCHING ||
			receive->phase ==
				V34_PHASE2_PROBE_ACQUISITION_RECEIVING);
	case PHASE2_RECIPROCAL_PROBE_SIGNAL_A_TONE:
		return probe->failure ==
			       PHASE2_RECIPROCAL_PROBE_NO_FAILURE &&
		       receive->phase ==
			       V34_PHASE2_PROBE_ACQUISITION_COMPLETE;
	case PHASE2_RECIPROCAL_PROBE_FAILED:
		return probe->failure ==
			       PHASE2_RECIPROCAL_PROBE_REMOTE_NOT_FOUND &&
		       receive->phase ==
			       V34_PHASE2_PROBE_ACQUISITION_NOT_FOUND;
	case PHASE2_RECIPROCAL_PROBE_SIGNAL_B_TRANSMITTING:
	case PHASE2_RECIPROCAL_PROBE_SIGNAL_B_COMPLETE:
		return false;
	}
	return false;
}

bool phase2_reciprocal_probe_is_valid(
	const struct phase2_reciprocal_probe *probe,
	const struct phase2_probe_turnaround *completed_turnaround,
	const struct phase2_initial_probe *completed_initial_probe,
	const struct phase2_info0_exchange *exchange)
{
	if (probe == NULL ||
	    !phase2_probe_turnaround_is_valid(
		    completed_turnaround, completed_initial_probe, exchange) ||
	    !completed_turnaround_matches_role(
		    completed_turnaround, exchange) ||
	    (unsigned int)probe->phase >
		    PHASE2_RECIPROCAL_PROBE_FAILED ||
	    (unsigned int)probe->failure >
		    PHASE2_RECIPROCAL_PROBE_LOCAL_TRANSMIT_LIMIT)
		return false;
	if (signal_is_b(exchange))
		return probe->signal_a_reversal_elapsed_samples == 0 &&
		       signal_b_path_is_valid(probe);
	return probe->signal_a_reversal_elapsed_samples >=
		       completed_turnaround
			       ->signal_a_reversal_elapsed_samples &&
	       signal_a_path_is_valid(probe);
}

static int16_t nominal_amplitude(
	const struct phase2_info0_exchange *exchange)
{
	const struct v34_phase2_tone_transmitter *tone =
		&exchange->tone_transmitter;

	return signal_is_b(exchange) ?
		       tone->carrier_amplitude : tone->guard_amplitude;
}

enum phase2_reciprocal_probe_result phase2_reciprocal_probe_init(
	struct phase2_reciprocal_probe *probe,
	const struct phase2_probe_turnaround *completed_turnaround,
	const struct phase2_initial_probe *completed_initial_probe,
	struct phase2_info0_exchange *exchange,
	uint16_t round_trip_delay_samples)
{
	struct phase2_reciprocal_probe initialized = { 0 };
	struct phase2_info0_exchange next_exchange;

	if (probe == NULL || exchange == NULL ||
	    !phase2_probe_turnaround_is_valid(
		    completed_turnaround, completed_initial_probe, exchange) ||
	    !completed_turnaround_matches_role(completed_turnaround, exchange))
		return PHASE2_RECIPROCAL_PROBE_INVALID_ARGUMENT;
	next_exchange = *exchange;
	if (signal_is_b(&next_exchange)) {
		initialized.phase =
			PHASE2_RECIPROCAL_PROBE_SIGNAL_B_TRANSMITTING;
		if (v34_phase2_tone_detector_reset(
			    &next_exchange.tone_detector) !=
			    V34_PHASE2_TONE_OK ||
		    v34_phase2_probe_sequence_init(
			    &initialized.path.transmit,
			    nominal_amplitude(&next_exchange),
			    round_trip_delay_samples) !=
			    V34_PHASE2_PROBE_SEQUENCE_OK)
			return PHASE2_RECIPROCAL_PROBE_INVALID_ARGUMENT;
	} else {
		initialized.signal_a_reversal_elapsed_samples =
			completed_turnaround
				->signal_a_reversal_elapsed_samples;
		initialized.phase =
			PHASE2_RECIPROCAL_PROBE_SIGNAL_A_RECEIVING;
		if (v34_phase2_probe_acquisition_init(
			    &initialized.path.receive) !=
		    V34_PHASE2_PROBE_ACQUISITION_OK)
			return PHASE2_RECIPROCAL_PROBE_INVALID_ARGUMENT;
	}
	if (!phase2_reciprocal_probe_is_valid(
		    &initialized, completed_turnaround,
		    completed_initial_probe, &next_exchange))
		return PHASE2_RECIPROCAL_PROBE_INVALID_ARGUMENT;
	*probe = initialized;
	*exchange = next_exchange;
	return PHASE2_RECIPROCAL_PROBE_OK;
}

static bool observe_remote_tone(
	struct phase2_info0_exchange *exchange,
	int16_t received_sample,
	struct phase2_reciprocal_probe_event *event)
{
	return v34_phase2_tone_detector_sample(
		       &exchange->tone_detector,
		       received_sample, &event->tone_event) ==
	       V34_PHASE2_TONE_OK;
}

static bool generate_local_tone(
	struct phase2_info0_exchange *exchange, int16_t *sample)
{
	return v34_phase2_tone_transmitter_sample(
		       &exchange->tone_transmitter, sample) ==
	       V34_PHASE2_TONE_OK;
}

static void fail_probe(
	struct phase2_reciprocal_probe *probe,
	enum phase2_reciprocal_probe_failure failure,
	struct phase2_reciprocal_probe_event *event)
{
	probe->phase = PHASE2_RECIPROCAL_PROBE_FAILED;
	probe->failure = failure;
	event->kind = PHASE2_RECIPROCAL_PROBE_EVENT_FAILED;
	event->failure = failure;
}

static bool transmit_local_probe(
	struct phase2_reciprocal_probe *probe,
	struct phase2_info0_exchange *exchange,
	int16_t received_sample, int16_t *transmit_sample,
	struct phase2_reciprocal_probe_event *event)
{
	struct v34_phase2_probe_sequence *transmit =
		&probe->path.transmit;
	bool remote_tone_started;

	if (!observe_remote_tone(exchange, received_sample, event))
		return false;
	remote_tone_started =
		event->tone_event.kind == V34_PHASE2_TONE_STARTED;
	if (remote_tone_started &&
	    transmit->phase == V34_PHASE2_PROBE_SEQUENCE_PHASE_L2 &&
	    transmit->samples_emitted > V34_PHASE2_PROBE_L1_SAMPLES) {
		if (v34_phase2_probe_sequence_sample(
			    transmit, true, transmit_sample,
			    &event->transmit_event) !=
			    V34_PHASE2_PROBE_SEQUENCE_OK ||
		    event->transmit_event.kind !=
			    V34_PHASE2_PROBE_SEQUENCE_EVENT_COMPLETED)
			return false;
		probe->phase = PHASE2_RECIPROCAL_PROBE_SIGNAL_B_COMPLETE;
		event->kind =
			PHASE2_RECIPROCAL_PROBE_EVENT_LOCAL_PROBE_ACKNOWLEDGED;
		return true;
	}
	if (v34_phase2_probe_sequence_sample(
		    transmit, false, transmit_sample,
		    &event->transmit_event) !=
	    V34_PHASE2_PROBE_SEQUENCE_OK)
		return false;
	if (event->transmit_event.kind ==
	    V34_PHASE2_PROBE_SEQUENCE_EVENT_LIMIT_REACHED)
		fail_probe(
			probe,
			PHASE2_RECIPROCAL_PROBE_LOCAL_TRANSMIT_LIMIT,
			event);
	return true;
}

static bool receive_remote_probe(
	struct phase2_reciprocal_probe *probe,
	struct phase2_info0_exchange *exchange,
	int16_t received_sample, int16_t *transmit_sample,
	struct phase2_reciprocal_probe_event *event)
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
		    event->receive_event.kind !=
			    V34_PHASE2_PROBE_ACQUISITION_EVENT_COMPLETED ||
		    !generate_local_tone(exchange, transmit_sample))
			return false;
		probe->phase = PHASE2_RECIPROCAL_PROBE_SIGNAL_A_TONE;
		event->kind =
			PHASE2_RECIPROCAL_PROBE_EVENT_REMOTE_PROBE_MEASURED;
		return true;
	}
	if (v34_phase2_probe_acquisition_sample(
		    receive, received_sample, false,
		    &event->receive_event) !=
	    V34_PHASE2_PROBE_ACQUISITION_OK)
		return false;
	switch (event->receive_event.kind) {
	case V34_PHASE2_PROBE_ACQUISITION_EVENT_NOT_FOUND:
		fail_probe(
			probe, PHASE2_RECIPROCAL_PROBE_REMOTE_NOT_FOUND,
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

enum phase2_reciprocal_probe_result phase2_reciprocal_probe_sample(
	struct phase2_reciprocal_probe *probe,
	const struct phase2_probe_turnaround *completed_turnaround,
	const struct phase2_initial_probe *completed_initial_probe,
	struct phase2_info0_exchange *exchange,
	int16_t received_sample, int16_t *transmit_sample,
	struct phase2_reciprocal_probe_event *event)
{
	struct phase2_reciprocal_probe next;
	struct phase2_info0_exchange next_exchange;
	struct phase2_reciprocal_probe_event emitted = { 0 };
	int16_t generated = 0;
	bool accepted = true;

	if (!phase2_reciprocal_probe_is_valid(
		    probe, completed_turnaround,
		    completed_initial_probe, exchange) ||
	    transmit_sample == NULL || event == NULL)
		return PHASE2_RECIPROCAL_PROBE_INVALID_ARGUMENT;
	next = *probe;
	next_exchange = *exchange;
	switch (next.phase) {
	case PHASE2_RECIPROCAL_PROBE_SIGNAL_B_TRANSMITTING:
		accepted = transmit_local_probe(
			&next, &next_exchange, received_sample,
			&generated, &emitted);
		break;
	case PHASE2_RECIPROCAL_PROBE_SIGNAL_A_RECEIVING:
		accepted = receive_remote_probe(
			&next, &next_exchange, received_sample,
			&generated, &emitted);
		break;
	case PHASE2_RECIPROCAL_PROBE_SIGNAL_A_TONE:
		accepted = generate_local_tone(&next_exchange, &generated);
		break;
	case PHASE2_RECIPROCAL_PROBE_SIGNAL_B_COMPLETE:
	case PHASE2_RECIPROCAL_PROBE_FAILED:
		break;
	}
	if (!signal_is_b(&next_exchange) &&
	    probe->phase != PHASE2_RECIPROCAL_PROBE_FAILED &&
	    next.signal_a_reversal_elapsed_samples != UINT32_MAX)
		next.signal_a_reversal_elapsed_samples++;
	if (!accepted ||
	    !phase2_reciprocal_probe_is_valid(
		    &next, completed_turnaround,
		    completed_initial_probe, &next_exchange))
		return PHASE2_RECIPROCAL_PROBE_INVALID_ARGUMENT;
	*probe = next;
	*exchange = next_exchange;
	*transmit_sample = generated;
	*event = emitted;
	return PHASE2_RECIPROCAL_PROBE_OK;
}

enum phase2_reciprocal_probe_result phase2_reciprocal_probe_measure(
	const struct phase2_reciprocal_probe *probe,
	const struct phase2_probe_turnaround *completed_turnaround,
	const struct phase2_initial_probe *completed_initial_probe,
	const struct phase2_info0_exchange *exchange,
	struct v34_phase2_probe_receiver_measurement *measurement)
{
	if (!phase2_reciprocal_probe_is_valid(
		    probe, completed_turnaround,
		    completed_initial_probe, exchange) ||
	    signal_is_b(exchange) ||
	    probe->phase != PHASE2_RECIPROCAL_PROBE_SIGNAL_A_TONE ||
	    measurement == NULL ||
	    v34_phase2_probe_acquisition_measure(
		    &probe->path.receive, measurement) !=
		    V34_PHASE2_PROBE_ACQUISITION_OK)
		return PHASE2_RECIPROCAL_PROBE_INVALID_ARGUMENT;
	return PHASE2_RECIPROCAL_PROBE_OK;
}

const char *phase2_reciprocal_probe_result_name(
	enum phase2_reciprocal_probe_result result)
{
	switch (result) {
	case PHASE2_RECIPROCAL_PROBE_OK:
		return "ok";
	case PHASE2_RECIPROCAL_PROBE_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
