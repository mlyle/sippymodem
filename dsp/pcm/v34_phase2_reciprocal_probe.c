// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2_reciprocal_probe.h"

#include <stddef.h>

static struct phase2_info0_exchange *transport(
	struct v34_phase2_reciprocal_probe *probe)
{
	return &probe->turnaround.initial_probe.ranging.info0_exchange
			.transport;
}

static const struct phase2_info0_exchange *const_transport(
	const struct v34_phase2_reciprocal_probe *probe)
{
	return &probe->turnaround.initial_probe.ranging.info0_exchange
			.transport;
}

bool v34_phase2_reciprocal_probe_is_valid(
	const struct v34_phase2_reciprocal_probe *probe)
{
	return probe != NULL &&
	       v34_phase2_probe_turnaround_is_valid(&probe->turnaround) &&
	       phase2_reciprocal_probe_is_valid(
		       &probe->control, &probe->turnaround.control,
		       &probe->turnaround.initial_probe.control,
		       const_transport(probe));
}

enum phase2_reciprocal_probe_result
v34_phase2_reciprocal_probe_init(
	struct v34_phase2_reciprocal_probe *probe,
	const struct v34_phase2_probe_turnaround *completed_turnaround)
{
	struct v34_phase2_reciprocal_probe initialized;

	if (probe == NULL ||
	    !v34_phase2_probe_turnaround_is_valid(completed_turnaround))
		return PHASE2_RECIPROCAL_PROBE_INVALID_ARGUMENT;
	initialized.turnaround = *completed_turnaround;
	if (phase2_reciprocal_probe_init(
		    &initialized.control, &initialized.turnaround.control,
		    &initialized.turnaround.initial_probe.control,
		    transport(&initialized),
		    initialized.turnaround.initial_probe.ranging.control
			    .round_trip_delay_samples) !=
		    PHASE2_RECIPROCAL_PROBE_OK ||
	    !v34_phase2_reciprocal_probe_is_valid(&initialized))
		return PHASE2_RECIPROCAL_PROBE_INVALID_ARGUMENT;
	*probe = initialized;
	return PHASE2_RECIPROCAL_PROBE_OK;
}

enum phase2_reciprocal_probe_result
v34_phase2_reciprocal_probe_sample(
	struct v34_phase2_reciprocal_probe *probe,
	int16_t received_sample, int16_t *transmit_sample,
	struct phase2_reciprocal_probe_event *event)
{
	struct v34_phase2_reciprocal_probe next;
	struct phase2_reciprocal_probe_event common;
	int16_t generated;

	if (!v34_phase2_reciprocal_probe_is_valid(probe) ||
	    transmit_sample == NULL || event == NULL)
		return PHASE2_RECIPROCAL_PROBE_INVALID_ARGUMENT;
	next = *probe;
	if (phase2_reciprocal_probe_sample(
		    &next.control, &next.turnaround.control,
		    &next.turnaround.initial_probe.control,
		    transport(&next), received_sample, &generated, &common) !=
		    PHASE2_RECIPROCAL_PROBE_OK ||
	    !v34_phase2_reciprocal_probe_is_valid(&next))
		return PHASE2_RECIPROCAL_PROBE_INVALID_ARGUMENT;
	*probe = next;
	*transmit_sample = generated;
	*event = common;
	return PHASE2_RECIPROCAL_PROBE_OK;
}

enum phase2_reciprocal_probe_result
v34_phase2_reciprocal_probe_measure(
	const struct v34_phase2_reciprocal_probe *probe,
	struct v34_phase2_probe_receiver_measurement *measurement)
{
	if (!v34_phase2_reciprocal_probe_is_valid(probe) ||
	    phase2_reciprocal_probe_measure(
		    &probe->control, &probe->turnaround.control,
		    &probe->turnaround.initial_probe.control,
		    const_transport(probe), measurement) !=
		    PHASE2_RECIPROCAL_PROBE_OK)
		return PHASE2_RECIPROCAL_PROBE_INVALID_ARGUMENT;
	return PHASE2_RECIPROCAL_PROBE_OK;
}
