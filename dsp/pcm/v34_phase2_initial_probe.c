// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2_initial_probe.h"

#include <stddef.h>

bool v34_phase2_initial_probe_is_valid(
	const struct v34_phase2_initial_probe *probe)
{
	return probe != NULL &&
	       v34_phase2_ranging_is_valid(&probe->ranging) &&
	       probe->ranging.control.phase == PHASE2_RANGING_PROBE_READY &&
	       phase2_initial_probe_is_valid(
		       &probe->control,
		       &probe->ranging.info0_exchange.transport);
}

enum v34_phase2_initial_probe_result v34_phase2_initial_probe_init(
	struct v34_phase2_initial_probe *probe,
	const struct v34_phase2_ranging *completed_ranging)
{
	struct v34_phase2_initial_probe initialized;

	if (probe == NULL ||
	    !v34_phase2_ranging_is_valid(completed_ranging) ||
	    completed_ranging->control.phase != PHASE2_RANGING_PROBE_READY)
		return V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	initialized.ranging = *completed_ranging;
	if (phase2_initial_probe_init(
		    &initialized.control,
		    &initialized.ranging.info0_exchange.transport,
		    initialized.ranging.control.round_trip_delay_samples) !=
		    PHASE2_INITIAL_PROBE_OK ||
	    !v34_phase2_initial_probe_is_valid(&initialized))
		return V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	*probe = initialized;
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
	int16_t generated;

	if (!v34_phase2_initial_probe_is_valid(probe) ||
	    transmit_sample == NULL || event == NULL)
		return V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	next = *probe;
	if (phase2_initial_probe_sample(
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
	*probe = next;
	*transmit_sample = generated;
	*event = emitted;
	return V34_PHASE2_INITIAL_PROBE_OK;
}

enum v34_phase2_initial_probe_result v34_phase2_initial_probe_measure(
	const struct v34_phase2_initial_probe *probe,
	struct v34_phase2_probe_receiver_measurement *measurement)
{
	if (!v34_phase2_initial_probe_is_valid(probe) ||
	    phase2_initial_probe_measure(
		    &probe->control,
		    &probe->ranging.info0_exchange.transport,
		    measurement) != PHASE2_INITIAL_PROBE_OK)
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
