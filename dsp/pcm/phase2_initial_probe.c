// SPDX-License-Identifier: BSD-3-Clause

#include "phase2_initial_probe.h"

#include <stddef.h>

_Static_assert(
	PHASE2_INITIAL_PROBE_TONE_WAIT_SAMPLES <= UINT16_MAX,
	"Initial-probe tone deadline must fit its sample counter");
_Static_assert(
	(unsigned int)PHASE2_INITIAL_PROBE_REMOTE_TONE_GUARD_SAMPLES <
		(unsigned int)V34_PHASE2_PROBE_L2_ECHO_LIMIT_BASE_SAMPLES,
	"Remote-tone guard must leave a bounded L2 response window");

static bool signal_is_b(const struct phase2_info0_exchange *exchange)
{
	return exchange->signal_role == PHASE2_SIGNAL_B;
}

static int16_t nominal_amplitude(
	const struct phase2_info0_exchange *exchange)
{
	const struct v34_phase2_tone_transmitter *tone =
		&exchange->tone_transmitter;

	return signal_is_b(exchange) ?
		       tone->carrier_amplitude :
		       tone->guard_amplitude;
}

static bool signal_b_path_is_valid(
	const struct phase2_initial_probe *probe)
{
	const struct v34_phase2_probe_acquisition *receive =
		&probe->path.receive;

	if (!v34_phase2_probe_acquisition_is_valid(receive))
		return false;
	switch (probe->phase) {
	case PHASE2_INITIAL_PROBE_SIGNAL_B_RECEIVING:
		return probe->tone_wait_samples == 0 &&
		       probe->failure ==
			       PHASE2_INITIAL_PROBE_NO_FAILURE &&
		       (receive->phase ==
				V34_PHASE2_PROBE_ACQUISITION_SEARCHING ||
			receive->phase ==
				V34_PHASE2_PROBE_ACQUISITION_RECEIVING);
	case PHASE2_INITIAL_PROBE_SIGNAL_B_TONE:
		return probe->tone_wait_samples > 0 &&
		       probe->tone_wait_samples <
			       PHASE2_INITIAL_PROBE_TONE_WAIT_SAMPLES &&
		       probe->failure ==
			       PHASE2_INITIAL_PROBE_NO_FAILURE &&
		       receive->phase ==
			       V34_PHASE2_PROBE_ACQUISITION_COMPLETE;
	case PHASE2_INITIAL_PROBE_SIGNAL_B_TONE_PAIR_ACTIVE:
		return probe->tone_wait_samples > 0 &&
		       probe->tone_wait_samples <=
			       PHASE2_INITIAL_PROBE_TONE_WAIT_SAMPLES &&
		       probe->failure ==
			       PHASE2_INITIAL_PROBE_NO_FAILURE &&
		       receive->phase ==
			       V34_PHASE2_PROBE_ACQUISITION_COMPLETE;
	case PHASE2_INITIAL_PROBE_FAILED:
		switch (probe->failure) {
		case PHASE2_INITIAL_PROBE_REMOTE_NOT_FOUND:
			return probe->tone_wait_samples == 0 &&
			       receive->phase ==
				       V34_PHASE2_PROBE_ACQUISITION_NOT_FOUND;
		case PHASE2_INITIAL_PROBE_REMOTE_TONE_TIMEOUT:
			return probe->tone_wait_samples ==
				       PHASE2_INITIAL_PROBE_TONE_WAIT_SAMPLES &&
			       receive->phase ==
				       V34_PHASE2_PROBE_ACQUISITION_COMPLETE;
		case PHASE2_INITIAL_PROBE_NO_FAILURE:
		case PHASE2_INITIAL_PROBE_LOCAL_TRANSMIT_LIMIT:
			return false;
		}
		return false;
	case PHASE2_INITIAL_PROBE_SIGNAL_A_TRANSMITTING:
	case PHASE2_INITIAL_PROBE_SIGNAL_A_TONE:
		return false;
	}
	return false;
}

static bool signal_a_path_is_valid(
	const struct phase2_initial_probe *probe)
{
	const struct v34_phase2_probe_sequence *transmit =
		&probe->path.transmit;

	if (!v34_phase2_probe_sequence_is_valid(transmit) ||
	    probe->tone_wait_samples != 0)
		return false;
	switch (probe->phase) {
	case PHASE2_INITIAL_PROBE_SIGNAL_A_TRANSMITTING:
		return probe->failure ==
			       PHASE2_INITIAL_PROBE_NO_FAILURE &&
		       (transmit->phase ==
				V34_PHASE2_PROBE_SEQUENCE_PHASE_L1 ||
			transmit->phase ==
				V34_PHASE2_PROBE_SEQUENCE_PHASE_L2);
	case PHASE2_INITIAL_PROBE_SIGNAL_A_TONE:
		return probe->failure ==
			       PHASE2_INITIAL_PROBE_NO_FAILURE &&
		       transmit->phase ==
			       V34_PHASE2_PROBE_SEQUENCE_PHASE_COMPLETE;
	case PHASE2_INITIAL_PROBE_FAILED:
		return probe->failure ==
			       PHASE2_INITIAL_PROBE_LOCAL_TRANSMIT_LIMIT &&
		       transmit->phase ==
			       V34_PHASE2_PROBE_SEQUENCE_PHASE_LIMIT_REACHED;
	case PHASE2_INITIAL_PROBE_SIGNAL_B_RECEIVING:
	case PHASE2_INITIAL_PROBE_SIGNAL_B_TONE:
	case PHASE2_INITIAL_PROBE_SIGNAL_B_TONE_PAIR_ACTIVE:
		return false;
	}
	return false;
}

