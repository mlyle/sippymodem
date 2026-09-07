// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2_initial_probe.h"

#include <stddef.h>

static bool adopt_retrain_probe_prefix(
	struct v34_phase2_initial_probe *probe)
{
	const struct v34_phase2_retrain_probe *trial =
		&probe->ranging.retrain_probe;
	struct v34_phase2_probe_acquisition *receive;
	struct v34_phase2_probe_acquisition_event event = { 0 };

	if (trial->phase != V34_PHASE2_RETRAIN_PROBE_ACQUIRED)
		return true;
	if (probe->ranging.info0_exchange.transport.signal_role ==
	    PHASE2_SIGNAL_B)
		receive = &probe->control.path.receive;
	else if (probe->concurrent_receive_enabled)
		receive = &probe->concurrent_receive;
	else
		return false;
	for (size_t index = 0;
	     index < V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES;
	     ++index)
		if (v34_phase2_probe_acquisition_sample(
			    receive, trial->synchronizer.capture[index],
			    false, &event) !=
		    V34_PHASE2_PROBE_ACQUISITION_OK)
			return false;
	return event.kind ==
		       V34_PHASE2_PROBE_ACQUISITION_EVENT_ACQUIRED &&
	       receive->phase ==
		       V34_PHASE2_PROBE_ACQUISITION_RECEIVING;
}

bool v34_phase2_initial_probe_is_valid(
	const struct v34_phase2_initial_probe *probe)
{
	if (probe == NULL || !v34_phase2_ranging_is_valid(&probe->ranging) ||
	    probe->ranging.control.phase != PHASE2_RANGING_PROBE_READY ||
	    !phase2_initial_probe_is_valid(
		    &probe->control,
		    &probe->ranging.info0_exchange.transport))
		return false;
	return !probe->concurrent_receive_enabled ||
	       (probe->ranging.info0_exchange.transport.signal_role ==
			PHASE2_SIGNAL_A &&
		v34_phase2_probe_acquisition_is_valid(
			&probe->concurrent_receive));
}

enum v34_phase2_initial_probe_result v34_phase2_initial_probe_init(
	struct v34_phase2_initial_probe *probe,
	const struct v34_phase2_ranging *completed_ranging,
	bool receive_concurrent_probe)
{
	struct v34_phase2_initial_probe initialized = {
		.concurrent_receive_enabled = receive_concurrent_probe,
	};

	if (probe == NULL ||
	    !v34_phase2_ranging_is_valid(completed_ranging) ||
	    completed_ranging->control.phase != PHASE2_RANGING_PROBE_READY ||
	    (receive_concurrent_probe &&
	     completed_ranging->info0_exchange.transport.signal_role !=
		     PHASE2_SIGNAL_A))
		return V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	initialized.ranging = *completed_ranging;
	if (phase2_initial_probe_init(
		    &initialized.control,
		    &initialized.ranging.info0_exchange.transport,
		    &initialized.ranging.control) !=
		    PHASE2_INITIAL_PROBE_OK ||
	    (receive_concurrent_probe &&
	     v34_phase2_probe_acquisition_init(
		     &initialized.concurrent_receive) !=
		     V34_PHASE2_PROBE_ACQUISITION_OK) ||
	    !adopt_retrain_probe_prefix(&initialized) ||
	    !v34_phase2_initial_probe_is_valid(&initialized))
		return V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	*probe = initialized;
	return V34_PHASE2_INITIAL_PROBE_OK;
}

enum v34_phase2_initial_probe_result
v34_phase2_initial_probe_sample_concurrent(
	struct v34_phase2_initial_probe *probe, int16_t received_sample,
	struct v34_phase2_probe_acquisition_event *event)
{
	struct v34_phase2_initial_probe next;
	struct v34_phase2_probe_acquisition_event emitted = { 0 };
	struct v34_phase2_probe_acquisition *receive =
		&next.concurrent_receive;
	bool finish_l2;

