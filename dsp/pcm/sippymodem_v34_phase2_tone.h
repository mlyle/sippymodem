// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_PHASE2_TONE_H
#define SIPPYMODEM_V34_PHASE2_TONE_H

#include "sippymodem_v34_phase2_signal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	/* Five milliseconds contains whole cycles of both Phase 2 carriers and
	 * exactly three cycles of their 600 Hz separation. */
	SIPPYMODEM_V34_PHASE2_TONE_WINDOW_SAMPLES =
		SIPPYMODEM_V34_PHASE2_SAMPLE_RATE * 5 / 1000,
	SIPPYMODEM_V34_PHASE2_TONE_ACQUIRE_SAMPLES =
		SIPPYMODEM_V34_PHASE2_TONE_WINDOW_SAMPLES * 2,
	SIPPYMODEM_V34_PHASE2_TONE_LOSS_SAMPLES =
		SIPPYMODEM_V34_PHASE2_TONE_WINDOW_SAMPLES * 2,
};

enum sippymodem_v34_phase2_tone_result {
	SIPPYMODEM_V34_PHASE2_TONE_OK,
	SIPPYMODEM_V34_PHASE2_TONE_INVALID_ARGUMENT,
};

struct sippymodem_v34_phase2_tone_transmitter {
	uint32_t carrier_phase;
	uint32_t carrier_phase_step;
	uint32_t guard_phase;
	uint32_t guard_phase_step;
	int16_t carrier_amplitude;
	int16_t guard_amplitude;
	enum sippymodem_v34_station_role local_role;
};

struct sippymodem_v34_phase2_tone_observation {
	double in_phase;
	double quadrature;
	double energy;
};

enum sippymodem_v34_phase2_tone_event_kind {
	SIPPYMODEM_V34_PHASE2_TONE_NO_EVENT,
	SIPPYMODEM_V34_PHASE2_TONE_STARTED,
	SIPPYMODEM_V34_PHASE2_TONE_PHASE_REVERSED,
	SIPPYMODEM_V34_PHASE2_TONE_ENDED,
};

struct sippymodem_v34_phase2_tone_event {
	enum sippymodem_v34_phase2_tone_event_kind kind;
	/* For PHASE_REVERSED, samples elapsed between the estimated line edge
	 * and this event. A controller can subtract this from its response
	 * deadline instead of adding detector latency to round-trip timing. */
	uint16_t reversal_delay_samples;
};

struct sippymodem_v34_phase2_tone_detector {
	struct sippymodem_v34_phase2_tone_observation
		history[SIPPYMODEM_V34_PHASE2_TONE_WINDOW_SAMPLES];
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
	enum sippymodem_v34_station_role local_role;
	bool measurement_ready;
	bool tone_present;
	bool reversal_pending;
};

struct sippymodem_v34_phase2_tone_measurement {
	double line_power;
	double carrier_power;
	double carrier_share;
	bool ready;
	bool tone_present;
};

/* Tone B is the calling station's 1200 Hz carrier. Tone A is the answering
 * station's 2400 Hz carrier at -1 dB plus a continuous 1800 Hz guard at the
 * nominal level. nominal_amplitude is the peak level before those gains. */
enum sippymodem_v34_phase2_tone_result
sippymodem_v34_phase2_tone_transmitter_init(
	struct sippymodem_v34_phase2_tone_transmitter *transmitter,
	enum sippymodem_v34_station_role local_role,
	int16_t nominal_amplitude);
bool sippymodem_v34_phase2_tone_transmitter_is_valid(
	const struct sippymodem_v34_phase2_tone_transmitter *transmitter);

/* Reverse only the carrier. The answering station's guard remains phase
 * continuous, as required for transitions between Tone A states. */
enum sippymodem_v34_phase2_tone_result
sippymodem_v34_phase2_tone_transmitter_reverse(
	struct sippymodem_v34_phase2_tone_transmitter *transmitter);
enum sippymodem_v34_phase2_tone_result
sippymodem_v34_phase2_tone_transmitter_sample(
	struct sippymodem_v34_phase2_tone_transmitter *transmitter,
	int16_t *sample);

/* local_role selects the opposite station's expected Tone A or Tone B. */
enum sippymodem_v34_phase2_tone_result
sippymodem_v34_phase2_tone_detector_init(
	struct sippymodem_v34_phase2_tone_detector *detector,
	enum sippymodem_v34_station_role local_role);
bool sippymodem_v34_phase2_tone_detector_is_valid(
	const struct sippymodem_v34_phase2_tone_detector *detector);
enum sippymodem_v34_phase2_tone_result
sippymodem_v34_phase2_tone_detector_reset(
	struct sippymodem_v34_phase2_tone_detector *detector);
enum sippymodem_v34_phase2_tone_result
sippymodem_v34_phase2_tone_detector_sample(
	struct sippymodem_v34_phase2_tone_detector *detector,
	int16_t sample, struct sippymodem_v34_phase2_tone_event *event);
struct sippymodem_v34_phase2_tone_measurement
sippymodem_v34_phase2_tone_detector_measure(
	const struct sippymodem_v34_phase2_tone_detector *detector);

const char *sippymodem_v34_phase2_tone_result_name(
	enum sippymodem_v34_phase2_tone_result result);

#endif
