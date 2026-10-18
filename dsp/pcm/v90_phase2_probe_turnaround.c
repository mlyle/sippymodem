// SPDX-License-Identifier: BSD-3-Clause

#include "v90_phase2_probe_turnaround.h"

#include <stddef.h>

bool v90_phase2_probe_turnaround_is_valid(
	const struct v90_phase2_probe_turnaround *turnaround)
{
	return turnaround != NULL &&
	       v90_phase2_initial_probe_is_valid(&turnaround->initial_probe) &&
	       phase2_probe_turnaround_is_valid(
		       &turnaround->control,
		       &turnaround->initial_probe.control,
		       &turnaround->initial_probe.ranging.info0_exchange
			.transport);
}

enum phase2_probe_turnaround_result v90_phase2_probe_turnaround_init(
	struct v90_phase2_probe_turnaround *turnaround,
	const struct v90_phase2_initial_probe *completed_probe)
{
	struct v90_phase2_probe_turnaround initialized;

	if (turnaround == NULL ||
	    !v90_phase2_initial_probe_is_valid(completed_probe))
		return PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT;
	initialized.initial_probe = *completed_probe;
	if (phase2_probe_turnaround_init(
		    &initialized.control,
		    &initialized.initial_probe.control,
		    &initialized.initial_probe.ranging.info0_exchange.transport) !=
		    PHASE2_PROBE_TURNAROUND_OK ||
	    !v90_phase2_probe_turnaround_is_valid(&initialized))
		return PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT;
	*turnaround = initialized;
	return PHASE2_PROBE_TURNAROUND_OK;
}

enum phase2_probe_turnaround_result v90_phase2_probe_turnaround_sample(
	struct v90_phase2_probe_turnaround *turnaround,
	int16_t received_sample, int16_t *transmit_sample,
	struct phase2_probe_turnaround_event *event)
{
	if (!v90_phase2_probe_turnaround_is_valid(turnaround) ||
	    transmit_sample == NULL || event == NULL)
		return PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT;

	/* The completed probe is read-only; the shared controller commits
	 * only the turnaround control, transport and outputs on success. */
	return phase2_probe_turnaround_sample(
		&turnaround->control, &turnaround->initial_probe.control,
		&turnaround->initial_probe.ranging.info0_exchange.transport,
		received_sample, transmit_sample, event);
}
