// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE2_H
#define V34_PHASE2_H

#include "v34_phase2_reciprocal_probe.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_phase2_stage {
	V34_PHASE2_RANGING,
	V34_PHASE2_INITIAL_PROBE,
	V34_PHASE2_PROBE_TURNAROUND,
	V34_PHASE2_RECIPROCAL_PROBE,
};

enum v34_phase2_result {
	V34_PHASE2_OK,
	V34_PHASE2_INVALID_ARGUMENT,
	V34_PHASE2_INVALID_STATE,
};

enum v34_phase2_failure {
	V34_PHASE2_FAILURE_NONE,
	V34_PHASE2_FAILURE_RANGING_TIMEOUT,
	V34_PHASE2_FAILURE_REMOTE_PROBE_NOT_FOUND,
	V34_PHASE2_FAILURE_LOCAL_PROBE_LIMIT,
	V34_PHASE2_FAILURE_REMOTE_TONE_TIMEOUT,
	V34_PHASE2_FAILURE_REMOTE_REVERSAL_TIMEOUT,
	V34_PHASE2_FAILURE_INTERNAL,
};

enum v34_phase2_event_kind {
	V34_PHASE2_EVENT_NONE,
	V34_PHASE2_EVENT_ROUND_TRIP_DELAY,
	V34_PHASE2_EVENT_INITIAL_PROBE_STARTED,
	V34_PHASE2_EVENT_REMOTE_INITIAL_PROBE_MEASURED,
	V34_PHASE2_EVENT_PROBE_TURNAROUND_STARTED,
	V34_PHASE2_EVENT_RECIPROCAL_PROBE_STARTED,
	V34_PHASE2_EVENT_READY_FOR_INFO1,
	V34_PHASE2_EVENT_FAILED,
};

/* A compact recording boundary for Phase 2 progress. Fields not selected by
 * `kind` are zero. */
struct v34_phase2_event {
	enum v34_phase2_event_kind kind;
	enum v34_phase2_failure failure;
	uint16_t round_trip_delay_samples;
};

union v34_phase2_state {
	struct v34_phase2_ranging ranging;
	struct v34_phase2_initial_probe initial_probe;
	struct v34_phase2_probe_turnaround turnaround;
	struct v34_phase2_reciprocal_probe reciprocal_probe;
};

/* Own the continuous INFO0, ranging, and bidirectional probe timeline for one
 * station. One instance's sample calls must be serialized by its caller. */
struct v34_phase2 {
	union v34_phase2_state state;
	enum v34_phase2_stage stage;
	enum v34_phase2_failure failure;
};

enum v34_phase2_result v34_phase2_init(
	struct v34_phase2 *phase2, enum v34_station_role local_role,
	int16_t nominal_amplitude, const struct v34_info0 *local_info0);

bool v34_phase2_is_valid(const struct v34_phase2 *phase2);
bool v34_phase2_has_failed(const struct v34_phase2 *phase2);
bool v34_phase2_is_ready_for_info1(const struct v34_phase2 *phase2);

/* Consume and produce exactly one 8 kHz line sample. Stage changes take
 * effect after the current sample, so the following call belongs to the new
 * owner. Successful calls always initialize both outputs. */
enum v34_phase2_result v34_phase2_sample(
	struct v34_phase2 *phase2, int16_t received_sample,
	int16_t *transmit_sample, struct v34_phase2_event *event);

/* Copy the role-appropriate remote probe measurement after the reciprocal
 * turn has completed. Failed calls leave the destination unchanged. */
enum v34_phase2_result v34_phase2_measure_remote_probe(
	const struct v34_phase2 *phase2,
	struct v34_phase2_probe_receiver_measurement *measurement);

const char *v34_phase2_result_name(enum v34_phase2_result result);
const char *v34_phase2_failure_name(enum v34_phase2_failure failure);

#endif /* V34_PHASE2_H */
