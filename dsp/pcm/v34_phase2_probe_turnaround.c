// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase2_probe_turnaround.h"

#include <stddef.h>

bool v34_phase2_probe_turnaround_is_valid(
	const struct v34_phase2_probe_turnaround *turnaround)
{
	return turnaround != NULL &&
	       v34_phase2_initial_probe_is_valid(
		       &turnaround->initial_probe) &&
	       phase2_probe_turnaround_is_valid(
		       &turnaround->control,
		       &turnaround->initial_probe.control,
		       &turnaround->initial_probe.ranging.info0_exchange
			.transport);
}

enum phase2_probe_turnaround_result
v34_phase2_probe_turnaround_init(
	struct v34_phase2_probe_turnaround *turnaround,
	const struct v34_phase2_initial_probe *completed_probe)
{
	struct v34_phase2_probe_turnaround initialized;

	if (turnaround == NULL ||
	    !v34_phase2_initial_probe_is_valid(completed_probe))
		return PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT;
	initialized.initial_probe = *completed_probe;
	if (phase2_probe_turnaround_init(
		    &initialized.control,
		    &initialized.initial_probe.control,
		    &initialized.initial_probe.ranging.info0_exchange.transport) !=
		    PHASE2_PROBE_TURNAROUND_OK ||
	    !v34_phase2_probe_turnaround_is_valid(&initialized))
		return PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT;
	*turnaround = initialized;
	return PHASE2_PROBE_TURNAROUND_OK;
}

enum phase2_probe_turnaround_result
v34_phase2_probe_turnaround_sample(
	struct v34_phase2_probe_turnaround *turnaround,
	int16_t received_sample, int16_t *transmit_sample,
	struct phase2_probe_turnaround_event *event)
{
	struct v34_phase2_probe_turnaround next;
	struct phase2_probe_turnaround_event emitted;
	struct v34_phase2_probe_acquisition_event concurrent;
	int16_t generated;

	if (!v34_phase2_probe_turnaround_is_valid(turnaround) ||
	    transmit_sample == NULL || event == NULL)
		return PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT;
	next = *turnaround;
	if (v34_phase2_initial_probe_sample_concurrent_owned(
		    &next.initial_probe, received_sample, &concurrent) !=
		    PHASE2_INITIAL_PROBE_OK ||
	    phase2_probe_turnaround_sample_owned(
		    &next.control,
		    &next.initial_probe.ranging.info0_exchange.transport,
		    received_sample, &generated, &emitted) !=
		    PHASE2_PROBE_TURNAROUND_OK ||
	    !v34_phase2_probe_turnaround_is_valid(&next))
		return PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT;
	*turnaround = next;
	*transmit_sample = generated;
	*event = emitted;
	return PHASE2_PROBE_TURNAROUND_OK;
}
