// SPDX-License-Identifier: BSD-3-Clause

#ifndef PHASE2_RECIPROCAL_PROBE_H
#define PHASE2_RECIPROCAL_PROBE_H

#include "phase2_probe_turnaround.h"
#include "v34_phase2_probe_acquisition.h"
#include "v34_phase2_probe_sequence.h"

#include <stdbool.h>
#include <stdint.h>

enum phase2_reciprocal_probe_phase {
	PHASE2_RECIPROCAL_PROBE_SIGNAL_B_TRANSMITTING,
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
};

/* Continue after probe turnaround. Signal B transmits reciprocal L1/L2;
 * Signal A measures it and raises Tone A when the fixed windows complete. */
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
