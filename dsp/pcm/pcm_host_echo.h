// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_HOST_ECHO_H
#define PCM_HOST_ECHO_H

#include "pcm_echo_reference.h"
#include "echo_timing.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	PCM_HOST_ECHO_FILTER_PRE_SAMPLES = 64,
	PCM_HOST_ECHO_FILTER_TAPS = 384,
	PCM_HOST_ECHO_DEFAULT_RETURN_DELAY_SAMPLES =
		PCM_ECHO_REFERENCE_SAMPLE_RATE * 322 / 1000,
	PCM_HOST_ECHO_DEFAULT_WINDOW_DELAY_SAMPLES =
		PCM_HOST_ECHO_DEFAULT_RETURN_DELAY_SAMPLES -
		PCM_HOST_ECHO_FILTER_PRE_SAMPLES,
	PCM_HOST_ECHO_QUIET_SAMPLES = PCM_ECHO_REFERENCE_SAMPLE_RATE / 100,
	PCM_HOST_ECHO_TRAIN_MIN_SAMPLES =
		PCM_ECHO_REFERENCE_SAMPLE_RATE * 3 / 8,
	PCM_HOST_ECHO_RETIMED_TRAIN_MIN_SAMPLES =
		PCM_ECHO_REFERENCE_SAMPLE_RATE / 4,
	PCM_HOST_ECHO_TRAIN_WINDOW_SAMPLES =
		PCM_ECHO_REFERENCE_SAMPLE_RATE / 2,
	/* Find the first return at 4 kHz so its filter has time to qualify.
	 * Once a delay is acquired or a filter is ready, continue at 2 kHz. */
	PCM_HOST_ECHO_ACQUIRE_SCAN_INTERVAL_SAMPLES = 2,
	PCM_HOST_ECHO_SCAN_INTERVAL_SAMPLES = 4,
	PCM_HOST_ECHO_READY_MIN_PEAK = 64,
	PCM_HOST_ECHO_READY_MIN_RMS = 32,

};

_Static_assert((unsigned)PCM_HOST_ECHO_FILTER_TAPS ==
		       (unsigned)PCM_ECHO_REFERENCE_MAX_WINDOW_SAMPLES,
	       "PCM host echo filter must fill the retained PCM window");

enum pcm_host_echo_mode {
	PCM_HOST_ECHO_BYPASS,
	PCM_HOST_ECHO_TRAIN,
	PCM_HOST_ECHO_FILTER,
	PCM_HOST_ECHO_FILTER_DATA,
};

enum pcm_host_echo_result {
	PCM_HOST_ECHO_OK,
	PCM_HOST_ECHO_INVALID_ARGUMENT,
	PCM_HOST_ECHO_INVALID_STATE,
	PCM_HOST_ECHO_SEQUENCE_MISMATCH,
	PCM_HOST_ECHO_NUMERIC_FAILURE,
};

struct pcm_host_echo {
	struct pcm_echo_reference reference;
	float coefficients[PCM_HOST_ECHO_FILTER_TAPS];
	/* Preserve qualified cancellation while an earlier window acquires. */
	float retained_coefficients[PCM_HOST_ECHO_FILTER_TAPS];
	uint32_t retained_delay_samples;
	/* Correlate input against predictions made before fitting that input. */
	uint64_t prediction_energy;
	uint64_t input_energy;
	uint64_t error_energy;
	int64_t prediction_cross_energy;
	uint32_t window_delay_samples;
	uint32_t quiet_samples;
	uint32_t train_samples;
	uint16_t train_peak;
	bool delay_acquired;
	bool filter_ready;
	bool weak_margin;
	bool filter_suspended;
	struct echo_timing timing;
};

enum pcm_host_echo_result pcm_host_echo_init(
	struct pcm_host_echo *echo);

/* Select an initial window without assuming that the echo occurs there.
 * History may already be present, but acquisition must not have begun. */
