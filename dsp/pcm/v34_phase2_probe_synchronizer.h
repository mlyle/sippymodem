// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE2_PROBE_SYNCHRONIZER_H
#define V34_PHASE2_PROBE_SYNCHRONIZER_H

#include "v34_phase2_probe.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	/* Ranging hands reception over near the end of the preceding tone. Two
	 * milliseconds bounds the residual edge-estimation error without making
	 * acquisition consume a second probe period. */
	V34_PHASE2_PROBE_SYNCHRONIZER_SEARCH_SAMPLES =
		V34_PHASE2_SAMPLE_RATE * 2 / 1000,
	V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES =
		V34_PHASE2_PROBE_SYNCHRONIZER_SEARCH_SAMPLES +
		V34_PHASE2_PROBE_PERIOD_SAMPLES,
};

enum v34_phase2_probe_synchronizer_phase {
	V34_PHASE2_PROBE_SYNCHRONIZER_COLLECTING,
	V34_PHASE2_PROBE_SYNCHRONIZER_ACQUIRED,
	V34_PHASE2_PROBE_SYNCHRONIZER_NOT_FOUND,
};

enum v34_phase2_probe_synchronizer_event_kind {
	V34_PHASE2_PROBE_SYNCHRONIZER_EVENT_NONE,
	V34_PHASE2_PROBE_SYNCHRONIZER_EVENT_ACQUIRED,
	V34_PHASE2_PROBE_SYNCHRONIZER_EVENT_NOT_FOUND,
};

struct v34_phase2_probe_synchronizer_event {
	enum v34_phase2_probe_synchronizer_event_kind kind;
	uint16_t leading_samples;
	uint16_t replay_samples;
	double probe_energy_fraction;
};

enum v34_phase2_probe_synchronizer_result {
	V34_PHASE2_PROBE_SYNCHRONIZER_OK,
	V34_PHASE2_PROBE_SYNCHRONIZER_INVALID_ARGUMENT,
};

struct v34_phase2_probe_synchronizer {
	int16_t capture[V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES];
	enum v34_phase2_probe_synchronizer_phase phase;
	uint16_t samples_observed;
	uint16_t leading_samples;
	double probe_energy_fraction;
};

/* Collect a bounded interval beginning near the expected L1 edge. Acquisition
 * measures every candidate over one exact probe period. It is invariant to
 * line gain, polarity, carrier phase, and independent phase rotation of the
 * probe frequencies. No waveform or coefficient table is stored. */
enum v34_phase2_probe_synchronizer_result
v34_phase2_probe_synchronizer_init(
	struct v34_phase2_probe_synchronizer *synchronizer);
bool v34_phase2_probe_synchronizer_is_valid(
	const struct v34_phase2_probe_synchronizer *synchronizer);
enum v34_phase2_probe_synchronizer_result
v34_phase2_probe_synchronizer_sample(
	struct v34_phase2_probe_synchronizer *synchronizer,
	int16_t received_sample,
	struct v34_phase2_probe_synchronizer_event *event);

/* After acquisition, replay exposes every captured sample from the detected L1
 * edge onward. Feed all replay samples to the probe receiver before resuming
 * the live stream; this preserves an exact, gap-free measurement timeline. */
uint16_t v34_phase2_probe_synchronizer_replay_count(
	const struct v34_phase2_probe_synchronizer *synchronizer);
enum v34_phase2_probe_synchronizer_result
v34_phase2_probe_synchronizer_replay(
	const struct v34_phase2_probe_synchronizer *synchronizer,
	uint16_t replay_index, int16_t *sample);

const char *v34_phase2_probe_synchronizer_result_name(
	enum v34_phase2_probe_synchronizer_result result);

#endif
