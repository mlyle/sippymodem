// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE2_RECIPROCAL_PROBE_H
#define V34_PHASE2_RECIPROCAL_PROBE_H

#include "v34_phase2_probe_acquisition.h"
#include "v34_phase2_probe_sequence.h"
#include "v34_phase2_probe_turnaround.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_phase2_reciprocal_probe_result {
	V34_PHASE2_RECIPROCAL_PROBE_OK,
	V34_PHASE2_RECIPROCAL_PROBE_INVALID_ARGUMENT,
};

enum v34_phase2_reciprocal_probe_phase {
	V34_PHASE2_RECIPROCAL_PROBE_CALLER_TRANSMITTING,
	V34_PHASE2_RECIPROCAL_PROBE_CALLER_COMPLETE,
	V34_PHASE2_RECIPROCAL_PROBE_ANSWERER_RECEIVING,
	V34_PHASE2_RECIPROCAL_PROBE_ANSWERER_TONE_A,
	V34_PHASE2_RECIPROCAL_PROBE_FAILED,
};

enum v34_phase2_reciprocal_probe_failure {
	V34_PHASE2_RECIPROCAL_PROBE_NO_FAILURE,
	V34_PHASE2_RECIPROCAL_PROBE_REMOTE_NOT_FOUND,
	V34_PHASE2_RECIPROCAL_PROBE_LOCAL_TRANSMIT_LIMIT,
};

enum v34_phase2_reciprocal_probe_event_kind {
	V34_PHASE2_RECIPROCAL_PROBE_EVENT_NONE,
	V34_PHASE2_RECIPROCAL_PROBE_EVENT_REMOTE_PROBE_MEASURED,
	V34_PHASE2_RECIPROCAL_PROBE_EVENT_LOCAL_PROBE_ACKNOWLEDGED,
	V34_PHASE2_RECIPROCAL_PROBE_EVENT_FAILED,
};

struct v34_phase2_reciprocal_probe_event {
	struct v34_phase2_probe_acquisition_event receive_event;
	struct v34_phase2_probe_sequence_event transmit_event;
	struct v34_phase2_tone_event tone_event;
	enum v34_phase2_reciprocal_probe_event_kind kind;
	enum v34_phase2_reciprocal_probe_failure failure;
};

union v34_phase2_reciprocal_probe_path {
	struct v34_phase2_probe_sequence transmit;
	struct v34_phase2_probe_acquisition receive;
};

struct v34_phase2_reciprocal_probe {
	struct v34_phase2_probe_turnaround turnaround;
	union v34_phase2_reciprocal_probe_path path;
	enum v34_phase2_reciprocal_probe_phase phase;
	enum v34_phase2_reciprocal_probe_failure failure;
	uint32_t answerer_info1c_elapsed_samples;
};

/* Continue from a role-appropriate probe-turnaround readiness event. The
 * caller begins L1 on the following sample; the answerer begins bounded probe
 * acquisition on the following received sample. */
enum v34_phase2_reciprocal_probe_result
v34_phase2_reciprocal_probe_init(
	struct v34_phase2_reciprocal_probe *probe,
	const struct v34_phase2_probe_turnaround
		*completed_turnaround);
bool v34_phase2_reciprocal_probe_is_valid(
	const struct v34_phase2_reciprocal_probe *probe);

/* One call consumes and produces one 8 kHz line sample. The answerer starts
 * Tone A immediately after completing its fixed probe measurement. The caller
 * ends L2 when that tone is acquired, leaving the following sample to the
 * INFO1 exchange owner. Terminal caller and failure states remain silent. */
enum v34_phase2_reciprocal_probe_result
v34_phase2_reciprocal_probe_sample(
	struct v34_phase2_reciprocal_probe *probe,
	int16_t received_sample, int16_t *transmit_sample,
	struct v34_phase2_reciprocal_probe_event *event);

enum v34_phase2_reciprocal_probe_result
v34_phase2_reciprocal_probe_measure(
	const struct v34_phase2_reciprocal_probe *probe,
	struct v34_phase2_probe_receiver_measurement *measurement);

const char *v34_phase2_reciprocal_probe_result_name(
	enum v34_phase2_reciprocal_probe_result result);

#endif
