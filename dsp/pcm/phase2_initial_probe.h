// SPDX-License-Identifier: BSD-3-Clause

#ifndef PHASE2_INITIAL_PROBE_H
#define PHASE2_INITIAL_PROBE_H

#include "phase2_info0_exchange.h"
#include "v34_phase2_probe_acquisition.h"
#include "v34_phase2_probe_sequence.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	PHASE2_INITIAL_PROBE_TONE_WAIT_SAMPLES =
		V34_PHASE2_SAMPLE_RATE * 2,
	/* The peer measures all of L1 and approximately 160 ms of L2 before
	 * responding. Keep the detector closed through the early local-echo
	 * interval; a genuine response remains present when it opens. */
	PHASE2_INITIAL_PROBE_REMOTE_TONE_GUARD_SAMPLES =
		V34_PHASE2_SAMPLE_RATE * 3 / 16,
};

enum phase2_initial_probe_phase {
	PHASE2_INITIAL_PROBE_SIGNAL_B_RECEIVING,
	PHASE2_INITIAL_PROBE_SIGNAL_B_TONE,
	PHASE2_INITIAL_PROBE_SIGNAL_B_TONE_PAIR_ACTIVE,
	PHASE2_INITIAL_PROBE_SIGNAL_A_TRANSMITTING,
	PHASE2_INITIAL_PROBE_SIGNAL_A_TONE,
	PHASE2_INITIAL_PROBE_FAILED,
};

enum phase2_initial_probe_failure {
	PHASE2_INITIAL_PROBE_NO_FAILURE,
	PHASE2_INITIAL_PROBE_REMOTE_NOT_FOUND,
	PHASE2_INITIAL_PROBE_LOCAL_TRANSMIT_LIMIT,
	PHASE2_INITIAL_PROBE_REMOTE_TONE_TIMEOUT,
};

enum phase2_initial_probe_event_kind {
	PHASE2_INITIAL_PROBE_EVENT_NONE,
	PHASE2_INITIAL_PROBE_EVENT_REMOTE_PROBE_COMPLETE,
	PHASE2_INITIAL_PROBE_EVENT_LOCAL_PROBE_COMPLETE,
	PHASE2_INITIAL_PROBE_EVENT_TONE_PAIR_ACTIVE,
	PHASE2_INITIAL_PROBE_EVENT_FAILED,
};

struct phase2_initial_probe_event {
	struct v34_phase2_probe_acquisition_event receive_event;
	struct v34_phase2_probe_sequence_event transmit_event;
	struct v34_phase2_tone_event tone_event;
	enum phase2_initial_probe_event_kind kind;
	enum phase2_initial_probe_failure failure;
};

enum phase2_initial_probe_result {
	PHASE2_INITIAL_PROBE_OK,
	PHASE2_INITIAL_PROBE_INVALID_ARGUMENT,
};

union phase2_initial_probe_path {
	struct v34_phase2_probe_acquisition receive;
	struct v34_phase2_probe_sequence transmit;
};

struct phase2_initial_probe {
	union phase2_initial_probe_path path;
	enum phase2_initial_probe_phase phase;
	enum phase2_initial_probe_failure failure;
	uint16_t tone_wait_samples;
};

/* Continue after first ranging reaches its role-appropriate probe boundary.
 * Signal A transmits L1/L2; Signal B measures it and responds with Tone B.
 * The transport owns the continuous Tone A/B phase and detector state. */
enum phase2_initial_probe_result phase2_initial_probe_init(
	struct phase2_initial_probe *probe,
	const struct phase2_info0_exchange *exchange,
	uint16_t round_trip_delay_samples);
bool phase2_initial_probe_is_valid(
	const struct phase2_initial_probe *probe,
	const struct phase2_info0_exchange *exchange);

enum phase2_initial_probe_result phase2_initial_probe_sample(
	struct phase2_initial_probe *probe,
	struct phase2_info0_exchange *exchange,
	int16_t received_sample, int16_t *transmit_sample,
	struct phase2_initial_probe_event *event);

enum phase2_initial_probe_result phase2_initial_probe_measure(
	const struct phase2_initial_probe *probe,
	const struct phase2_info0_exchange *exchange,
	struct v34_phase2_probe_receiver_measurement *measurement);

const char *phase2_initial_probe_result_name(
	enum phase2_initial_probe_result result);

#endif
