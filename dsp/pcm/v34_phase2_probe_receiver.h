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

bool v34_phase2_probe_measurement_is_valid(
	const struct v34_phase2_probe_receiver_measurement *measurement);

/* The first sample passed after initialization must be the synchronized first
 * L1 sample. L1 spans exactly 160 ms; L2 is variable (V.34 10.1.2.4).
 * Unless its owner finishes L2 earlier, the receiver measures the first
 * 160 ms of L2 and then waits for its end, enforcing the 500 ms limit. */
enum v34_phase2_probe_receiver_result
v34_phase2_probe_receiver_init(
	struct v34_phase2_probe_receiver *receiver);
bool v34_phase2_probe_receiver_is_valid(
	const struct v34_phase2_probe_receiver *receiver);

/* The owner may finish L2 after any complete 20 ms probe period while still
 * measuring, or at any sample after MEASUREMENT_READY. That call does not
 * count received_sample as part of L2, allowing the owner to hand the first
 * sample of the following signal to its next receiver. */
enum v34_phase2_probe_receiver_result
v34_phase2_probe_receiver_sample(
	struct v34_phase2_probe_receiver *receiver,
	int16_t received_sample, bool finish_l2,
	struct v34_phase2_probe_receiver_event *event);

/* Measurement is available after MEASUREMENT_READY or COMPLETED. Both spectra
 * span whole probe periods, so complex amplitudes use exact normalization. */
enum v34_phase2_probe_receiver_result
v34_phase2_probe_receiver_measure(
	const struct v34_phase2_probe_receiver *receiver,
	struct v34_phase2_probe_receiver_measurement *measurement);

#endif
