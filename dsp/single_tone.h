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
	/* Profiles may use a shorter window while retaining bounded ownership. */
	SINGLE_TONE_MAX_WINDOW_SAMPLES =
		SINGLE_TONE_WINDOW_SAMPLES,
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

enum single_tone_interruption {
	SINGLE_TONE_UNINTERRUPTED,
	SINGLE_TONE_SUPPRESSING,
	SINGLE_TONE_REACQUIRING,
};

struct single_tone_detector_profile {
	double minimum_line_power;
	double minimum_carrier_share;
	double maximum_carrier_share;
	uint16_t sample_rate_hz;
	uint16_t frequency_hz;
	uint16_t window_samples;
	uint16_t acquire_samples;
	uint16_t loss_samples;
	bool retain_reference_on_loss;
};

struct single_tone_detector {
	struct single_tone_observation
		history[SINGLE_TONE_MAX_WINDOW_SAMPLES];
	struct single_tone_detector_profile profile;
	double in_phase;
	double quadrature;
	double line_energy;
	double reference_in_phase;
	double reference_quadrature;
	uint32_t phase;
	uint32_t phase_step;
	size_t history_index;
	size_t history_count;
	uint16_t qualifying_samples;
	uint16_t missing_samples;
	uint16_t reversal_delay_samples;
	enum single_tone_interruption interruption;
	bool tone_present;
	bool reversal_pending;
	bool rejected_reversal_active;
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
enum single_tone_result single_tone_detector_init_profile(
	struct single_tone_detector *detector,
	const struct single_tone_detector_profile *profile);
bool single_tone_detector_is_valid(
	const struct single_tone_detector *detector);
enum single_tone_result single_tone_detector_reset(
	struct single_tone_detector *detector);
/* event must be separate from detector; rejection changes neither. */
enum single_tone_result single_tone_detector_sample(
	struct single_tone_detector *detector, int16_t sample,
	struct single_tone_event *event);
/* For an owner which has validated the complete detector at this sample
 * boundary. Recheck metadata and the replaced observation, preserving all
 * numeric rejection, but do not rescan unchanged history. Return the event
 * and post-update measurement together. Outputs must be separate from each
 * other and the detector; rejection changes neither state nor outputs. */
enum single_tone_result single_tone_detector_observe_owned(
	struct single_tone_detector *detector, int16_t sample,
	struct single_tone_event *event,
	struct single_tone_measurement *measurement);
enum single_tone_result single_tone_detector_reject_reversal(
	struct single_tone_detector *detector,
	const struct single_tone_event *event);
enum single_tone_result single_tone_detector_suppress_sample(
	struct single_tone_detector *detector);
enum single_tone_result single_tone_detector_resume(
	struct single_tone_detector *detector,
	uint16_t handoff_delay_samples);
struct single_tone_measurement
single_tone_detector_measure(
	const struct single_tone_detector *detector);


#endif
