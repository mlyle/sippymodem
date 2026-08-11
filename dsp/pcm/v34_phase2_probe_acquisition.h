// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE2_PROBE_ACQUISITION_H
#define V34_PHASE2_PROBE_ACQUISITION_H

#include "v34_phase2_probe_receiver.h"
#include "v34_phase2_probe_synchronizer.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_phase2_probe_acquisition_phase {
	V34_PHASE2_PROBE_ACQUISITION_SEARCHING,
	V34_PHASE2_PROBE_ACQUISITION_RECEIVING,
	V34_PHASE2_PROBE_ACQUISITION_NOT_FOUND,
	V34_PHASE2_PROBE_ACQUISITION_COMPLETE,
	V34_PHASE2_PROBE_ACQUISITION_LIMIT_REACHED,
};

enum v34_phase2_probe_acquisition_event_kind {
	V34_PHASE2_PROBE_ACQUISITION_EVENT_NONE,
	V34_PHASE2_PROBE_ACQUISITION_EVENT_ACQUIRED,
	V34_PHASE2_PROBE_ACQUISITION_EVENT_NOT_FOUND,
	V34_PHASE2_PROBE_ACQUISITION_EVENT_L2_STARTED,
	V34_PHASE2_PROBE_ACQUISITION_EVENT_MEASUREMENT_READY,
	V34_PHASE2_PROBE_ACQUISITION_EVENT_COMPLETED,
	V34_PHASE2_PROBE_ACQUISITION_EVENT_LIMIT_REACHED,
};

struct v34_phase2_probe_acquisition_event {
	enum v34_phase2_probe_acquisition_event_kind kind;
	uint16_t leading_samples;
	uint16_t replay_samples;
	double normalized_correlation;
	uint32_t l2_samples;
};

enum v34_phase2_probe_acquisition_result {
	V34_PHASE2_PROBE_ACQUISITION_OK,
	V34_PHASE2_PROBE_ACQUISITION_INVALID_ARGUMENT,
};

struct v34_phase2_probe_acquisition {
	struct v34_phase2_probe_synchronizer synchronizer;
	struct v34_phase2_probe_receiver receiver;
	enum v34_phase2_probe_acquisition_phase phase;
};

/* Begin within the bounded pre-L1 handoff interval. Acquisition retains and
 * replays the synchronization prefix internally, so every sample from the
 * selected edge contributes to the exact receive windows. */
enum v34_phase2_probe_acquisition_result
v34_phase2_probe_acquisition_init(
	struct v34_phase2_probe_acquisition *acquisition);
bool v34_phase2_probe_acquisition_is_valid(
	const struct v34_phase2_probe_acquisition *acquisition);

/* finish_l2 is accepted only after MEASUREMENT_READY. The completion call does
 * not consume received_sample, allowing the next protocol owner to receive
 * the first sample following L2. Terminal phases are idempotent. */
enum v34_phase2_probe_acquisition_result
v34_phase2_probe_acquisition_sample(
	struct v34_phase2_probe_acquisition *acquisition,
	int16_t received_sample, bool finish_l2,
	struct v34_phase2_probe_acquisition_event *event);

enum v34_phase2_probe_acquisition_result
v34_phase2_probe_acquisition_measure(
	const struct v34_phase2_probe_acquisition *acquisition,
	struct v34_phase2_probe_receiver_measurement *measurement);

const char *v34_phase2_probe_acquisition_result_name(
	enum v34_phase2_probe_acquisition_result result);

#endif
