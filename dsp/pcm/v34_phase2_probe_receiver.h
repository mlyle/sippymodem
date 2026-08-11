// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE2_PROBE_RECEIVER_H
#define V34_PHASE2_PROBE_RECEIVER_H

#include "v34_phase2_probe_spectrum.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_phase2_probe_receiver_phase {
	V34_PHASE2_PROBE_RECEIVER_MEASURE_L1,
	V34_PHASE2_PROBE_RECEIVER_MEASURE_L2,
	V34_PHASE2_PROBE_RECEIVER_WAIT_L2_END,
	V34_PHASE2_PROBE_RECEIVER_COMPLETE,
	V34_PHASE2_PROBE_RECEIVER_LIMIT_REACHED,
};

enum v34_phase2_probe_receiver_event_kind {
	V34_PHASE2_PROBE_RECEIVER_EVENT_NONE,
	V34_PHASE2_PROBE_RECEIVER_EVENT_L2_STARTED,
	V34_PHASE2_PROBE_RECEIVER_EVENT_MEASUREMENT_READY,
	V34_PHASE2_PROBE_RECEIVER_EVENT_COMPLETED,
	V34_PHASE2_PROBE_RECEIVER_EVENT_LIMIT_REACHED,
};

struct v34_phase2_probe_receiver_event {
	enum v34_phase2_probe_receiver_event_kind kind;
	uint32_t l2_samples;
};

enum v34_phase2_probe_receiver_result {
	V34_PHASE2_PROBE_RECEIVER_OK,
	V34_PHASE2_PROBE_RECEIVER_INVALID_ARGUMENT,
};

struct v34_phase2_probe_receiver {
	struct v34_phase2_probe_spectrum l1_spectrum;
	struct v34_phase2_probe_spectrum l2_spectrum;
	enum v34_phase2_probe_receiver_phase phase;
	uint32_t l2_samples;
};

struct v34_phase2_probe_receiver_measurement {
	struct v34_phase2_probe_spectrum_measurement l1;
	struct v34_phase2_probe_spectrum_measurement l2;
	uint32_t l2_samples;
};

/* The first sample passed after initialization must be the synchronized first
 * L1 sample. The receiver measures one exact 160 ms window at each level,
 * then waits for its protocol owner to report the end of L2. Independently,
 * it enforces the 500 ms receive limit. */
enum v34_phase2_probe_receiver_result
v34_phase2_probe_receiver_init(
	struct v34_phase2_probe_receiver *receiver);
bool v34_phase2_probe_receiver_is_valid(
	const struct v34_phase2_probe_receiver *receiver);

/* finish_l2 is valid only after the fixed L2 measurement is ready. That call
 * does not count received_sample as part of L2, allowing the owner to hand the
 * first sample of the following signal to its next receiver. */
enum v34_phase2_probe_receiver_result
v34_phase2_probe_receiver_sample(
	struct v34_phase2_probe_receiver *receiver,
	int16_t received_sample, bool finish_l2,
	struct v34_phase2_probe_receiver_event *event);

/* Measurement is available from MEASUREMENT_READY onward. Both spectra span
 * whole probe periods, so their complex amplitudes use exact normalization. */
enum v34_phase2_probe_receiver_result
v34_phase2_probe_receiver_measure(
	const struct v34_phase2_probe_receiver *receiver,
	struct v34_phase2_probe_receiver_measurement *measurement);

const char *v34_phase2_probe_receiver_result_name(
	enum v34_phase2_probe_receiver_result result);

#endif
