// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2_initial_probe.h"

#include <stddef.h>

static bool adopt_probe_prefix(
	struct v34_phase2_initial_probe *probe)
{
	const struct v34_phase2_retrain_probe *trial =
		&probe->ranging.retrain_probe;
	struct v34_phase2_probe_acquisition *receive;
	struct v34_phase2_probe_acquisition_event event = { 0 };
	bool partial = trial->phase == V34_PHASE2_RETRAIN_PROBE_COLLECTING;

	if (trial->phase != V34_PHASE2_RETRAIN_PROBE_ACQUIRED && !partial)
		return true;
	if (probe->ranging.info0_exchange.transport.signal_role ==
	    PHASE2_SIGNAL_B)
		receive = &probe->control.path.receive;
	else if (probe->concurrent_receive_enabled)
		receive = &probe->concurrent_receive;
	else
		return true;
	for (size_t index = 0;
	     index < trial->synchronizer.samples_observed;
	     ++index)
		if (v34_phase2_probe_acquisition_sample(
			    receive, trial->synchronizer.capture[index],
			    false, &event) !=
		    V34_PHASE2_PROBE_ACQUISITION_OK)
			return false;
	return partial ? event.kind == V34_PHASE2_PROBE_ACQUISITION_EVENT_NONE :
	       event.kind ==
		       V34_PHASE2_PROBE_ACQUISITION_EVENT_ACQUIRED &&
	       receive->phase ==
		       V34_PHASE2_PROBE_ACQUISITION_RECEIVING;
}

static bool resolve_early_prefix(
	struct v34_phase2_initial_probe *probe,
	struct v34_phase2_probe_acquisition_event *event)
{
	struct v34_phase2_retrain_probe *trial = &probe->ranging.retrain_probe;
	struct v34_phase2_probe_acquisition *receive = &probe->concurrent_receive;
	uint16_t prefix = trial->synchronizer.samples_observed;

	if (trial->phase != V34_PHASE2_RETRAIN_PROBE_COLLECTING)
		return true;
	if (event->kind == V34_PHASE2_PROBE_ACQUISITION_EVENT_ACQUIRED) {
		trial->synchronizer = receive->synchronizer;
		trial->phase = V34_PHASE2_RETRAIN_PROBE_ACQUIRED;
	} else if (event->kind == V34_PHASE2_PROBE_ACQUISITION_EVENT_NOT_FOUND) {
		/* Keep only samples since the original local-probe boundary. This
		 * is the existing echo-safe trial, not a sliding retry window. */
		uint16_t count = V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES - prefix;

		for (uint16_t index = 0; index < count; ++index)
			trial->synchronizer.capture[index] =
				receive->synchronizer.capture[prefix + index];
		if (v34_phase2_probe_acquisition_init(receive) !=
		    V34_PHASE2_PROBE_ACQUISITION_OK)
			return false;
		for (uint16_t index = 0; index < count; ++index)
			if (v34_phase2_probe_acquisition_sample(receive,
				    trial->synchronizer.capture[index], false, event) !=
			    V34_PHASE2_PROBE_ACQUISITION_OK)
				return false;
		*trial = (struct v34_phase2_retrain_probe){ 0 };
	}
	return true;
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

enum phase2_initial_probe_result v34_phase2_initial_probe_init(
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
		return PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
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
	    !adopt_probe_prefix(&initialized) ||
	    !v34_phase2_initial_probe_is_valid(&initialized))
		return PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	*probe = initialized;
	return PHASE2_INITIAL_PROBE_OK;
}

enum phase2_initial_probe_result
v34_phase2_initial_probe_sample_concurrent_owned(
	struct v34_phase2_initial_probe *probe, int16_t received_sample,
	struct v34_phase2_probe_acquisition_event *event)
{
	struct v34_phase2_probe_acquisition_event emitted = { 0 };
	struct v34_phase2_probe_acquisition *receive =
		&probe->concurrent_receive;
	bool finish_l2;

