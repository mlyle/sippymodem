// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE2_PROBE_TURNAROUND_H
#define V90_PHASE2_PROBE_TURNAROUND_H

#include "phase2_probe_turnaround.h"
#include "v90_phase2_initial_probe.h"

#include <stdbool.h>
#include <stdint.h>

struct v90_phase2_probe_turnaround {
	struct v90_phase2_initial_probe initial_probe;
	struct phase2_probe_turnaround control;
};

/* Continue after the first role-appropriate L1/L2 probe. The analog modem
 * emits Tone A and becomes the reciprocal-probe receiver; the digital modem
 * returns the Tone B reversal and becomes the reciprocal-probe transmitter. */
enum phase2_probe_turnaround_result v90_phase2_probe_turnaround_init(
	struct v90_phase2_probe_turnaround *turnaround,
	const struct v90_phase2_initial_probe *completed_probe);
bool v90_phase2_probe_turnaround_is_valid(
	const struct v90_phase2_probe_turnaround *turnaround);

enum phase2_probe_turnaround_result v90_phase2_probe_turnaround_sample(
	struct v90_phase2_probe_turnaround *turnaround,
	int16_t received_sample, int16_t *transmit_sample,
	struct phase2_probe_turnaround_event *event);

#endif