	if (!v34_phase2_initial_probe_is_valid(probe) || event == NULL)
		return V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	next = *probe;
	if (!probe->concurrent_receive_enabled ||
	    probe->concurrent_receive.phase ==
		    V34_PHASE2_PROBE_ACQUISITION_COMPLETE) {
		*event = emitted;
		return V34_PHASE2_INITIAL_PROBE_OK;
	}
	finish_l2 = receive->phase ==
			    V34_PHASE2_PROBE_ACQUISITION_RECEIVING &&
		    receive->receiver.phase ==
			    V34_PHASE2_PROBE_RECEIVER_WAIT_L2_END;
	if (v34_phase2_probe_acquisition_sample(
		    receive, received_sample, finish_l2, &emitted) !=
	    V34_PHASE2_PROBE_ACQUISITION_OK)
		return V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	if (emitted.kind == V34_PHASE2_PROBE_ACQUISITION_EVENT_NOT_FOUND ||
	    emitted.kind == V34_PHASE2_PROBE_ACQUISITION_EVENT_LIMIT_REACHED) {
		if (!next.ranging.retraining) {
			next.concurrent_receive_enabled = false;
		} else {
			if (v34_phase2_probe_acquisition_init(receive) !=
			    V34_PHASE2_PROBE_ACQUISITION_OK)
				return V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
			emitted =
				(struct v34_phase2_probe_acquisition_event){ 0 };
		}
	}
	if (!v34_phase2_initial_probe_is_valid(&next))
		return V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	*probe = next;
	*event = emitted;
	return V34_PHASE2_INITIAL_PROBE_OK;
}

enum v34_phase2_initial_probe_result v34_phase2_initial_probe_sample(
	struct v34_phase2_initial_probe *probe,
	int16_t received_sample, int16_t *transmit_sample,
	struct v34_phase2_initial_probe_event *event)
{
	struct v34_phase2_initial_probe next;
	struct phase2_initial_probe_event common;
	struct v34_phase2_initial_probe_event emitted;
	struct v34_phase2_probe_acquisition_event concurrent;
	int16_t generated;

	if (!v34_phase2_initial_probe_is_valid(probe) ||
	    transmit_sample == NULL || event == NULL)
		return V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	next = *probe;
	if (v34_phase2_initial_probe_sample_concurrent(
		    &next, received_sample, &concurrent) !=
		    V34_PHASE2_INITIAL_PROBE_OK ||
	    phase2_initial_probe_sample(
		    &next.control,
		    &next.ranging.info0_exchange.transport,
		    received_sample, &generated, &common) !=
		    PHASE2_INITIAL_PROBE_OK ||
	    !v34_phase2_initial_probe_is_valid(&next))
		return V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	emitted = (struct v34_phase2_initial_probe_event){
		.receive_event = common.receive_event,
		.transmit_event = common.transmit_event,
		.tone_event = common.tone_event,
		.kind = common.kind,
		.failure = common.failure,
	};
	if (next.concurrent_receive_enabled)
		emitted.receive_event = concurrent;
	*probe = next;
	*transmit_sample = generated;
	*event = emitted;
	return V34_PHASE2_INITIAL_PROBE_OK;
}

enum v34_phase2_initial_probe_result v34_phase2_initial_probe_measure(
	const struct v34_phase2_initial_probe *probe,
	struct v34_phase2_probe_receiver_measurement *measurement)
{
	const struct phase2_info0_exchange *transport;
	const struct v34_phase2_probe_acquisition *receive;

	if (!v34_phase2_initial_probe_is_valid(probe) || measurement == NULL)
		return V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	transport = &probe->ranging.info0_exchange.transport;
	if (transport->signal_role == PHASE2_SIGNAL_B)
		receive = &probe->control.path.receive;
	else if (probe->concurrent_receive_enabled)
		receive = &probe->concurrent_receive;
	else
		return V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	if (v34_phase2_probe_acquisition_measure(receive, measurement) !=
	    V34_PHASE2_PROBE_ACQUISITION_OK)
		return V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	return V34_PHASE2_INITIAL_PROBE_OK;
}

enum v34_phase2_initial_probe_result
v34_phase2_initial_probe_receive_gain(
	const struct v34_phase2_initial_probe *probe,
	int16_t *receive_gain)
{
	const struct phase2_info0_exchange *transport;
	const struct v34_phase2_probe_acquisition *receive;

	if (!v34_phase2_initial_probe_is_valid(probe) || receive_gain == NULL)
		return V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	transport = &probe->ranging.info0_exchange.transport;
	if (transport->signal_role == PHASE2_SIGNAL_B)
		receive = &probe->control.path.receive;
	else if (probe->concurrent_receive_enabled)
		receive = &probe->concurrent_receive;
	else
		return V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	if (v34_phase2_probe_acquisition_receive_gain(
		    receive, receive_gain) !=
	    V34_PHASE2_PROBE_ACQUISITION_OK)
		return V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	return V34_PHASE2_INITIAL_PROBE_OK;
}
