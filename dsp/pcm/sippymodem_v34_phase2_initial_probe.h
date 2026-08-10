// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_H
#define SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_H

#include "sippymodem_v34_phase2_probe_acquisition.h"
#include "sippymodem_v34_phase2_probe_sequence.h"
#include "sippymodem_v34_phase2_ranging.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_TONE_WAIT_SAMPLES =
		SIPPYMODEM_V34_PHASE2_SAMPLE_RATE * 2,
};

enum sippymodem_v34_phase2_initial_probe_phase {
	SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_CALLER_RECEIVING,
	SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_CALLER_TONE_B,
	SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_CALLER_TONE_PAIR_ACTIVE,
	SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_ANSWERER_TRANSMITTING,
	SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_ANSWERER_TONE_A,
	SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_FAILED,
};

enum sippymodem_v34_phase2_initial_probe_failure {
	SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_NO_FAILURE,
	SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_REMOTE_NOT_FOUND,
	SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_LOCAL_TRANSMIT_LIMIT,
	SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_REMOTE_TONE_TIMEOUT,
};

enum sippymodem_v34_phase2_initial_probe_event_kind {
	SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_EVENT_NONE,
	SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_EVENT_REMOTE_PROBE_COMPLETE,
	SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_EVENT_LOCAL_PROBE_COMPLETE,
	SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_EVENT_TONE_PAIR_ACTIVE,
	SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_EVENT_FAILED,
};

struct sippymodem_v34_phase2_initial_probe_event {
	struct sippymodem_v34_phase2_probe_acquisition_event receive_event;
	struct sippymodem_v34_phase2_probe_sequence_event transmit_event;
	struct sippymodem_v34_phase2_tone_event tone_event;
	enum sippymodem_v34_phase2_initial_probe_event_kind kind;
	enum sippymodem_v34_phase2_initial_probe_failure failure;
};

enum sippymodem_v34_phase2_initial_probe_result {
	SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_OK,
	SIPPYMODEM_V34_PHASE2_INITIAL_PROBE_INVALID_ARGUMENT,
};

union sippymodem_v34_phase2_initial_probe_path {
	struct sippymodem_v34_phase2_probe_acquisition receive;
	struct sippymodem_v34_phase2_probe_sequence transmit;
};

struct sippymodem_v34_phase2_initial_probe {
	struct sippymodem_v34_phase2_ranging ranging;
	union sippymodem_v34_phase2_initial_probe_path path;
	enum sippymodem_v34_phase2_initial_probe_phase phase;
	enum sippymodem_v34_phase2_initial_probe_failure failure;
	uint16_t tone_wait_samples;
};

/* Continue from a ranging controller after it emits its role-appropriate
 * probe-ready event. The completed state is copied so the tone phases, remote
 * detector state, and measured round-trip delay remain coherent. The answerer
 * begins L1 on the next sample; the caller begins bounded onset acquisition on
 * the next received sample. */
enum sippymodem_v34_phase2_initial_probe_result
sippymodem_v34_phase2_initial_probe_init(
	struct sippymodem_v34_phase2_initial_probe *probe,
	const struct sippymodem_v34_phase2_ranging *completed_ranging);
bool sippymodem_v34_phase2_initial_probe_is_valid(
	const struct sippymodem_v34_phase2_initial_probe *probe);

/* One call consumes and produces one 8 kHz line sample. After measuring the
 * remote probe, the caller emits Tone B. The answerer ends L2 when that tone is
 * acquired and emits Tone A from the following call. Both tones remain active
 * for the owner of the next reversal turn. */
enum sippymodem_v34_phase2_initial_probe_result
sippymodem_v34_phase2_initial_probe_sample(
	struct sippymodem_v34_phase2_initial_probe *probe,
	int16_t received_sample, int16_t *transmit_sample,
	struct sippymodem_v34_phase2_initial_probe_event *event);

enum sippymodem_v34_phase2_initial_probe_result
sippymodem_v34_phase2_initial_probe_measure(
	const struct sippymodem_v34_phase2_initial_probe *probe,
	struct sippymodem_v34_phase2_probe_receiver_measurement *measurement);

const char *sippymodem_v34_phase2_initial_probe_result_name(
	enum sippymodem_v34_phase2_initial_probe_result result);

#endif
