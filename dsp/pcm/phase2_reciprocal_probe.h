// SPDX-License-Identifier: BSD-3-Clause

#ifndef PHASE2_RECIPROCAL_PROBE_H
#define PHASE2_RECIPROCAL_PROBE_H

#include "phase2_probe_turnaround.h"
#include "v34_phase2_probe_acquisition.h"
#include "v34_phase2_probe_sequence.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	/* V.34 11.2.2.1.5 and V.90 9.2.1.2.5 allow 650 ms plus one
	 * round-trip delay from the beginning of L2 to detect final Tone A. The
	 * probe sequence becomes silent after 550 ms plus that same delay, so
	 * retain the detector for the remaining 100 ms. */
	PHASE2_RECIPROCAL_PROBE_TONE_A_DEADLINE_SAMPLES =
		V34_PHASE2_SAMPLE_RATE * 650 / 1000,
	PHASE2_RECIPROCAL_PROBE_TONE_A_WAIT_SAMPLES =
		PHASE2_RECIPROCAL_PROBE_TONE_A_DEADLINE_SAMPLES -
		V34_PHASE2_PROBE_L2_ECHO_LIMIT_BASE_SAMPLES,
};

enum phase2_reciprocal_probe_phase {
	PHASE2_RECIPROCAL_PROBE_SIGNAL_B_TRANSMITTING,
	PHASE2_RECIPROCAL_PROBE_SIGNAL_B_WAIT_TONE,
	PHASE2_RECIPROCAL_PROBE_SIGNAL_B_COMPLETE,
	PHASE2_RECIPROCAL_PROBE_SIGNAL_A_RECEIVING,
	PHASE2_RECIPROCAL_PROBE_SIGNAL_A_TONE,
	PHASE2_RECIPROCAL_PROBE_FAILED,
};

enum phase2_reciprocal_probe_failure {
	PHASE2_RECIPROCAL_PROBE_NO_FAILURE,
	PHASE2_RECIPROCAL_PROBE_REMOTE_NOT_FOUND,
	PHASE2_RECIPROCAL_PROBE_LOCAL_TRANSMIT_LIMIT,
};

enum phase2_reciprocal_probe_event_kind {
	PHASE2_RECIPROCAL_PROBE_EVENT_NONE,
	PHASE2_RECIPROCAL_PROBE_EVENT_REMOTE_PROBE_MEASURED,
	PHASE2_RECIPROCAL_PROBE_EVENT_LOCAL_PROBE_ACKNOWLEDGED,
	PHASE2_RECIPROCAL_PROBE_EVENT_FAILED,
};

struct phase2_reciprocal_probe_event {
	struct v34_phase2_probe_acquisition_event receive_event;
	struct v34_phase2_probe_sequence_event transmit_event;
	struct v34_phase2_tone_event tone_event;
	enum phase2_reciprocal_probe_event_kind kind;
	enum phase2_reciprocal_probe_failure failure;
};

enum phase2_reciprocal_probe_result {
	PHASE2_RECIPROCAL_PROBE_OK,
	PHASE2_RECIPROCAL_PROBE_INVALID_ARGUMENT,
};

union phase2_reciprocal_probe_path {
	struct v34_phase2_probe_sequence transmit;
	struct v34_phase2_probe_acquisition receive;
};

struct phase2_reciprocal_probe {
	union phase2_reciprocal_probe_path path;
	enum phase2_reciprocal_probe_phase phase;
	enum phase2_reciprocal_probe_failure failure;
	/* Elapsed samples from Signal B's reversal. The mode-specific INFO1
	 * owner retains this epoch for its response deadline. */
	uint32_t signal_a_reversal_elapsed_samples;
	uint16_t signal_b_tone_wait_samples;
};

/* Continue after probe turnaround. Signal B transmits reciprocal L1/L2, then
 * waits silently for a bounded Tone-A response if L2 reaches its wire limit.
 * Signal A measures the probe and raises Tone A when the fixed windows complete. */
enum phase2_reciprocal_probe_result phase2_reciprocal_probe_init(
	struct phase2_reciprocal_probe *probe,
	const struct phase2_probe_turnaround *completed_turnaround,
	const struct phase2_initial_probe *completed_initial_probe,
	struct phase2_info0_exchange *exchange,
	uint16_t round_trip_delay_samples);
bool phase2_reciprocal_probe_is_valid(
	const struct phase2_reciprocal_probe *probe,
	const struct phase2_probe_turnaround *completed_turnaround,
	const struct phase2_initial_probe *completed_initial_probe,
	const struct phase2_info0_exchange *exchange);

enum phase2_reciprocal_probe_result phase2_reciprocal_probe_sample(
	struct phase2_reciprocal_probe *probe,
	const struct phase2_probe_turnaround *completed_turnaround,
	const struct phase2_initial_probe *completed_initial_probe,
	struct phase2_info0_exchange *exchange,
	int16_t received_sample, int16_t *transmit_sample,
	struct phase2_reciprocal_probe_event *event);

enum phase2_reciprocal_probe_result phase2_reciprocal_probe_measure(
	const struct phase2_reciprocal_probe *probe,
	const struct phase2_probe_turnaround *completed_turnaround,
	const struct phase2_initial_probe *completed_initial_probe,
	const struct phase2_info0_exchange *exchange,
	struct v34_phase2_probe_receiver_measurement *measurement);

const char *phase2_reciprocal_probe_result_name(
	enum phase2_reciprocal_probe_result result);

#endif
