// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase2_reciprocal_probe.h"

#include <stddef.h>

static struct phase2_info0_exchange *transport(
	struct v90_phase2_reciprocal_probe *probe)
{
	return &probe->turnaround.initial_probe.ranging.info0_exchange.transport;
}

static const struct phase2_info0_exchange *const_transport(
	const struct v90_phase2_reciprocal_probe *probe)
{
	return &probe->turnaround.initial_probe.ranging.info0_exchange.transport;
}

bool v90_phase2_reciprocal_probe_is_valid(
	const struct v90_phase2_reciprocal_probe *probe)
{
	return probe != NULL &&
	       v90_phase2_probe_turnaround_is_valid(&probe->turnaround) &&
	       phase2_reciprocal_probe_is_valid(
		       &probe->control, &probe->turnaround.control,
		       &probe->turnaround.initial_probe.control,
		       const_transport(probe));
}

enum phase2_reciprocal_probe_result v90_phase2_reciprocal_probe_init(
	struct v90_phase2_reciprocal_probe *probe,
	const struct v90_phase2_probe_turnaround *completed_turnaround)
{
	struct v90_phase2_reciprocal_probe initialized;

	if (probe == NULL ||
	    !v90_phase2_probe_turnaround_is_valid(completed_turnaround))
		return PHASE2_RECIPROCAL_PROBE_INVALID_ARGUMENT;
	initialized.turnaround = *completed_turnaround;
	if (phase2_reciprocal_probe_init(
		    &initialized.control, &initialized.turnaround.control,
		    &initialized.turnaround.initial_probe.control,
		    transport(&initialized),
		    initialized.turnaround.initial_probe.ranging.control
			    .round_trip_delay_samples) !=
		    PHASE2_RECIPROCAL_PROBE_OK ||
	    !v90_phase2_reciprocal_probe_is_valid(&initialized))
		return PHASE2_RECIPROCAL_PROBE_INVALID_ARGUMENT;
	*probe = initialized;
	return PHASE2_RECIPROCAL_PROBE_OK;
}

enum phase2_reciprocal_probe_result v90_phase2_reciprocal_probe_sample(
	struct v90_phase2_reciprocal_probe *probe,
	int16_t received_sample, int16_t *transmit_sample,
	struct phase2_reciprocal_probe_event *event)
{
	struct v90_phase2_reciprocal_probe next;
	struct phase2_reciprocal_probe_event emitted;
	int16_t generated;

	if (!v90_phase2_reciprocal_probe_is_valid(probe) ||
	    transmit_sample == NULL || event == NULL)
		return PHASE2_RECIPROCAL_PROBE_INVALID_ARGUMENT;
	next = *probe;
	if (phase2_reciprocal_probe_sample(
		    &next.control, &next.turnaround.control,
		    &next.turnaround.initial_probe.control,
		    transport(&next), received_sample, &generated, &emitted) !=
		    PHASE2_RECIPROCAL_PROBE_OK ||
	    !v90_phase2_reciprocal_probe_is_valid(&next))
		return PHASE2_RECIPROCAL_PROBE_INVALID_ARGUMENT;
	*probe = next;
	*transmit_sample = generated;
	*event = emitted;
	return PHASE2_RECIPROCAL_PROBE_OK;
}

enum phase2_reciprocal_probe_result v90_phase2_reciprocal_probe_measure(
	const struct v90_phase2_reciprocal_probe *probe,
	struct v34_phase2_probe_receiver_measurement *measurement)
{
	if (!v90_phase2_reciprocal_probe_is_valid(probe) ||
	    phase2_reciprocal_probe_measure(
		    &probe->control, &probe->turnaround.control,
		    &probe->turnaround.initial_probe.control,
		    const_transport(probe), measurement) !=
		    PHASE2_RECIPROCAL_PROBE_OK)
		return PHASE2_RECIPROCAL_PROBE_INVALID_ARGUMENT;
	return PHASE2_RECIPROCAL_PROBE_OK;
}

enum phase2_reciprocal_probe_result
v90_phase2_reciprocal_probe_receive_gain(
	const struct v90_phase2_reciprocal_probe *probe,
	int16_t *receive_gain)
{
	if (!v90_phase2_reciprocal_probe_is_valid(probe) ||
	    receive_gain == NULL ||
	    const_transport(probe)->signal_role != PHASE2_SIGNAL_A ||
	    probe->control.phase != PHASE2_RECIPROCAL_PROBE_SIGNAL_A_TONE ||
	    v34_phase2_probe_acquisition_receive_gain(
		    &probe->control.path.receive, receive_gain) !=
		    V34_PHASE2_PROBE_ACQUISITION_OK)
		return PHASE2_RECIPROCAL_PROBE_INVALID_ARGUMENT;
	return PHASE2_RECIPROCAL_PROBE_OK;
}
