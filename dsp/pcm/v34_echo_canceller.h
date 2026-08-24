// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_ECHO_CANCELLER_H
#define V34_ECHO_CANCELLER_H

#include "v34_phase2_signal.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_ECHO_TAPS = 144,
	V34_ECHO_PRE_TAPS = 64,
	V34_ECHO_REFERENCE_SAMPLES = V34_PHASE2_SAMPLE_RATE * 2,
	V34_ECHO_MIN_DELAY_SAMPLES = 1,
	V34_ECHO_MAX_DELAY_SAMPLES =
		V34_ECHO_REFERENCE_SAMPLES -
		(V34_ECHO_TAPS - V34_ECHO_PRE_TAPS),
	V34_ECHO_QUIET_SAMPLES = V34_PHASE2_SAMPLE_RATE / 100,
	V34_ECHO_TRAIN_SAMPLES = V34_PHASE2_SAMPLE_RATE * 3 / 8,
	V34_ECHO_VALIDATE_SAMPLES = V34_PHASE2_SAMPLE_RATE / 8,
};

enum v34_echo_result {
	V34_ECHO_OK,
	V34_ECHO_INVALID_ARGUMENT,
	V34_ECHO_INVALID_STATE,
};

enum v34_echo_mode {
	V34_ECHO_DISABLED,
	V34_ECHO_TRAINING,
	V34_ECHO_ACTIVE,
};

/* A prepared sample binds a pure receive-side prediction to the unchanged
 * owner. The caller may process another transactional owner before committing
 * the generated transmit reference. */
struct v34_echo_prepared_sample {
	uint64_t epoch;
	double scale;
	double input_energy;
	double error_energy;
	uint32_t input_magnitude;
	uint8_t quiet_samples;
	uint8_t action;
};

/* Own one bounded 8 kHz transmit-reference timeline and a real sample-domain
 * near-echo predictor. The 144-tap window places 64 taps before the Phase 2
 * round-trip estimate so normal filter and transport offsets remain inside
 * the learned path. Coefficients are trained only during the local L1/L2
 * probe, validated on a disjoint frozen window, and then retained through
 * Phase 3, data, and full retrain. */
struct v34_echo_canceller {
	int16_t reference[V34_ECHO_REFERENCE_SAMPLES];
	double coefficients[V34_ECHO_TAPS];
	double validation_input_energy;
	double validation_error_energy;
	uint64_t epoch;
	uint32_t reference_write;
	uint32_t reference_fill;
	uint32_t train_samples;
	uint32_t validation_samples;
	uint32_t training_peak;
	uint16_t round_trip_delay_samples;
	uint8_t pre_taps;
	uint8_t quiet_samples;
	enum v34_echo_mode mode;
};

enum v34_echo_result v34_echo_canceller_init(
	struct v34_echo_canceller *canceller);
bool v34_echo_canceller_is_valid(
	const struct v34_echo_canceller *canceller);

/* Reset only the learned model and start a new probe-owned training epoch.
 * The continuous transmit-reference ring and its epoch are preserved. */
enum v34_echo_result v34_echo_canceller_start_training(
	struct v34_echo_canceller *canceller,
	uint16_t round_trip_delay_samples);

/* Freeze and activate a proven model, or return to the canonical disabled
 * state when the probe did not provide enough independent evidence. */
enum v34_echo_result v34_echo_canceller_finish_training(
	struct v34_echo_canceller *canceller);

/* Prepare one receive sample without changing owner or outputs on failure.
 * Training deliberately returns the raw sample; cancellation begins only
 * after the frozen validation gate succeeds. */
enum v34_echo_result v34_echo_canceller_prepare(
	const struct v34_echo_canceller *canceller, int16_t received_sample,
	int16_t *cleaned_sample,
	struct v34_echo_prepared_sample *prepared);

/* Commit a preparation against the unchanged owner and append the transmit
 * sample from the same duplex interval. */
enum v34_echo_result v34_echo_canceller_commit(
	struct v34_echo_canceller *canceller, int16_t transmit_sample,
	const struct v34_echo_prepared_sample *prepared);

#endif /* V34_ECHO_CANCELLER_H */
