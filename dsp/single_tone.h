// SPDX-License-Identifier: BSD-3-Clause

#ifndef SINGLE_TONE_H
#define SINGLE_TONE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	SINGLE_TONE_SAMPLE_RATE = 8000,
	/* Ten milliseconds contains whole cycles of the V.32 600, 1800,
	 * 2100 and 3000 Hz startup carriers. */
	SINGLE_TONE_WINDOW_SAMPLES =
		SINGLE_TONE_SAMPLE_RATE / 100,
	SINGLE_TONE_ACQUIRE_SAMPLES =
		SINGLE_TONE_WINDOW_SAMPLES * 2,
	SINGLE_TONE_LOSS_SAMPLES =
		SINGLE_TONE_WINDOW_SAMPLES * 2,
};

enum single_tone_result {
	SINGLE_TONE_OK,
	SINGLE_TONE_INVALID_ARGUMENT,
};

struct single_tone_transmitter {
	uint32_t phase;
	uint32_t phase_step;
	uint16_t frequency_hz;
	int16_t amplitude;
};

struct single_tone_observation {
	double in_phase;
	double quadrature;
	double energy;
};

enum single_tone_event_kind {
	SINGLE_TONE_NO_EVENT,
	SINGLE_TONE_STARTED,
	SINGLE_TONE_PHASE_REVERSED,
	SINGLE_TONE_ENDED,
};

struct single_tone_event {
	enum single_tone_event_kind kind;
	/* Estimated samples between the line reversal and this event. */
	uint16_t reversal_delay_samples;
};

struct single_tone_detector {
	struct single_tone_observation
		history[SINGLE_TONE_WINDOW_SAMPLES];
	double in_phase;
	double quadrature;
	double line_energy;
	double reference_in_phase;
	double reference_quadrature;
	double line_power;
	double carrier_power;
	double carrier_share;
	double minimum_carrier_share;
	uint32_t phase;
	uint32_t phase_step;
	size_t history_index;
	size_t history_count;
	uint16_t frequency_hz;
	uint16_t qualifying_samples;
	uint16_t missing_samples;
	uint16_t reversal_delay_samples;
	bool measurement_ready;
	bool tone_present;
	bool reversal_pending;
};

struct single_tone_measurement {
	double line_power;
	double carrier_power;
	double carrier_share;
	bool ready;
	bool tone_present;
};

enum single_tone_result
single_tone_transmitter_init(
	struct single_tone_transmitter *transmitter,
	unsigned int frequency_hz, int16_t amplitude);
bool single_tone_transmitter_is_valid(
	const struct single_tone_transmitter *transmitter);
enum single_tone_result
single_tone_transmitter_reverse(
	struct single_tone_transmitter *transmitter);
enum single_tone_result
single_tone_transmitter_sample(
	struct single_tone_transmitter *transmitter,
	int16_t *sample);

enum single_tone_result single_tone_detector_init(
	struct single_tone_detector *detector,
	unsigned int frequency_hz);
enum single_tone_result
single_tone_detector_init_with_minimum_carrier_share(
	struct single_tone_detector *detector,
	unsigned int frequency_hz, double minimum_carrier_share);
bool single_tone_detector_is_valid(
	const struct single_tone_detector *detector);
enum single_tone_result single_tone_detector_reset(
	struct single_tone_detector *detector);
enum single_tone_result single_tone_detector_sample(
	struct single_tone_detector *detector, int16_t sample,
	struct single_tone_event *event);
struct single_tone_measurement
single_tone_detector_measure(
	const struct single_tone_detector *detector);

const char *single_tone_result_name(
	enum single_tone_result result);

#endif
