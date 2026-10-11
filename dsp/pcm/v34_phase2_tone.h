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
	V34_RETRAIN_SILENCE_MAX_WINDOW_ENERGY = 9600000,
	/* Full retrain transmits 70 +/- 5 ms silence before role-selected
	 * Tone A/B; V.34 11.5, V.90 9.5, and V.92 9.7 require a response
	 * after more than 50 ms of tone.
	 * Accept 60 ms observed quiet, with one sample at each edge, to tolerate
	 * short observed peer preambles. Keep an 80 ms continuous tone guard
	 * to prevent ordinary QAM training from looking like a retrain request.
	 * A short line impulse contaminates the
	 * trailing silence window after the impulse itself; retain otherwise
	 * qualifying silence across at most a 2 ms impulse.  Permit a two-window
	 * codec-boundary onset transition and one spectral window for it to age
	 * out before tone acquisition. */
	V34_RETRAIN_MIN_SILENCE_SAMPLES =
		V34_PHASE2_SAMPLE_RATE * 60 / 1000 - 2,
	V34_RETRAIN_MAX_SILENCE_GLITCH_SAMPLES =
		V34_RETRAIN_SILENCE_WINDOW_SAMPLES +
		V34_PHASE2_SAMPLE_RATE * 2 / 1000,
	V34_RETRAIN_TONE_SAMPLES =
		V34_PHASE2_SAMPLE_RATE * 80 / 1000,
	V34_RETRAIN_EDGE_SAMPLES =
		V34_PHASE2_TONE_WINDOW_SAMPLES * 3 +
		V34_PHASE2_TONE_ACQUIRE_SAMPLES,
	/* V.34, V.90, and V.92 use the same full-retrain response: 70 ms
	 * of silence before the role-selected Phase 2 tone. */
	V34_FULL_RETRAIN_RESPONSE_SILENCE_SAMPLES =
		V34_PHASE2_SAMPLE_RATE * 70 / 1000,
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
 * additionally needs a qualified 1800 Hz answer guard.
 * The detector is instance-owned and reusable by V.34, V.90, and V.92 call
 * controllers, whose full-retrain preamble is the same. */
struct v34_retrain_detector {
	struct v34_phase2_tone_detector tone;
	struct single_tone_detector answer_guard;
	uint64_t silence_energy;
	/* Raw energy and signed samples retain a DC-independent quiet measure. */
	int16_t silence_history[V34_RETRAIN_SILENCE_WINDOW_SAMPLES];
	int32_t silence_sum;
	size_t silence_history_index;
	size_t silence_history_count;
	uint16_t silence_samples;
	uint16_t silence_glitch_samples;
	uint16_t tone_samples;
	uint16_t edge_samples;
};

/* Own the shared full-retrain request detector and response-silence epoch.
 * A peer-initiated response retains the qualified Tone A/B phase so the
 * restarted Phase 2 receiver can observe the following reversal. */
struct v34_full_retrain {
	struct v34_retrain_detector detector;
	uint16_t response_silence_samples_remaining;
	bool peer_tone_retained;
};

enum v34_full_retrain_response_event {
	V34_FULL_RETRAIN_RESPONSE_CONTINUES,
	V34_FULL_RETRAIN_RESPONSE_COMPLETES,
	V34_FULL_RETRAIN_RESPONSE_COMPLETES_WITH_PEER_TONE,
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

/* Recover the nominal level of a valid transmitter for a phase handoff.
 * Tone A's carrier is attenuated; its guard retains the nominal level. */
static inline int16_t v34_phase2_tone_nominal_amplitude(
	const struct v34_phase2_tone_transmitter *transmitter)
{
	return transmitter->local_role == V34_CALLING_STATION ?
		transmitter->carrier_amplitude : transmitter->guard_amplitude;
}

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
/* Adopt a positive Q10 gain qualified by the receive owner. Only the raw
 * power floor changes; histories, reference phase, carrier shares and timing
 * remain intact. This does not run or reset an AGC. */
enum v34_phase2_tone_result
v34_phase2_tone_detector_set_receive_gain(
	struct v34_phase2_tone_detector *detector, int16_t receive_gain);
/* Reset clears observations while retaining the instance's receive level.
 * Initialization restores the default level. Copies retain both level and
 * raw physical reference phase. */
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
/* A trained receiver may scale raw silence and tone power to its signal level.
 * The inclusive range is 1..V34_RETRAIN_SILENCE_MAX_WINDOW_ENERGY. This affects
 * silence qualification/expiry and proportionally lowers the raw tone-power
 * floor; carrier shares and every timing bound remain unchanged. The selected
 * level follows retained Tone A/B through response and Phase 2 handoff/reset.
 * The ordinary sample entry point restores the default receive level.
 * Invalid input leaves the detector and detected output unchanged. */
enum v34_phase2_tone_result v34_retrain_detector_sample_with_power_limit(
	struct v34_retrain_detector *detector, int16_t sample,
	uint32_t maximum_window_energy, bool *detected);

enum v34_phase2_tone_result v34_full_retrain_init(
	struct v34_full_retrain *retrain,
	enum v34_station_role local_role);
bool v34_full_retrain_is_valid(
	const struct v34_full_retrain *retrain);
bool v34_full_retrain_response_is_active(
	const struct v34_full_retrain *retrain);
/* retain_peer_tone is true only after detector emitted a peer request. */
enum v34_phase2_tone_result v34_full_retrain_begin_response(
	struct v34_full_retrain *retrain, bool retain_peer_tone);
/* On COMPLETE_WITH_PEER_TONE, detector.tone is the phase-continuous state
 * to graft into the restarted Phase 2 receiver. Tone loss during the response
 * instead emits COMPLETE on the original deadline, requiring reacquisition. */
enum v34_phase2_tone_result v34_full_retrain_sample_response(
	struct v34_full_retrain *retrain, int16_t received_sample,
	enum v34_full_retrain_response_event *event);

#endif
