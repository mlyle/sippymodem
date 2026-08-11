// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE2_TONE_H
#define V34_PHASE2_TONE_H

#include "v34_phase2_signal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	/* Five milliseconds contains whole cycles of both Phase 2 carriers and
	 * exactly three cycles of their 600 Hz separation. */
	V34_PHASE2_TONE_WINDOW_SAMPLES =
		V34_PHASE2_SAMPLE_RATE * 5 / 1000,
	V34_PHASE2_TONE_ACQUIRE_SAMPLES =
		V34_PHASE2_TONE_WINDOW_SAMPLES * 2,
	V34_PHASE2_TONE_LOSS_SAMPLES =
		V34_PHASE2_TONE_WINDOW_SAMPLES * 2,
};

enum v34_phase2_tone_result {
	V34_PHASE2_TONE_OK,
	V34_PHASE2_TONE_INVALID_ARGUMENT,
};

struct v34_phase2_tone_transmitter {
	uint32_t carrier_phase;
	uint32_t carrier_phase_step;
	uint32_t guard_phase;
	uint32_t guard_phase_step;
	int16_t carrier_amplitude;
	int16_t guard_amplitude;
	enum v34_station_role local_role;
};

struct v34_phase2_tone_observation {
	double in_phase;
	double quadrature;
	double energy;
};

enum v34_phase2_tone_event_kind {
	V34_PHASE2_TONE_NO_EVENT,
	V34_PHASE2_TONE_STARTED,
	V34_PHASE2_TONE_PHASE_REVERSED,
	V34_PHASE2_TONE_ENDED,
};

struct v34_phase2_tone_event {
	enum v34_phase2_tone_event_kind kind;
	/* For PHASE_REVERSED, samples elapsed between the estimated line edge
	 * and this event. A controller can subtract this from its response
	 * deadline instead of adding detector latency to round-trip timing. */
	uint16_t reversal_delay_samples;
};

struct v34_phase2_tone_detector {
	struct v34_phase2_tone_observation
		history[V34_PHASE2_TONE_WINDOW_SAMPLES];
	double in_phase;
	double quadrature;
	double line_energy;
	double reference_in_phase;
	double reference_quadrature;
	double line_power;
	double carrier_power;
	double carrier_share;
	uint32_t carrier_phase;
	uint32_t carrier_phase_step;
	size_t history_index;
	size_t history_count;
	uint16_t qualifying_samples;
	uint16_t missing_samples;
	uint16_t reversal_delay_samples;
	enum v34_station_role local_role;
	bool measurement_ready;
	bool tone_present;
	bool reversal_pending;
};

struct v34_phase2_tone_measurement {
	double line_power;
	double carrier_power;
	double carrier_share;
	bool ready;
	bool tone_present;
};

/* Tone B is the calling station's 1200 Hz carrier. Tone A is the answering
 * station's 2400 Hz carrier at -1 dB plus a continuous 1800 Hz guard at the
 * nominal level. nominal_amplitude is the peak level before those gains. */
enum v34_phase2_tone_result
v34_phase2_tone_transmitter_init(
	struct v34_phase2_tone_transmitter *transmitter,
	enum v34_station_role local_role,
	int16_t nominal_amplitude);
bool v34_phase2_tone_transmitter_is_valid(
	const struct v34_phase2_tone_transmitter *transmitter);

/* Reverse only the carrier. The answering station's guard remains phase
 * continuous, as required for transitions between Tone A states. */
enum v34_phase2_tone_result
v34_phase2_tone_transmitter_reverse(
	struct v34_phase2_tone_transmitter *transmitter);
enum v34_phase2_tone_result
v34_phase2_tone_transmitter_sample(
	struct v34_phase2_tone_transmitter *transmitter,
	int16_t *sample);

/* local_role selects the opposite station's expected Tone A or Tone B. */
enum v34_phase2_tone_result
v34_phase2_tone_detector_init(
	struct v34_phase2_tone_detector *detector,
	enum v34_station_role local_role);
bool v34_phase2_tone_detector_is_valid(
	const struct v34_phase2_tone_detector *detector);
enum v34_phase2_tone_result
v34_phase2_tone_detector_reset(
	struct v34_phase2_tone_detector *detector);
enum v34_phase2_tone_result
v34_phase2_tone_detector_sample(
	struct v34_phase2_tone_detector *detector,
	int16_t sample, struct v34_phase2_tone_event *event);
struct v34_phase2_tone_measurement
v34_phase2_tone_detector_measure(
	const struct v34_phase2_tone_detector *detector);

const char *v34_phase2_tone_result_name(
	enum v34_phase2_tone_result result);

#endif
