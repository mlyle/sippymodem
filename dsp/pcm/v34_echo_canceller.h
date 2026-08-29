// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_ECHO_CANCELLER_H
#define V34_ECHO_CANCELLER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V34_ECHO_TAP_COUNT = 144,
};

enum v34_echo_canceller_result {
	V34_ECHO_CANCELLER_OK,
	V34_ECHO_CANCELLER_INVALID_ARGUMENT,
	V34_ECHO_CANCELLER_INVALID_STATE,
	V34_ECHO_CANCELLER_SEQUENCE_MISMATCH,
};

/*
 * Own one fixed-point adaptive echo window over the preceding 144 samples of
 * one continuous local transmit reference.  Phase 2 round-trip timing belongs
 * to peer response scheduling and is deliberately not used as an echo delay.
 *
 * Prediction is pure.  Advance verifies that prediction and only then updates
 * coefficients and clocks the transmit reference, which lets a surrounding
 * duplex owner preserve source-failure transactionality.
 */
struct v34_echo_canceller {
	int16_t references[V34_ECHO_TAP_COUNT];
	int16_t coefficients[V34_ECHO_TAP_COUNT];
	int16_t residues[V34_ECHO_TAP_COUNT];
	size_t reference_write;
	size_t reference_fill;
	uint64_t validation_input_energy;
	uint64_t validation_residual_energy;
	uint32_t sample_count;
	uint32_t validation_samples;
	int16_t alpha;
	bool training_started;
	bool filtering;
	bool adapting;
};

enum v34_echo_canceller_result v34_echo_canceller_init(
	struct v34_echo_canceller *canceller);
bool v34_echo_canceller_is_valid(
	const struct v34_echo_canceller *canceller);
bool v34_echo_canceller_is_adapting(
	const struct v34_echo_canceller *canceller);

/* Stop coefficient updates.  Retain and apply the model only when completed
 * training evidence shows that it reduced received energy. */
enum v34_echo_canceller_result v34_echo_canceller_freeze(
	struct v34_echo_canceller *canceller);

enum v34_echo_canceller_result v34_echo_canceller_predict(
	const struct v34_echo_canceller *canceller,
	int16_t received_sample, int16_t *cleaned_sample);

/* Advance exactly the prediction identified by received/cleaned, then retain
 * the newly emitted local sample for subsequent predictions. */
enum v34_echo_canceller_result v34_echo_canceller_advance(
	struct v34_echo_canceller *canceller,
	int16_t received_sample, int16_t cleaned_sample,
	int16_t transmit_sample);

#endif /* V34_ECHO_CANCELLER_H */
