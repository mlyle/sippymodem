// SPDX-License-Identifier: BSD-3-Clause

#ifndef PHASE2_INITIAL_PROBE_H
#define PHASE2_INITIAL_PROBE_H

#include "phase2_ranging.h"
#include "phase2_probe.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	/* V.34 11.2.2.1.4 / V.90 9.2.1.2.4, from Signal A's second
	 * ranging reversal, not from completion of the initial probe. */
	PHASE2_INITIAL_PROBE_REVERSAL_WAIT_BASE_SAMPLES =
		V34_PHASE2_SAMPLE_RATE * 900 / 1000,
	/* V.34 11.2.2.2.3 / V.90 9.2.2.2.3: Tone B may arrive up to
	 * 600 ms plus RTD after L2 begins, 50 ms beyond its wire limit. */
	PHASE2_INITIAL_PROBE_TONE_B_WAIT_SAMPLES =
		V34_PHASE2_SAMPLE_RATE * 600 / 1000 -
		V34_PHASE2_PROBE_L2_ECHO_LIMIT_BASE_SAMPLES,
};

enum phase2_initial_probe_phase {
	PHASE2_INITIAL_PROBE_SIGNAL_B_RECEIVING,
	PHASE2_INITIAL_PROBE_SIGNAL_B_TONE,
	PHASE2_INITIAL_PROBE_SIGNAL_A_TRANSMITTING,
	PHASE2_INITIAL_PROBE_SIGNAL_A_WAIT_TONE,
	PHASE2_INITIAL_PROBE_SIGNAL_A_TONE,
	PHASE2_INITIAL_PROBE_FAILED,
};

enum phase2_initial_probe_event_kind {
	PHASE2_INITIAL_PROBE_EVENT_NONE,
	PHASE2_INITIAL_PROBE_EVENT_REMOTE_PROBE_COMPLETE,
	PHASE2_INITIAL_PROBE_EVENT_LOCAL_PROBE_COMPLETE,
	PHASE2_INITIAL_PROBE_EVENT_FAILED,
};

struct phase2_initial_probe_event {
	struct v34_phase2_probe_acquisition_event receive_event;
	struct v34_phase2_probe_sequence_event transmit_event;
	struct v34_phase2_tone_event tone_event;
	enum phase2_initial_probe_event_kind kind;
	enum phase2_probe_failure failure;
};

enum phase2_initial_probe_result {
	PHASE2_INITIAL_PROBE_OK,
	PHASE2_INITIAL_PROBE_INVALID_ARGUMENT,
};

struct phase2_initial_probe {
	union phase2_probe_path path;
	enum phase2_initial_probe_phase phase;
	enum phase2_probe_failure failure;
	uint16_t tone_wait_samples;
	uint16_t reversal_wait_samples_remaining;
};

/* Continue after first ranging reaches its role-appropriate probe boundary.
 * Signal A transmits L1/L2; Signal B measures it and responds with Tone B.
 * REMOTE_PROBE_COMPLETE hands Signal B directly to turnaround, which owns
 * both acquisition of Tone A and its reversal deadline. The transport owns
 * the continuous Tone A/B phase and detector state. */
enum phase2_initial_probe_result phase2_initial_probe_init(
	struct phase2_initial_probe *probe,
	const struct phase2_info0_exchange *exchange,
	const struct phase2_ranging_control *completed_ranging);
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
