// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_PHASE2_RECIPROCAL_PROBE_H
#define SIPPYMODEM_V34_PHASE2_RECIPROCAL_PROBE_H

#include "sippymodem_v34_phase2_probe_acquisition.h"
#include "sippymodem_v34_phase2_probe_sequence.h"
#include "sippymodem_v34_phase2_probe_turnaround.h"

#include <stdbool.h>
#include <stdint.h>

enum sippymodem_v34_phase2_reciprocal_probe_result {
	SIPPYMODEM_V34_PHASE2_RECIPROCAL_PROBE_OK,
	SIPPYMODEM_V34_PHASE2_RECIPROCAL_PROBE_INVALID_ARGUMENT,
};

enum sippymodem_v34_phase2_reciprocal_probe_phase {
	SIPPYMODEM_V34_PHASE2_RECIPROCAL_PROBE_CALLER_TRANSMITTING,
	SIPPYMODEM_V34_PHASE2_RECIPROCAL_PROBE_CALLER_COMPLETE,
	SIPPYMODEM_V34_PHASE2_RECIPROCAL_PROBE_ANSWERER_RECEIVING,
	SIPPYMODEM_V34_PHASE2_RECIPROCAL_PROBE_ANSWERER_TONE_A,
	SIPPYMODEM_V34_PHASE2_RECIPROCAL_PROBE_FAILED,
};

enum sippymodem_v34_phase2_reciprocal_probe_failure {
	SIPPYMODEM_V34_PHASE2_RECIPROCAL_PROBE_NO_FAILURE,
	SIPPYMODEM_V34_PHASE2_RECIPROCAL_PROBE_REMOTE_NOT_FOUND,
	SIPPYMODEM_V34_PHASE2_RECIPROCAL_PROBE_LOCAL_TRANSMIT_LIMIT,
};

enum sippymodem_v34_phase2_reciprocal_probe_event_kind {
	SIPPYMODEM_V34_PHASE2_RECIPROCAL_PROBE_EVENT_NONE,
	SIPPYMODEM_V34_PHASE2_RECIPROCAL_PROBE_EVENT_REMOTE_PROBE_MEASURED,
	SIPPYMODEM_V34_PHASE2_RECIPROCAL_PROBE_EVENT_LOCAL_PROBE_ACKNOWLEDGED,
	SIPPYMODEM_V34_PHASE2_RECIPROCAL_PROBE_EVENT_FAILED,
};

struct sippymodem_v34_phase2_reciprocal_probe_event {
	struct sippymodem_v34_phase2_probe_acquisition_event receive_event;
	struct sippymodem_v34_phase2_probe_sequence_event transmit_event;
	struct sippymodem_v34_phase2_tone_event tone_event;
	enum sippymodem_v34_phase2_reciprocal_probe_event_kind kind;
	enum sippymodem_v34_phase2_reciprocal_probe_failure failure;
};

union sippymodem_v34_phase2_reciprocal_probe_path {
	struct sippymodem_v34_phase2_probe_sequence transmit;
	struct sippymodem_v34_phase2_probe_acquisition receive;
};

struct sippymodem_v34_phase2_reciprocal_probe {
	struct sippymodem_v34_phase2_probe_turnaround turnaround;
	union sippymodem_v34_phase2_reciprocal_probe_path path;
	enum sippymodem_v34_phase2_reciprocal_probe_phase phase;
	enum sippymodem_v34_phase2_reciprocal_probe_failure failure;
	uint32_t answerer_info1c_elapsed_samples;
};

/* Continue from a role-appropriate probe-turnaround readiness event. The
 * caller begins L1 on the following sample; the answerer begins bounded probe
 * acquisition on the following received sample. */
enum sippymodem_v34_phase2_reciprocal_probe_result
sippymodem_v34_phase2_reciprocal_probe_init(
	struct sippymodem_v34_phase2_reciprocal_probe *probe,
	const struct sippymodem_v34_phase2_probe_turnaround
		*completed_turnaround);
bool sippymodem_v34_phase2_reciprocal_probe_is_valid(
	const struct sippymodem_v34_phase2_reciprocal_probe *probe);

/* One call consumes and produces one 8 kHz line sample. The answerer starts
 * Tone A immediately after completing its fixed probe measurement. The caller
 * ends L2 when that tone is acquired, leaving the following sample to the
 * INFO1 exchange owner. Terminal caller and failure states remain silent. */
enum sippymodem_v34_phase2_reciprocal_probe_result
sippymodem_v34_phase2_reciprocal_probe_sample(
	struct sippymodem_v34_phase2_reciprocal_probe *probe,
	int16_t received_sample, int16_t *transmit_sample,
	struct sippymodem_v34_phase2_reciprocal_probe_event *event);

enum sippymodem_v34_phase2_reciprocal_probe_result
sippymodem_v34_phase2_reciprocal_probe_measure(
	const struct sippymodem_v34_phase2_reciprocal_probe *probe,
	struct sippymodem_v34_phase2_probe_receiver_measurement *measurement);

const char *sippymodem_v34_phase2_reciprocal_probe_result_name(
	enum sippymodem_v34_phase2_reciprocal_probe_result result);

#endif
