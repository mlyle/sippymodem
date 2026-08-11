// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE2_INITIAL_PROBE_H
#define V34_PHASE2_INITIAL_PROBE_H

#include "v34_phase2_probe_acquisition.h"
#include "v34_phase2_probe_sequence.h"
#include "v34_phase2_ranging.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_PHASE2_INITIAL_PROBE_TONE_WAIT_SAMPLES =
		V34_PHASE2_SAMPLE_RATE * 2,
};

enum v34_phase2_initial_probe_phase {
	V34_PHASE2_INITIAL_PROBE_CALLER_RECEIVING,
	V34_PHASE2_INITIAL_PROBE_CALLER_TONE_B,
	V34_PHASE2_INITIAL_PROBE_CALLER_TONE_PAIR_ACTIVE,
	V34_PHASE2_INITIAL_PROBE_ANSWERER_TRANSMITTING,
	V34_PHASE2_INITIAL_PROBE_ANSWERER_TONE_A,
	V34_PHASE2_INITIAL_PROBE_FAILED,
};

enum v34_phase2_initial_probe_failure {
	V34_PHASE2_INITIAL_PROBE_NO_FAILURE,
	V34_PHASE2_INITIAL_PROBE_REMOTE_NOT_FOUND,
	V34_PHASE2_INITIAL_PROBE_LOCAL_TRANSMIT_LIMIT,
	V34_PHASE2_INITIAL_PROBE_REMOTE_TONE_TIMEOUT,
};

enum v34_phase2_initial_probe_event_kind {
	V34_PHASE2_INITIAL_PROBE_EVENT_NONE,
	V34_PHASE2_INITIAL_PROBE_EVENT_REMOTE_PROBE_COMPLETE,
	V34_PHASE2_INITIAL_PROBE_EVENT_LOCAL_PROBE_COMPLETE,
	V34_PHASE2_INITIAL_PROBE_EVENT_TONE_PAIR_ACTIVE,
	V34_PHASE2_INITIAL_PROBE_EVENT_FAILED,
};

struct v34_phase2_initial_probe_event {
	struct v34_phase2_probe_acquisition_event receive_event;
	struct v34_phase2_probe_sequence_event transmit_event;
	struct v34_phase2_tone_event tone_event;
	enum v34_phase2_initial_probe_event_kind kind;
	enum v34_phase2_initial_probe_failure failure;
};

enum v34_phase2_initial_probe_result {
	V34_PHASE2_INITIAL_PROBE_OK,
	V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT,
};

union v34_phase2_initial_probe_path {
	struct v34_phase2_probe_acquisition receive;
	struct v34_phase2_probe_sequence transmit;
};

struct v34_phase2_initial_probe {
	struct v34_phase2_ranging ranging;
	union v34_phase2_initial_probe_path path;
	enum v34_phase2_initial_probe_phase phase;
	enum v34_phase2_initial_probe_failure failure;
	uint16_t tone_wait_samples;
};

/* Continue from a ranging controller after it emits its role-appropriate
 * probe-ready event. The completed state is copied so the tone phases, remote
 * detector state, and measured round-trip delay remain coherent. The answerer
 * begins L1 on the next sample; the caller begins bounded onset acquisition on
 * the next received sample. */
enum v34_phase2_initial_probe_result
v34_phase2_initial_probe_init(
	struct v34_phase2_initial_probe *probe,
	const struct v34_phase2_ranging *completed_ranging);
bool v34_phase2_initial_probe_is_valid(
	const struct v34_phase2_initial_probe *probe);

/* One call consumes and produces one 8 kHz line sample. After measuring the
 * remote probe, the caller emits Tone B. The answerer ends L2 when that tone is
 * acquired and emits Tone A from the following call. Both tones remain active
 * for the owner of the next reversal turn. */
enum v34_phase2_initial_probe_result
v34_phase2_initial_probe_sample(
	struct v34_phase2_initial_probe *probe,
	int16_t received_sample, int16_t *transmit_sample,
	struct v34_phase2_initial_probe_event *event);

enum v34_phase2_initial_probe_result
v34_phase2_initial_probe_measure(
	const struct v34_phase2_initial_probe *probe,
	struct v34_phase2_probe_receiver_measurement *measurement);

const char *v34_phase2_initial_probe_result_name(
	enum v34_phase2_initial_probe_result result);

#endif
