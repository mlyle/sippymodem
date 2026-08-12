// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE2_H
#define V90_PHASE2_H

#include "v90_phase2_reciprocal_probe.h"

#include <stdbool.h>
#include <stdint.h>

enum v90_phase2_stage {
	V90_PHASE2_RANGING,
	V90_PHASE2_INITIAL_PROBE,
	V90_PHASE2_PROBE_TURNAROUND,
	V90_PHASE2_RECIPROCAL_PROBE,
};

enum v90_phase2_result {
	V90_PHASE2_OK,
	V90_PHASE2_INVALID_ARGUMENT,
	V90_PHASE2_INVALID_STATE,
};

enum v90_phase2_failure {
	V90_PHASE2_FAILURE_NONE,
	V90_PHASE2_FAILURE_RANGING_TIMEOUT,
	V90_PHASE2_FAILURE_REMOTE_PROBE_NOT_FOUND,
	V90_PHASE2_FAILURE_LOCAL_PROBE_LIMIT,
	V90_PHASE2_FAILURE_REMOTE_TONE_TIMEOUT,
	V90_PHASE2_FAILURE_REMOTE_REVERSAL_TIMEOUT,
	V90_PHASE2_FAILURE_INTERNAL,
};

enum v90_phase2_event_kind {
	V90_PHASE2_EVENT_NONE,
	V90_PHASE2_EVENT_ROUND_TRIP_DELAY,
	V90_PHASE2_EVENT_INITIAL_PROBE_STARTED,
	V90_PHASE2_EVENT_REMOTE_INITIAL_PROBE_MEASURED,
	V90_PHASE2_EVENT_PROBE_TURNAROUND_STARTED,
	V90_PHASE2_EVENT_RECIPROCAL_PROBE_STARTED,
	V90_PHASE2_EVENT_READY_FOR_INFO1,
	V90_PHASE2_EVENT_FAILED,
};

/* A compact recording boundary for Phase 2 progress. Fields not selected by
 * kind are zero. */
struct v90_phase2_event {
	enum v90_phase2_event_kind kind;
	enum v90_phase2_failure failure;
	uint16_t round_trip_delay_samples;
};

union v90_phase2_state {
	struct v90_phase2_ranging ranging;
	struct v90_phase2_initial_probe initial_probe;
	struct v90_phase2_probe_turnaround turnaround;
	struct v90_phase2_reciprocal_probe reciprocal_probe;
};

/* Own one analog or digital modem's continuous INFO0 and probing timeline.
 * Retained records and measurements survive waveform handoffs for INFO1 and
 * Phase 3 setup. A caller must serialize sample calls for one instance;
 * separate instances share no mutable state. */
struct v90_phase2 {
	union v90_phase2_state state;
	union v90_info0_record local_info0;
	union v90_info0_record peer_info0;
	struct v34_phase2_probe_receiver_measurement remote_probe;
	enum v90_phase2_stage stage;
	enum v90_phase2_failure failure;
	enum v90_modem_role local_role;
	bool peer_info0_stored;
	bool remote_probe_stored;
};

enum v90_phase2_result v90_phase2_init_analog(
	struct v90_phase2 *phase2, int16_t nominal_amplitude,
	const struct v90_info0 *local_info0a);
enum v90_phase2_result v90_phase2_init_digital(
	struct v90_phase2 *phase2, int16_t nominal_amplitude,
	const struct v90_info0d *local_info0d);

bool v90_phase2_is_valid(const struct v90_phase2 *phase2);
bool v90_phase2_has_failed(const struct v90_phase2 *phase2);
bool v90_phase2_is_ready_for_info1(const struct v90_phase2 *phase2);

/* Typed access prevents interpreting the role-dependent union through the
 * wrong member. Failed calls leave the destination unchanged. */
enum v90_phase2_result v90_phase2_peer_info0a(
	const struct v90_phase2 *phase2, struct v90_info0 *peer_info0a);
enum v90_phase2_result v90_phase2_peer_info0d(
	const struct v90_phase2 *phase2, struct v90_info0d *peer_info0d);

/* Consume and produce exactly one 8 kHz line-terminal sample. A stage change
 * applies after the current sample, so the next call belongs to the new owner.
 * Successful calls always initialize both outputs. */
enum v90_phase2_result v90_phase2_sample(
	struct v90_phase2 *phase2, int16_t received_sample,
	int16_t *transmit_sample, struct v90_phase2_event *event);

/* Copy the completed remote probe measurement at the INFO1 boundary. */
enum v90_phase2_result v90_phase2_measure_remote_probe(
	const struct v90_phase2 *phase2,
	struct v34_phase2_probe_receiver_measurement *measurement);

const char *v90_phase2_result_name(enum v90_phase2_result result);
const char *v90_phase2_failure_name(enum v90_phase2_failure failure);

#endif /* V90_PHASE2_H */
