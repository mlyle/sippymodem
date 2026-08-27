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

enum v34_phase2_probe_turnaround_result
v34_phase2_probe_turnaround_init(
	struct v34_phase2_probe_turnaround *turnaround,
	const struct v34_phase2_initial_probe *completed_probe)
{
	struct v34_phase2_probe_turnaround initialized;

	if (turnaround == NULL ||
	    !v34_phase2_initial_probe_is_valid(completed_probe))
		return V34_PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT;
	initialized.initial_probe = *completed_probe;
	if (phase2_probe_turnaround_init(
		    &initialized.control,
		    &initialized.initial_probe.control,
		    &initialized.initial_probe.ranging.info0_exchange.transport) !=
		    PHASE2_PROBE_TURNAROUND_OK ||
	    !v34_phase2_probe_turnaround_is_valid(&initialized))
		return V34_PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT;
	*turnaround = initialized;
	return V34_PHASE2_PROBE_TURNAROUND_OK;
}

enum v34_phase2_probe_turnaround_result
v34_phase2_probe_turnaround_sample(
	struct v34_phase2_probe_turnaround *turnaround,
	int16_t received_sample, int16_t *transmit_sample,
	struct v34_phase2_probe_turnaround_event *event)
{
	struct v34_phase2_probe_turnaround next;
	struct phase2_probe_turnaround_event common;
	struct v34_phase2_probe_turnaround_event emitted;
	int16_t generated;

	if (!v34_phase2_probe_turnaround_is_valid(turnaround) ||
	    transmit_sample == NULL || event == NULL)
		return V34_PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT;
	next = *turnaround;
	if (phase2_probe_turnaround_sample(
		    &next.control, &next.initial_probe.control,
		    &next.initial_probe.ranging.info0_exchange.transport,
		    received_sample, &generated, &common) !=
		    PHASE2_PROBE_TURNAROUND_OK ||
	    !v34_phase2_probe_turnaround_is_valid(&next))
		return V34_PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT;
	emitted = (struct v34_phase2_probe_turnaround_event){
		.tone_event = common.tone_event,
		.kind = common.kind,
		.failure = common.failure,
	};
	*turnaround = next;
	*transmit_sample = generated;
	*event = emitted;
	return V34_PHASE2_PROBE_TURNAROUND_OK;
}
