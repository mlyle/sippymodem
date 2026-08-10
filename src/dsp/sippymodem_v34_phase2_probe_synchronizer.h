// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_H
#define SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_H

#include "sippymodem_v34_phase2_probe.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	/* Ranging hands reception over near the end of the preceding tone. Two
	 * milliseconds bounds the residual edge-estimation error without making
	 * acquisition consume a second probe period. */
	SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_SEARCH_SAMPLES =
		SIPPYMODEM_V34_PHASE2_SAMPLE_RATE * 2 / 1000,
	SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES =
		SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_SEARCH_SAMPLES +
		SIPPYMODEM_V34_PHASE2_PROBE_PERIOD_SAMPLES,
};

enum sippymodem_v34_phase2_probe_synchronizer_phase {
	SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_COLLECTING,
	SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_ACQUIRED,
	SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_NOT_FOUND,
};

enum sippymodem_v34_phase2_probe_synchronizer_event_kind {
	SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_EVENT_NONE,
	SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_EVENT_ACQUIRED,
	SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_EVENT_NOT_FOUND,
};

struct sippymodem_v34_phase2_probe_synchronizer_event {
	enum sippymodem_v34_phase2_probe_synchronizer_event_kind kind;
	uint16_t leading_samples;
	uint16_t replay_samples;
	double normalized_correlation;
};

enum sippymodem_v34_phase2_probe_synchronizer_result {
	SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_OK,
	SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_INVALID_ARGUMENT,
};

struct sippymodem_v34_phase2_probe_synchronizer {
	int16_t capture[SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES];
	enum sippymodem_v34_phase2_probe_synchronizer_phase phase;
	uint16_t samples_observed;
	uint16_t leading_samples;
	double normalized_correlation;
};

/* Collect a bounded interval beginning near the expected L1 edge. Acquisition
 * compares every candidate against a generated probe period and is invariant
 * to line gain and polarity. No waveform or coefficient table is stored. */
enum sippymodem_v34_phase2_probe_synchronizer_result
sippymodem_v34_phase2_probe_synchronizer_init(
	struct sippymodem_v34_phase2_probe_synchronizer *synchronizer);
bool sippymodem_v34_phase2_probe_synchronizer_is_valid(
	const struct sippymodem_v34_phase2_probe_synchronizer *synchronizer);
enum sippymodem_v34_phase2_probe_synchronizer_result
sippymodem_v34_phase2_probe_synchronizer_sample(
	struct sippymodem_v34_phase2_probe_synchronizer *synchronizer,
	int16_t received_sample,
	struct sippymodem_v34_phase2_probe_synchronizer_event *event);

/* After acquisition, replay exposes every captured sample from the detected L1
 * edge onward. Feed all replay samples to the probe receiver before resuming
 * the live stream; this preserves an exact, gap-free measurement timeline. */
uint16_t sippymodem_v34_phase2_probe_synchronizer_replay_count(
	const struct sippymodem_v34_phase2_probe_synchronizer *synchronizer);
enum sippymodem_v34_phase2_probe_synchronizer_result
sippymodem_v34_phase2_probe_synchronizer_replay(
	const struct sippymodem_v34_phase2_probe_synchronizer *synchronizer,
	uint16_t replay_index, int16_t *sample);

const char *sippymodem_v34_phase2_probe_synchronizer_result_name(
	enum sippymodem_v34_phase2_probe_synchronizer_result result);

#endif
