// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_H
#define SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_H

#include "v34_phase2_initial_probe.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_RESPONSE_SAMPLES =
		SIPPYMODEM_V34_PHASE2_SAMPLE_RATE * 40 / 1000,
	SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_INITIAL_TONE_SAMPLES =
		SIPPYMODEM_V34_PHASE2_SAMPLE_RATE * 50 / 1000,
	SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_TONE_TAIL_SAMPLES =
		SIPPYMODEM_V34_PHASE2_SAMPLE_RATE * 10 / 1000,
	/* Center the expected L1 edge within the synchronizer's search prefix so
	 * small positive or negative edge-estimation errors remain observable. */
	SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_RECEIVE_HANDOFF_LEAD_SAMPLES =
		SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_SEARCH_SAMPLES / 2,
	SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_MAXIMUM_WAIT_SAMPLES =
		SIPPYMODEM_V34_PHASE2_SAMPLE_RATE * 2,
};

enum sippymodem_v34_phase2_probe_turnaround_result {
	SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_OK,
	SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_INVALID_ARGUMENT,
};

enum sippymodem_v34_phase2_probe_turnaround_phase {
	SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_CALLER_WAIT_A_REVERSAL,
	SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_CALLER_RESPONSE_DELAY,
	SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_CALLER_REVERSAL_TAIL,
	SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_CALLER_PROBE_READY,
	SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_ANSWERER_INITIAL_TONE_A,
	SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_ANSWERER_REVERSAL_TAIL,
	SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_ANSWERER_WAIT_B_REVERSAL,
	SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_ANSWERER_WAIT_PROBE_START,
	SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_ANSWERER_PROBE_READY,
	SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_TIMED_OUT,
};

enum sippymodem_v34_phase2_probe_turnaround_failure {
	SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_NO_FAILURE,
	SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_REMOTE_REVERSAL_TIMEOUT,
};

enum sippymodem_v34_phase2_probe_turnaround_event_kind {
	SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_EVENT_NONE,
	SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_EVENT_PROBE_TRANSMIT_READY,
	SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_EVENT_PROBE_RECEIVE_READY,
	SIPPYMODEM_V34_PHASE2_PROBE_TURNAROUND_EVENT_FAILED,
};

struct sippymodem_v34_phase2_probe_turnaround_event {
	struct sippymodem_v34_phase2_tone_event tone_event;
	enum sippymodem_v34_phase2_probe_turnaround_event_kind kind;
	enum sippymodem_v34_phase2_probe_turnaround_failure failure;
};

struct sippymodem_v34_phase2_probe_turnaround {
	struct sippymodem_v34_phase2_initial_probe initial_probe;
	enum sippymodem_v34_phase2_probe_turnaround_phase phase;
	enum sippymodem_v34_phase2_probe_turnaround_failure failure;
	uint16_t phase_samples_remaining;
	uint16_t wait_samples;
	/* Elapsed samples from the estimated Tone B reversal in 11.2.1.2.6.
	 * The following probe and INFO1 owners preserve this recovery epoch. */
	uint32_t answerer_info1c_elapsed_samples;
};

/* Continue from a role-appropriate completed initial probe. The answerer is
 * handed off before its first Tone A sample; the caller is handed off after
 * acquiring that tone while its own Tone B remains continuous. */
enum sippymodem_v34_phase2_probe_turnaround_result
sippymodem_v34_phase2_probe_turnaround_init(
	struct sippymodem_v34_phase2_probe_turnaround *turnaround,
	const struct sippymodem_v34_phase2_initial_probe *completed_probe);
bool sippymodem_v34_phase2_probe_turnaround_is_valid(
	const struct sippymodem_v34_phase2_probe_turnaround *turnaround);

/* One call consumes and produces one sample at the 8 kHz line terminal. The
 * caller's reversal deadline compensates the tone detector's reported edge
 * latency. The transmit-ready event is emitted on the caller's final tone-tail
 * sample. Receive-ready leaves the synchronizer's bounded search prefix before
 * the expected L1 edge. The following sample belongs to the reciprocal probe
 * transmitter or receiver; this owner never generates or measures L1/L2. */
enum sippymodem_v34_phase2_probe_turnaround_result
sippymodem_v34_phase2_probe_turnaround_sample(
	struct sippymodem_v34_phase2_probe_turnaround *turnaround,
	int16_t received_sample, int16_t *transmit_sample,
	struct sippymodem_v34_phase2_probe_turnaround_event *event);

const char *sippymodem_v34_phase2_probe_turnaround_result_name(
	enum sippymodem_v34_phase2_probe_turnaround_result result);

#endif
