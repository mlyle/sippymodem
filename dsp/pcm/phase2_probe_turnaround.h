// SPDX-License-Identifier: BSD-3-Clause

#ifndef PHASE2_PROBE_TURNAROUND_H
#define PHASE2_PROBE_TURNAROUND_H

#include "phase2_initial_probe.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	PHASE2_PROBE_TURNAROUND_RESPONSE_SAMPLES =
		V34_PHASE2_SAMPLE_RATE * 40 / 1000,
	PHASE2_PROBE_TURNAROUND_INITIAL_TONE_SAMPLES =
		V34_PHASE2_SAMPLE_RATE * 50 / 1000,
	PHASE2_PROBE_TURNAROUND_TONE_TAIL_SAMPLES =
		V34_PHASE2_SAMPLE_RATE * 10 / 1000,
	/* Begin one millisecond before the expected L1 edge. The remaining
	 * synchronizer prefix tolerates the receiver timing-settling interval. */
	PHASE2_PROBE_TURNAROUND_RECEIVE_HANDOFF_LEAD_SAMPLES =
		V34_PHASE2_PROBE_SYNCHRONIZER_NOMINAL_LEADING_SAMPLES,
	PHASE2_PROBE_TURNAROUND_MAXIMUM_WAIT_SAMPLES =
		V34_PHASE2_SAMPLE_RATE * 2,
};

enum phase2_probe_turnaround_phase {
	PHASE2_PROBE_TURNAROUND_SIGNAL_B_WAIT_A_REVERSAL,
	PHASE2_PROBE_TURNAROUND_SIGNAL_B_RESPONSE_DELAY,
	PHASE2_PROBE_TURNAROUND_SIGNAL_B_REVERSAL_TAIL,
	PHASE2_PROBE_TURNAROUND_SIGNAL_B_PROBE_READY,
	PHASE2_PROBE_TURNAROUND_SIGNAL_A_INITIAL_TONE,
	PHASE2_PROBE_TURNAROUND_SIGNAL_A_REVERSAL_TAIL,
	PHASE2_PROBE_TURNAROUND_SIGNAL_A_WAIT_B_REVERSAL,
	PHASE2_PROBE_TURNAROUND_SIGNAL_A_WAIT_PROBE_START,
	PHASE2_PROBE_TURNAROUND_SIGNAL_A_PROBE_READY,
	PHASE2_PROBE_TURNAROUND_TIMED_OUT,
};

enum phase2_probe_turnaround_failure {
	PHASE2_PROBE_TURNAROUND_NO_FAILURE,
	PHASE2_PROBE_TURNAROUND_REMOTE_REVERSAL_TIMEOUT,
};

enum phase2_probe_turnaround_event_kind {
	PHASE2_PROBE_TURNAROUND_EVENT_NONE,
	PHASE2_PROBE_TURNAROUND_EVENT_RECOVERY_STARTED,
	PHASE2_PROBE_TURNAROUND_EVENT_PROBE_TRANSMIT_READY,
	PHASE2_PROBE_TURNAROUND_EVENT_PROBE_RECEIVE_READY,
	PHASE2_PROBE_TURNAROUND_EVENT_FAILED,
};

struct phase2_probe_turnaround_event {
	struct v34_phase2_tone_event tone_event;
	enum phase2_probe_turnaround_event_kind kind;
	enum phase2_probe_turnaround_failure failure;
};

enum phase2_probe_turnaround_result {
	PHASE2_PROBE_TURNAROUND_OK,
	PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT,
};

struct phase2_probe_turnaround {
	enum phase2_probe_turnaround_phase phase;
	enum phase2_probe_turnaround_failure failure;
	uint16_t phase_samples_remaining;
	/* Signal B inherits the remaining 900 ms + RTD deadline from ranging
	 * and probing; Signal A counts its own unanswered reversal interval. */
	uint16_t wait_samples;
	/* Elapsed samples from the estimated Signal B reversal. Later owners may
	 * retain this epoch for mode-specific recovery deadlines. */
	uint32_t signal_a_reversal_elapsed_samples;
};

/* Continue from the completed first probe. Signal A emits its 50 ms tone and
 * reversal; Signal B returns a reversal after 40 ms. The resulting events
 * hand reciprocal L1/L2 transmission to Signal B and reception to Signal A. */
enum phase2_probe_turnaround_result phase2_probe_turnaround_init(
	struct phase2_probe_turnaround *turnaround,
	const struct phase2_initial_probe *completed_probe,
	const struct phase2_info0_exchange *exchange);
bool phase2_probe_turnaround_is_valid(
	const struct phase2_probe_turnaround *turnaround,
	const struct phase2_initial_probe *completed_probe,
	const struct phase2_info0_exchange *exchange);

enum phase2_probe_turnaround_result phase2_probe_turnaround_sample(
	struct phase2_probe_turnaround *turnaround,
	const struct phase2_initial_probe *completed_probe,
	struct phase2_info0_exchange *exchange,
	int16_t received_sample, int16_t *transmit_sample,
	struct phase2_probe_turnaround_event *event);

#endif
