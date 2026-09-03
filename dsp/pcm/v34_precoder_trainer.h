// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PRECODER_TRAINER_H
#define V34_PRECODER_TRAINER_H

#include "qam.h"
#include "v34_precoder.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	/* Leave the feed-forward equalizer time to settle before judging whether
	 * the residual decision error has useful predictable memory. */
	V34_PRECODER_TRAINER_SETTLING_SYMBOLS = 128,
	V34_PRECODER_TRAINER_MINIMUM_MEASURED_SYMBOLS = 256,
};

enum v34_precoder_trainer_result {
	V34_PRECODER_TRAINER_OK,
	V34_PRECODER_TRAINER_INVALID_ARGUMENT,
	V34_PRECODER_TRAINER_INVALID_STATE,
	V34_PRECODER_TRAINER_NUMERIC_FAILURE,
};

/* A disabled recommendation is represented canonically by all-zero
 * coefficients. This makes a zero-initialized Phase 3 fixture a valid
 * no-precoder handoff. */
struct v34_precoder_recommendation {
	struct v34_q14_coefficient
		coefficients[V34_PRECODER_TAP_COUNT];
	bool enabled;
};

/* Learn the three-tap decision-error predictor used to request transmitter
 * precoding. Coefficients are stored in protocol h(1)..h(3) order and the
 * history is e(n-1)..e(n-3). */
struct v34_precoder_trainer {
	struct qam_point coefficients[V34_PRECODER_TAP_COUNT];
	struct qam_point error_history[V34_PRECODER_TAP_COUNT];
	double decision_error_power;
	double feedback_error_power;
	uint32_t observed_symbols;
	uint32_t measured_symbols;
	bool initialized;
};

bool v34_precoder_recommendation_is_valid(
	const struct v34_precoder_recommendation *recommendation);
bool v34_precoder_trainer_is_valid(
	const struct v34_precoder_trainer *trainer);

enum v34_precoder_trainer_result v34_precoder_trainer_init(
	struct v34_precoder_trainer *trainer);

/* Observe one equalized decision and its semantic target. The update follows
 * the V.34 decision-feedback learner: predict the current error from the
 * preceding three errors, then adapt the complex predictor toward zero
 * residual. Rejected calls leave the trainer unchanged. */
enum v34_precoder_trainer_result v34_precoder_trainer_observe(
	struct v34_precoder_trainer *trainer,
	const struct qam_point *decision,
	const struct qam_point *target);

/* Quantize a useful, stable predictor as a Type-1 MP recommendation. A weak,
 * noisy, or insufficiently measured predictor succeeds with the canonical
 * disabled recommendation. */
enum v34_precoder_trainer_result v34_precoder_trainer_finish(
	const struct v34_precoder_trainer *trainer,
	struct v34_precoder_recommendation *recommendation);

#endif /* V34_PRECODER_TRAINER_H */
