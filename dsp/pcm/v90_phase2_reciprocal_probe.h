// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE2_RECIPROCAL_PROBE_H
#define V90_PHASE2_RECIPROCAL_PROBE_H

#include "phase2_reciprocal_probe.h"
#include "v90_phase2_probe_turnaround.h"

#include <stdbool.h>
#include <stdint.h>

struct v90_phase2_reciprocal_probe {
	struct v90_phase2_probe_turnaround turnaround;
	struct phase2_reciprocal_probe control;
};

/* Continue from the role-appropriate turnaround handoff. The digital modem
 * transmits L1/L2; the analog modem measures it and raises Tone A. */
enum phase2_reciprocal_probe_result v90_phase2_reciprocal_probe_init(
	struct v90_phase2_reciprocal_probe *probe,
	const struct v90_phase2_probe_turnaround *completed_turnaround);
bool v90_phase2_reciprocal_probe_is_valid(
	const struct v90_phase2_reciprocal_probe *probe);

enum phase2_reciprocal_probe_result v90_phase2_reciprocal_probe_sample(
	struct v90_phase2_reciprocal_probe *probe,
	int16_t received_sample, int16_t *transmit_sample,
	struct phase2_reciprocal_probe_event *event);

enum phase2_reciprocal_probe_result v90_phase2_reciprocal_probe_measure(
	const struct v90_phase2_reciprocal_probe *probe,
	struct v34_phase2_probe_receiver_measurement *measurement);

#endif
