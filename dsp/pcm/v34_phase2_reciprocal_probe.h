// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE2_RECIPROCAL_PROBE_H
#define V34_PHASE2_RECIPROCAL_PROBE_H

#include "phase2_reciprocal_probe.h"
#include "v34_phase2_probe_turnaround.h"

#include <stdbool.h>
#include <stdint.h>

struct v34_phase2_reciprocal_probe {
	struct v34_phase2_probe_turnaround turnaround;
	struct phase2_reciprocal_probe control;
};

/* Continue from a role-appropriate probe-turnaround readiness event. The
 * caller begins L1 on the following sample; the answerer begins bounded probe
 * acquisition on the following received sample. */
enum phase2_reciprocal_probe_result
v34_phase2_reciprocal_probe_init(
	struct v34_phase2_reciprocal_probe *probe,
	const struct v34_phase2_probe_turnaround
		*completed_turnaround);
bool v34_phase2_reciprocal_probe_is_valid(
	const struct v34_phase2_reciprocal_probe *probe);

/* One call consumes and produces one 8 kHz line sample. The answerer starts
 * Tone A immediately after completing its fixed probe measurement. The caller
 * ends L2 when that tone is acquired, leaving the following sample to the
 * INFO1 exchange owner. Terminal caller and failure states remain silent. */
enum phase2_reciprocal_probe_result
v34_phase2_reciprocal_probe_sample(
	struct v34_phase2_reciprocal_probe *probe,
	int16_t received_sample, int16_t *transmit_sample,
	struct phase2_reciprocal_probe_event *event);

enum phase2_reciprocal_probe_result
v34_phase2_reciprocal_probe_measure(
	const struct v34_phase2_reciprocal_probe *probe,
	struct v34_phase2_probe_receiver_measurement *measurement);

#endif