bool phase2_initial_probe_is_valid(
	const struct phase2_initial_probe *probe,
	const struct phase2_info0_exchange *exchange)
{
	if (probe == NULL ||
	    !phase2_info0_exchange_is_valid(exchange) ||
	    (unsigned int)probe->phase >
		    PHASE2_INITIAL_PROBE_FAILED ||
	    (unsigned int)probe->failure >
		    PHASE2_INITIAL_PROBE_REMOTE_TONE_TIMEOUT)
		return false;
	return signal_is_b(exchange) ?
		       signal_b_path_is_valid(probe) :
		       signal_a_path_is_valid(probe);
}

enum phase2_initial_probe_result
phase2_initial_probe_init(
	struct phase2_initial_probe *probe,
	const struct phase2_info0_exchange *exchange,
	uint16_t round_trip_delay_samples)
{
	struct phase2_initial_probe initialized = { 0 };

	if (probe == NULL ||
	    !phase2_info0_exchange_is_valid(exchange) ||
	    exchange->transmit_phase != PHASE2_INFO0_TONE)
		return PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	if (signal_is_b(exchange)) {
		initialized.phase =
			PHASE2_INITIAL_PROBE_SIGNAL_B_RECEIVING;
		if (v34_phase2_probe_acquisition_init(
			    &initialized.path.receive) !=
		    V34_PHASE2_PROBE_ACQUISITION_OK)
			return PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	} else {
		initialized.phase =
			PHASE2_INITIAL_PROBE_SIGNAL_A_TRANSMITTING;
		if (v34_phase2_probe_sequence_init(
			    &initialized.path.transmit,
			    nominal_amplitude(exchange),
			    round_trip_delay_samples) !=
		    V34_PHASE2_PROBE_SEQUENCE_OK)
			return PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	}
	if (!phase2_initial_probe_is_valid(&initialized, exchange))
		return PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	*probe = initialized;
	return PHASE2_INITIAL_PROBE_OK;
}

static bool observe_remote_tone(
	struct phase2_info0_exchange *exchange,
	int16_t received_sample,
	struct phase2_initial_probe_event *event)
{
	return v34_phase2_tone_detector_sample(
		       &exchange->tone_detector,
		       received_sample, &event->tone_event) ==
	       V34_PHASE2_TONE_OK;
}

static bool reset_remote_tone_detector(
	struct phase2_info0_exchange *exchange)
{
	return v34_phase2_tone_detector_reset(
		       &exchange->tone_detector) ==
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
	struct phase2_initial_probe *probe,
	enum phase2_initial_probe_failure failure,
	struct phase2_initial_probe_event *event)
{
	probe->phase = PHASE2_INITIAL_PROBE_FAILED;
	probe->failure = failure;
	event->kind = PHASE2_INITIAL_PROBE_EVENT_FAILED;
	event->failure = failure;
}

static bool receive_remote_probe(
	struct phase2_initial_probe *probe,
	struct phase2_info0_exchange *exchange,
	int16_t received_sample, int16_t *transmit_sample,
	struct phase2_initial_probe_event *event)
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
		    !reset_remote_tone_detector(exchange) ||
		    !observe_remote_tone(exchange, received_sample, event) ||
		    !generate_local_tone(exchange, transmit_sample))
			return false;
		probe->phase =
			PHASE2_INITIAL_PROBE_SIGNAL_B_TONE;
		probe->tone_wait_samples = 1;
		event->kind =
			PHASE2_INITIAL_PROBE_EVENT_REMOTE_PROBE_COMPLETE;
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
			PHASE2_INITIAL_PROBE_REMOTE_NOT_FOUND,
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
	struct phase2_initial_probe *probe,
	struct phase2_info0_exchange *exchange,
	int16_t received_sample, int16_t *transmit_sample,
	struct phase2_initial_probe_event *event)
{
	struct v34_phase2_probe_sequence *transmit =
		&probe->path.transmit;
	uint32_t l2_samples = transmit->samples_emitted >=
				      V34_PHASE2_PROBE_L1_SAMPLES ?
				      transmit->samples_emitted -
					      V34_PHASE2_PROBE_L1_SAMPLES :
				      0;
	bool remote_tone_started;

