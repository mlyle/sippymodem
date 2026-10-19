// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE2_INITIAL_PROBE_H
#define V34_PHASE2_INITIAL_PROBE_H

#include "phase2_initial_probe.h"
#include "v34_phase2_ranging.h"

#include <stdbool.h>
#include <stdint.h>

struct v34_phase2_initial_probe {
	struct v34_phase2_ranging ranging;
	struct phase2_initial_probe control;
	struct v34_phase2_probe_acquisition concurrent_receive;
	bool concurrent_receive_enabled;
};

/* Continue from a ranging controller after it emits its role-appropriate
 * probe-ready event. The completed state is copied so the tone phases, remote
 * detector state, and measured round-trip delay remain coherent. The answerer
 * begins L1 on the next sample; the caller begins bounded onset acquisition on
 * the next received sample. Concurrent reception is valid only for Signal A:
 * retraining may retain a probe edge identified during ranging, while an
 * initial call may arm one echo-safe synchronization capture. */
enum phase2_initial_probe_result
v34_phase2_initial_probe_init(
	struct v34_phase2_initial_probe *probe,
	const struct v34_phase2_ranging *completed_ranging,
	bool receive_concurrent_probe);
bool v34_phase2_initial_probe_is_valid(
	const struct v34_phase2_initial_probe *probe);

/* One call consumes and produces one 8 kHz line sample. After measuring the
 * remote probe, the caller emits Tone B. The answerer ends L2 when that tone is
 * acquired and emits Tone A from the following call. Both tones remain active
 * for the owner of the next reversal turn. */
enum phase2_initial_probe_result
v34_phase2_initial_probe_sample(
	struct v34_phase2_initial_probe *probe,
	int16_t received_sample, int16_t *transmit_sample,
	struct phase2_initial_probe_event *event);

/* Keep an armed overlapping-probe receiver live after the local probe has
 * handed transmission to the turnaround owner. Initial-call trials are
 * deliberately one-shot so a later copy of the local probe cannot be adopted
 * as the remote line measurement. */
enum phase2_initial_probe_result
v34_phase2_initial_probe_sample_concurrent(
	struct v34_phase2_initial_probe *probe,
	int16_t received_sample,
	struct v34_phase2_probe_acquisition_event *event);

/* Internal step shared by initial probing and turnaround. Supply a validated
 * private owner copy and a distinct event. Validate and commit the complete
 * owner only after every child succeeds; discard staged state on rejection. */
enum phase2_initial_probe_result
v34_phase2_initial_probe_sample_concurrent_owned(
	struct v34_phase2_initial_probe *probe,
	int16_t received_sample,
	struct v34_phase2_probe_acquisition_event *event);

enum phase2_initial_probe_result
v34_phase2_initial_probe_measure(
	const struct v34_phase2_initial_probe *probe,
	struct v34_phase2_probe_receiver_measurement *measurement);
enum phase2_initial_probe_result
v34_phase2_initial_probe_receive_gain(
	const struct v34_phase2_initial_probe *probe,
	int16_t *receive_gain);

#endif
