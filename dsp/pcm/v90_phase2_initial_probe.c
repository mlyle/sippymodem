// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase2_initial_probe.h"

#include <stddef.h>

bool v90_phase2_initial_probe_is_valid(
	const struct v90_phase2_initial_probe *probe)
{
	return probe != NULL &&
	       pcm_phase2_ranging_is_valid(&probe->ranging) &&
	       probe->ranging.control.phase == PHASE2_RANGING_PROBE_READY &&
	       phase2_initial_probe_is_valid(
		       &probe->control,
		       &probe->ranging.info0_exchange.transport);
}

enum phase2_initial_probe_result v90_phase2_initial_probe_init(
	struct v90_phase2_initial_probe *probe,
	const struct pcm_phase2_ranging *completed_ranging)
{
	struct v90_phase2_initial_probe initialized;

	if (probe == NULL ||
	    !pcm_phase2_ranging_is_valid(completed_ranging) ||
	    completed_ranging->control.phase != PHASE2_RANGING_PROBE_READY)
		return PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	initialized.ranging = *completed_ranging;
	if (phase2_initial_probe_init(
		    &initialized.control,
		    &initialized.ranging.info0_exchange.transport,
		    &initialized.ranging.control) !=
		    PHASE2_INITIAL_PROBE_OK ||
	    !v90_phase2_initial_probe_is_valid(&initialized))
		return PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	*probe = initialized;
	return PHASE2_INITIAL_PROBE_OK;
}

enum phase2_initial_probe_result v90_phase2_initial_probe_sample(
	struct v90_phase2_initial_probe *probe,
	int16_t received_sample, int16_t *transmit_sample,
	struct phase2_initial_probe_event *event)
{
	struct v90_phase2_initial_probe next;
	struct phase2_initial_probe_event emitted;
	int16_t generated;

	if (!v90_phase2_initial_probe_is_valid(probe) ||
	    transmit_sample == NULL || event == NULL)
		return PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	next = *probe;
	if (phase2_initial_probe_sample(
		    &next.control,
		    &next.ranging.info0_exchange.transport,
		    received_sample, &generated, &emitted) !=
		    PHASE2_INITIAL_PROBE_OK ||
	    !v90_phase2_initial_probe_is_valid(&next))
		return PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	*probe = next;
	*transmit_sample = generated;
	*event = emitted;
	return PHASE2_INITIAL_PROBE_OK;
}

enum phase2_initial_probe_result v90_phase2_initial_probe_measure(
	const struct v90_phase2_initial_probe *probe,
	struct v34_phase2_probe_receiver_measurement *measurement)
{
	if (!v90_phase2_initial_probe_is_valid(probe))
		return PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	return phase2_initial_probe_measure(
		&probe->control,
		&probe->ranging.info0_exchange.transport,
		measurement);
}

enum phase2_initial_probe_result v90_phase2_initial_probe_receive_gain(
	const struct v90_phase2_initial_probe *probe, int16_t *receive_gain)
{
	if (!v90_phase2_initial_probe_is_valid(probe) ||
	    receive_gain == NULL ||
	    probe->ranging.info0_exchange.transport.signal_role !=
		    PHASE2_SIGNAL_B ||
	    v34_phase2_probe_acquisition_receive_gain(
		    &probe->control.path.receive, receive_gain) !=
		    V34_PHASE2_PROBE_ACQUISITION_OK)
		return PHASE2_INITIAL_PROBE_INVALID_ARGUMENT;
	return PHASE2_INITIAL_PROBE_OK;
}
