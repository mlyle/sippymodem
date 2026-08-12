// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE2_PROBE_TURNAROUND_H
#define V34_PHASE2_PROBE_TURNAROUND_H

#include "phase2_probe_turnaround.h"
#include "v34_phase2_initial_probe.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_phase2_probe_turnaround_result {
	V34_PHASE2_PROBE_TURNAROUND_OK,
	V34_PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT,
};

struct v34_phase2_probe_turnaround_event {
	struct v34_phase2_tone_event tone_event;
	enum phase2_probe_turnaround_event_kind kind;
	enum phase2_probe_turnaround_failure failure;
};

struct v34_phase2_probe_turnaround {
	struct v34_phase2_initial_probe initial_probe;
	struct phase2_probe_turnaround control;
};

/* Continue from a role-appropriate completed initial probe. The answerer is
 * handed off before its first Tone A sample; the caller is handed off after
 * acquiring that tone while its own Tone B remains continuous. */
enum v34_phase2_probe_turnaround_result
v34_phase2_probe_turnaround_init(
	struct v34_phase2_probe_turnaround *turnaround,
	const struct v34_phase2_initial_probe *completed_probe);
bool v34_phase2_probe_turnaround_is_valid(
	const struct v34_phase2_probe_turnaround *turnaround);

/* One call consumes and produces one sample at the 8 kHz line terminal. The
 * caller's reversal deadline compensates the tone detector's reported edge
 * latency. The transmit-ready event is emitted on the caller's final tone-tail
 * sample. Receive-ready leaves the synchronizer's bounded search prefix before
 * the expected L1 edge. The following sample belongs to the reciprocal probe
 * transmitter or receiver; this owner never generates or measures L1/L2. */
enum v34_phase2_probe_turnaround_result
v34_phase2_probe_turnaround_sample(
	struct v34_phase2_probe_turnaround *turnaround,
	int16_t received_sample, int16_t *transmit_sample,
	struct v34_phase2_probe_turnaround_event *event);

const char *v34_phase2_probe_turnaround_result_name(
	enum v34_phase2_probe_turnaround_result result);

#endif