	if (!probe->concurrent_receive_enabled ||
	    probe->concurrent_receive.phase ==
		    V34_PHASE2_PROBE_ACQUISITION_COMPLETE) {
		*event = emitted;
		return PHASE2_INITIAL_PROBE_OK;
	}
	finish_l2 = receive->phase ==
			    V34_PHASE2_PROBE_ACQUISITION_RECEIVING &&
		    receive->receiver.phase ==
			    V34_PHASE2_PROBE_RECEIVER_WAIT_L2_END;
	if (v34_phase2_probe_acquisition_sample(
		    receive, received_sample, finish_l2, &emitted) !=
		    V34_PHASE2_PROBE_ACQUISITION_OK ||
	    !resolve_early_prefix(probe, &emitted))
		return PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	if (emitted.kind == V34_PHASE2_PROBE_ACQUISITION_EVENT_NOT_FOUND ||
	    emitted.kind == V34_PHASE2_PROBE_ACQUISITION_EVENT_LIMIT_REACHED) {
		if (!probe->ranging.retraining) {
			probe->concurrent_receive_enabled = false;
		} else {
			if (v34_phase2_probe_acquisition_init(receive) !=
			    V34_PHASE2_PROBE_ACQUISITION_OK)
				return PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
			emitted =
				(struct v34_phase2_probe_acquisition_event){ 0 };
		}
	}
	*event = emitted;
	return PHASE2_INITIAL_PROBE_OK;
}

enum phase2_initial_probe_result
v34_phase2_initial_probe_sample_concurrent(
	struct v34_phase2_initial_probe *probe, int16_t received_sample,
	struct v34_phase2_probe_acquisition_event *event)
{
	struct v34_phase2_initial_probe next;
	struct v34_phase2_probe_acquisition_event emitted;

	if (!v34_phase2_initial_probe_is_valid(probe) || event == NULL)
		return PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	next = *probe;
	if (v34_phase2_initial_probe_sample_concurrent_owned(
		    &next, received_sample, &emitted) != PHASE2_INITIAL_PROBE_OK ||
	    !v34_phase2_initial_probe_is_valid(&next))
		return PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	*probe = next;
	*event = emitted;
	return PHASE2_INITIAL_PROBE_OK;
}

enum phase2_initial_probe_result v34_phase2_initial_probe_sample(
	struct v34_phase2_initial_probe *probe,
	int16_t received_sample, int16_t *transmit_sample,
	struct phase2_initial_probe_event *event)
{
	struct v34_phase2_initial_probe next;
	struct phase2_initial_probe_event emitted;
	struct v34_phase2_probe_acquisition_event concurrent;
	int16_t generated;

	if (!v34_phase2_initial_probe_is_valid(probe) ||
	    transmit_sample == NULL || event == NULL)
		return PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	next = *probe;
	if (v34_phase2_initial_probe_sample_concurrent_owned(
		    &next, received_sample, &concurrent) !=
		    PHASE2_INITIAL_PROBE_OK ||
	    phase2_initial_probe_sample(
		    &next.control,
		    &next.ranging.info0_exchange.transport,
		    !next.ranging.retraining, received_sample, &generated, &emitted) !=
		    PHASE2_INITIAL_PROBE_OK ||
	    !v34_phase2_initial_probe_is_valid(&next))
		return PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	if (next.concurrent_receive_enabled)
		emitted.receive_event = concurrent;
	*probe = next;
	*transmit_sample = generated;
	*event = emitted;
	return PHASE2_INITIAL_PROBE_OK;
}

static const struct v34_phase2_probe_acquisition *receive_acquisition(
	const struct v34_phase2_initial_probe *probe)
{
	if (!v34_phase2_initial_probe_is_valid(probe))
		return NULL;
	if (probe->ranging.info0_exchange.transport.signal_role == PHASE2_SIGNAL_B)
		return &probe->control.path.receive;
	return probe->concurrent_receive_enabled ? &probe->concurrent_receive : NULL;
}

enum phase2_initial_probe_result v34_phase2_initial_probe_measure(
	const struct v34_phase2_initial_probe *probe,
	struct v34_phase2_probe_receiver_measurement *measurement)
{
	const struct v34_phase2_probe_acquisition *receive = receive_acquisition(probe);

	if (receive == NULL || measurement == NULL ||
	    v34_phase2_probe_acquisition_measure(receive, measurement) !=
		    V34_PHASE2_PROBE_ACQUISITION_OK)
		return PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	return PHASE2_INITIAL_PROBE_OK;
}

enum phase2_initial_probe_result
v34_phase2_initial_probe_receive_gain(
	const struct v34_phase2_initial_probe *probe,
	int16_t *receive_gain)
{
	const struct v34_phase2_probe_acquisition *receive = receive_acquisition(probe);

	if (receive == NULL || receive_gain == NULL ||
	    v34_phase2_probe_acquisition_receive_gain(receive, receive_gain) !=
		    V34_PHASE2_PROBE_ACQUISITION_OK)
		return PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	return PHASE2_INITIAL_PROBE_OK;
}
