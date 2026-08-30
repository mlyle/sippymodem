// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_HOST_ECHO_H
#define V90_HOST_ECHO_H

#include "pcm_echo_reference.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V90_HOST_ECHO_FILTER_PRE_SAMPLES = 64,
	V90_HOST_ECHO_FILTER_TAPS = 384,
	V90_HOST_ECHO_DEFAULT_RETURN_DELAY_SAMPLES =
		PCM_ECHO_REFERENCE_SAMPLE_RATE * 322 / 1000,
	V90_HOST_ECHO_DEFAULT_WINDOW_DELAY_SAMPLES =
		V90_HOST_ECHO_DEFAULT_RETURN_DELAY_SAMPLES -
		V90_HOST_ECHO_FILTER_PRE_SAMPLES,
	V90_HOST_ECHO_QUIET_SAMPLES = PCM_ECHO_REFERENCE_SAMPLE_RATE / 100,
	V90_HOST_ECHO_TRAIN_MIN_SAMPLES =
		PCM_ECHO_REFERENCE_SAMPLE_RATE * 3 / 8,
	V90_HOST_ECHO_RETIMED_TRAIN_MIN_SAMPLES =
		PCM_ECHO_REFERENCE_SAMPLE_RATE / 4,
	V90_HOST_ECHO_TRAIN_WINDOW_SAMPLES =
		PCM_ECHO_REFERENCE_SAMPLE_RATE / 2,
	/* A 2 kHz correlation stream still acquires after the retained-history
	 * fill and leaves the retimed filter its required 250 ms in TRN1d. */
	V90_HOST_ECHO_SCAN_INTERVAL_SAMPLES = 4,
	V90_HOST_ECHO_READY_MIN_PEAK = 64,
};

_Static_assert((unsigned)V90_HOST_ECHO_FILTER_TAPS ==
		       (unsigned)PCM_ECHO_REFERENCE_MAX_WINDOW_SAMPLES,
	       "V.90 host echo filter must fill the retained PCM window");

enum v90_host_echo_mode {
	V90_HOST_ECHO_BYPASS,
	V90_HOST_ECHO_TRAIN,
	V90_HOST_ECHO_FILTER,
};

enum v90_host_echo_result {
	V90_HOST_ECHO_OK,
	V90_HOST_ECHO_INVALID_ARGUMENT,
	V90_HOST_ECHO_INVALID_STATE,
	V90_HOST_ECHO_SEQUENCE_MISMATCH,
	V90_HOST_ECHO_NUMERIC_FAILURE,
};

struct v90_host_echo {
	struct pcm_echo_reference reference;
	float coefficients[V90_HOST_ECHO_FILTER_TAPS];
	uint64_t reference_energy;
	uint64_t input_energy;
	uint64_t error_energy;
	int64_t cross_energy;
	uint32_t window_delay_samples;
	uint32_t quiet_samples;
	uint32_t train_samples;
	uint16_t train_peak;
	bool delay_acquired;
	bool filter_ready;
	bool weak_margin;
};

enum v90_host_echo_result v90_host_echo_init(
	struct v90_host_echo *echo);
bool v90_host_echo_is_valid(const struct v90_host_echo *echo);
bool v90_host_echo_is_ready(const struct v90_host_echo *echo);
bool v90_host_echo_has_weak_margin(const struct v90_host_echo *echo);
uint32_t v90_host_echo_window_delay_samples(
	const struct v90_host_echo *echo);

/* Prediction is pure.  TRAIN retains the raw sample while learning; FILTER
 * subtracts only a model that passed the owned readiness evidence. */
enum v90_host_echo_result v90_host_echo_predict(
	const struct v90_host_echo *echo, enum v90_host_echo_mode mode,
	int16_t received_sample, int16_t *cleaned_sample);

/* Verify and advance exactly one prediction, then clock the actual host-rate
 * transmit sample into the delayed reference history. */
enum v90_host_echo_result v90_host_echo_advance(
	struct v90_host_echo *echo, enum v90_host_echo_mode mode,
	int16_t received_sample, int16_t cleaned_sample,
	int16_t transmitted_sample);

#endif /* V90_HOST_ECHO_H */
