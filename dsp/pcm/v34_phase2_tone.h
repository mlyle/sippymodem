// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE2_TONE_H
#define V34_PHASE2_TONE_H

#include "single_tone.h"
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
	V34_RETRAIN_SILENCE_WINDOW_SAMPLES = 64,
	/* V.34 full retrain begins with 70 +/- 5 ms of silence and more than
	 * 50 ms of the role-selected Tone A/B.  Use the lower silence edge and
	 * allow one native sample at each sampled transition of that physical
	 * interval.  An 80 ms tone guard prevents ordinary QAM training from
	 * looking like a retrain request.  A short line impulse contaminates the
	 * trailing silence window after the impulse itself; retain otherwise
	 * qualifying silence across at most a 2 ms impulse.  Permit a two-window
	 * codec-boundary onset transition and one spectral window for it to age
	 * out before tone acquisition. */
	V34_RETRAIN_MIN_SILENCE_SAMPLES =
		V34_PHASE2_SAMPLE_RATE * 65 / 1000 - 2,
	V34_RETRAIN_MAX_SILENCE_GLITCH_SAMPLES =
		V34_RETRAIN_SILENCE_WINDOW_SAMPLES +
		V34_PHASE2_SAMPLE_RATE * 2 / 1000,
	V34_RETRAIN_TONE_SAMPLES =
		V34_PHASE2_SAMPLE_RATE * 80 / 1000,
	V34_RETRAIN_EDGE_SAMPLES =
		V34_PHASE2_TONE_WINDOW_SAMPLES * 3 +
		V34_PHASE2_TONE_ACQUIRE_SAMPLES,
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
	struct single_tone_detector carrier;
	enum v34_station_role local_role;
};

struct v34_phase2_tone_measurement {
	double line_power;
	double carrier_power;
	double carrier_share;
	bool ready;
	bool tone_present;
};

/* Qualify the complete silence-to-Tone-A/B full-retrain request.  Tone A
 * additionally needs the answer-guard energy implied by its carrier share.
 * The detector is instance-owned and reusable by V.34, V.90, and V.92 call
 * controllers, whose full-retrain preamble is the same. */
struct v34_retrain_detector {
	struct v34_phase2_tone_detector tone;
	uint64_t silence_energy;
	uint32_t silence_history[V34_RETRAIN_SILENCE_WINDOW_SAMPLES];
	size_t silence_history_index;
	size_t silence_history_count;
	uint16_t silence_samples;
	uint16_t silence_glitch_samples;
	uint16_t tone_samples;
	uint16_t edge_samples;
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

/* A reversal event commits the new reference for direct consumers. A
 * state-machine owner which rejects that event must call this on the same
 * sample. The rejected phase is then ignored until the retained phase returns,
 * so one stable early edge cannot be reported repeatedly. */
enum v34_phase2_tone_result
v34_phase2_tone_detector_reject_reversal(
	struct v34_phase2_tone_detector *detector,
	const struct v34_phase2_tone_event *event);

/* INFO and the following tone share a carrier. Advance the detector oscillator
 * once for each synchronized INFO sample, then resume with the causal INFO
 * receive-pipeline delay. Zero denotes an undated rejected frame: reacquire
 * from its boundary and report only the ordinary acquisition delay. In both
 * cases the last qualified tone phase remains available for comparison. */
enum v34_phase2_tone_result
v34_phase2_tone_detector_suppress_sample(
	struct v34_phase2_tone_detector *detector);
enum v34_phase2_tone_result
v34_phase2_tone_detector_resume(
	struct v34_phase2_tone_detector *detector,
	uint16_t handoff_delay_samples);
struct v34_phase2_tone_measurement
v34_phase2_tone_detector_measure(
	const struct v34_phase2_tone_detector *detector);

enum v34_phase2_tone_result v34_retrain_detector_init(
	struct v34_retrain_detector *detector,
	enum v34_station_role local_role);
bool v34_retrain_detector_is_valid(
	const struct v34_retrain_detector *detector);
enum v34_phase2_tone_result v34_retrain_detector_reset(
	struct v34_retrain_detector *detector);
/* detected is true for exactly the sample that completes a request. */
enum v34_phase2_tone_result v34_retrain_detector_sample(
	struct v34_retrain_detector *detector, int16_t sample,
	bool *detected);

const char *v34_phase2_tone_result_name(
	enum v34_phase2_tone_result result);

#endif