enum pcm_host_echo_result pcm_host_echo_seed_window_delay(
	struct pcm_host_echo *echo, uint32_t window_delay_samples);

/* Refine the initial delay hypothesis from the shared Phase-2 ranging result.
 * Transmit history may already be present, but training must not have begun. */
enum pcm_host_echo_result pcm_host_echo_seed_phase2_delay(
	struct pcm_host_echo *echo, uint32_t round_trip_delay_samples);
bool pcm_host_echo_is_valid(const struct pcm_host_echo *echo);
bool pcm_host_echo_is_ready(const struct pcm_host_echo *echo);
bool pcm_host_echo_has_weak_margin(const struct pcm_host_echo *echo);
/* Current acquisition window; FILTER can retain a previously qualified model. */
uint32_t pcm_host_echo_window_delay_samples(
	const struct pcm_host_echo *echo);

/* Reposition a qualified frozen model after independently confirmed timing
 * evidence. Preserve its coefficients and TX history; reject an unavailable
 * reference window without mutation. */
enum pcm_host_echo_result pcm_host_echo_retime(
	struct pcm_host_echo *echo, int delay_change_samples);

/* Prediction is pure. TRAIN retains raw input while learning. FILTER keeps
 * coefficients frozen; independently qualified timing changes may reposition
 * the active model, including a retained model whose replacement is still
 * unqualified, but cannot learn the peer's signal as a new echo response.
 * FILTER_DATA suspends unsupported marginal fits and predictions that
 * independently increase input energy. A suspended model is monitored without
 * subtraction until fresh evidence qualifies it again. */
enum pcm_host_echo_result pcm_host_echo_predict(
	const struct pcm_host_echo *echo, enum pcm_host_echo_mode mode,
	int16_t received_sample, int16_t *cleaned_sample);

/* Observe timing once for a successful prediction, before the receiver commits
 * evidence for this sample. No coefficients may change after prediction.
 * A known peer component may be removed from both samples, preserving the
 * frozen echo prediction as received-cleaned. No TX history or coefficients
 * advance. Optional delay_change reports the accepted shift, or zero; invalid
 * calls preserve it. Reset timing when changing the observation's input view.
 * Follow a successful observation with advance_reference, not another observe
 * or advance_predicted. All calls on the owner remain serialized. */
enum pcm_host_echo_result pcm_host_echo_observe_timing(
	struct pcm_host_echo *echo, enum pcm_host_echo_mode mode,
	int16_t received_sample, int16_t cleaned_sample, int *delay_change);

/* Discard timing evidence without changing either filter, its confidence,
 * or TX history. Use at a change of observation coordinates or epoch. */
enum pcm_host_echo_result pcm_host_echo_reset_timing(struct pcm_host_echo *echo);

/* Complete an explicitly observed sample: train on the actual physical input
 * in TRAIN mode, then clock the actual TX sample into reference history.
 * Timing was already observed; this operation does not observe it again. */
enum pcm_host_echo_result pcm_host_echo_advance_reference(
	struct pcm_host_echo *echo, enum pcm_host_echo_mode mode,
	int16_t received_sample, int16_t transmitted_sample);

/* Verify and advance exactly one prediction, then clock the actual host-rate
 * transmit sample into the delayed reference history. */
enum pcm_host_echo_result pcm_host_echo_advance(
	struct pcm_host_echo *echo, enum pcm_host_echo_mode mode,
	int16_t received_sample, int16_t cleaned_sample,
	int16_t transmitted_sample);

/* For the media owner immediately after a successful predict call, with no
 * intervening mutation of echo. The checked advance above remains available
 * to callers that cannot guarantee this ordering. */
enum pcm_host_echo_result pcm_host_echo_advance_predicted(
	struct pcm_host_echo *echo, enum pcm_host_echo_mode mode,
	int16_t received_sample, int16_t cleaned_sample,
	int16_t transmitted_sample);

#endif /* PCM_HOST_ECHO_H */
