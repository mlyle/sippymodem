// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_HOST_ECHO_H
#define PCM_HOST_ECHO_H

#include "pcm_echo_reference.h"

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
	/* A 2 kHz correlation stream still acquires after the retained-history
	 * fill and leaves the retimed filter its required 250 ms in TRN1d. */
	PCM_HOST_ECHO_SCAN_INTERVAL_SAMPLES = 4,
	PCM_HOST_ECHO_READY_MIN_PEAK = 64,
	/* Requalify small host timing changes without adapting to peer data. */
	PCM_HOST_ECHO_TIMING_RADIUS = PCM_ECHO_REFERENCE_SAMPLE_RATE / 50,
	PCM_HOST_ECHO_TIMING_BINS = 2 * PCM_HOST_ECHO_TIMING_RADIUS + 1,
	PCM_HOST_ECHO_TIMING_WINDOW = 8192,
};

_Static_assert((unsigned)PCM_HOST_ECHO_FILTER_TAPS ==
		       (unsigned)PCM_ECHO_REFERENCE_MAX_WINDOW_SAMPLES,
	       "PCM host echo filter must fill the retained PCM window");

enum pcm_host_echo_mode {
	PCM_HOST_ECHO_BYPASS,
	PCM_HOST_ECHO_TRAIN,
	PCM_HOST_ECHO_FILTER,
};

enum pcm_host_echo_result {
	PCM_HOST_ECHO_OK,
	PCM_HOST_ECHO_INVALID_ARGUMENT,
	PCM_HOST_ECHO_INVALID_STATE,
	PCM_HOST_ECHO_SEQUENCE_MISMATCH,
	PCM_HOST_ECHO_NUMERIC_FAILURE,
};

struct pcm_host_echo_timing_evidence {
	int64_t cross, current_cross;
	uint64_t energy, current_energy, input;
};

struct pcm_host_echo_timing {
	int32_t prediction[PCM_HOST_ECHO_TIMING_BINS];
	int16_t received[PCM_HOST_ECHO_TIMING_BINS];
	int64_t cross[PCM_HOST_ECHO_TIMING_BINS];
	uint64_t energy[PCM_HOST_ECHO_TIMING_BINS];
	struct pcm_host_echo_timing_evidence first;
	uint64_t input;
	uint16_t write, fill, count, candidate;
	bool second, first_qualified;
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
	struct pcm_host_echo_timing timing;
};

enum pcm_host_echo_result pcm_host_echo_init(
	struct pcm_host_echo *echo);

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

/* Prediction is pure. TRAIN retains raw input while learning. FILTER keeps
 * coefficients frozen; independently qualified timing changes may reposition
 * the model, but cannot learn the peer's signal as a new echo response. */
enum pcm_host_echo_result pcm_host_echo_predict(
	const struct pcm_host_echo *echo, enum pcm_host_echo_mode mode,
	int16_t received_sample, int16_t *cleaned_sample);

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