	if (transmit->samples_emitted == 0 &&
	    !reset_remote_tone_detector(exchange))
		return false;
	if (transmit->phase == V34_PHASE2_PROBE_SEQUENCE_PHASE_L2 &&
	    l2_samples >= PHASE2_INITIAL_PROBE_REMOTE_TONE_GUARD_SAMPLES &&
	    !observe_remote_tone(exchange, received_sample, event))
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
			PHASE2_INITIAL_PROBE_SIGNAL_A_TONE;
		event->kind =
			PHASE2_INITIAL_PROBE_EVENT_LOCAL_PROBE_COMPLETE;
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
			PHASE2_INITIAL_PROBE_LOCAL_TRANSMIT_LIMIT,
			event);
	return true;
}

static bool continue_signal_b_tone(
	struct phase2_initial_probe *probe,
	struct phase2_info0_exchange *exchange,
	int16_t received_sample, int16_t *transmit_sample,
	struct phase2_initial_probe_event *event)
{
	if (!observe_remote_tone(exchange, received_sample, event) ||
	    !generate_local_tone(exchange, transmit_sample))
		return false;
	if (probe->phase ==
	    PHASE2_INITIAL_PROBE_SIGNAL_B_TONE_PAIR_ACTIVE)
		return true;
	probe->tone_wait_samples++;
	if (event->tone_event.kind == V34_PHASE2_TONE_STARTED) {
		probe->phase =
			PHASE2_INITIAL_PROBE_SIGNAL_B_TONE_PAIR_ACTIVE;
		event->kind =
			PHASE2_INITIAL_PROBE_EVENT_TONE_PAIR_ACTIVE;
	} else if (probe->tone_wait_samples ==
		   PHASE2_INITIAL_PROBE_TONE_WAIT_SAMPLES) {
		fail_probe(
			probe,
			PHASE2_INITIAL_PROBE_REMOTE_TONE_TIMEOUT,
			event);
	}
	return true;
}

enum phase2_initial_probe_result
phase2_initial_probe_sample(
	struct phase2_initial_probe *probe,
	struct phase2_info0_exchange *exchange,
	int16_t received_sample, int16_t *transmit_sample,
	struct phase2_initial_probe_event *event)
{
	struct phase2_initial_probe next;
	struct phase2_info0_exchange next_exchange;
	struct phase2_initial_probe_event emitted = { 0 };
	int16_t generated = 0;
	bool accepted = true;

	if (!phase2_initial_probe_is_valid(probe, exchange) ||
	    transmit_sample == NULL || event == NULL)
		return PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	next = *probe;
	next_exchange = *exchange;
	switch (next.phase) {
	case PHASE2_INITIAL_PROBE_SIGNAL_B_RECEIVING:
		accepted = receive_remote_probe(
			&next, &next_exchange, received_sample,
			&generated, &emitted);
		break;
	case PHASE2_INITIAL_PROBE_SIGNAL_B_TONE:
	case PHASE2_INITIAL_PROBE_SIGNAL_B_TONE_PAIR_ACTIVE:
		accepted = continue_signal_b_tone(
			&next, &next_exchange, received_sample,
			&generated, &emitted);
		break;
	case PHASE2_INITIAL_PROBE_SIGNAL_A_TRANSMITTING:
		accepted = transmit_local_probe(
			&next, &next_exchange, received_sample,
			&generated, &emitted);
		break;
	case PHASE2_INITIAL_PROBE_SIGNAL_A_TONE:
		accepted = observe_remote_tone(
				   &next_exchange, received_sample, &emitted) &&
			   generate_local_tone(&next_exchange, &generated);
		break;
	case PHASE2_INITIAL_PROBE_FAILED:
		break;
	}
	if (!accepted ||
	    !phase2_initial_probe_is_valid(&next, &next_exchange))
		return PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	*probe = next;
	*exchange = next_exchange;
	*transmit_sample = generated;
	*event = emitted;
	return PHASE2_INITIAL_PROBE_OK;
}

enum phase2_initial_probe_result
phase2_initial_probe_measure(
	const struct phase2_initial_probe *probe,
	const struct phase2_info0_exchange *exchange,
	struct v34_phase2_probe_receiver_measurement *measurement)
{
	if (!phase2_initial_probe_is_valid(probe, exchange) ||
	    !signal_is_b(exchange) ||
	    measurement == NULL ||
	    v34_phase2_probe_acquisition_measure(
		    &probe->path.receive, measurement) !=
		    V34_PHASE2_PROBE_ACQUISITION_OK)
		return PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	return PHASE2_INITIAL_PROBE_OK;
}

const char *phase2_initial_probe_result_name(
	enum phase2_initial_probe_result result)
{
	switch (result) {
	case PHASE2_INITIAL_PROBE_OK:
		return "ok";
	case PHASE2_INITIAL_PROBE_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
