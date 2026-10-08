// SPDX-License-Identifier: BSD-3-Clause

#ifndef QAM_GAIN_H
#define QAM_GAIN_H

#include "qam.h"

#include <stdbool.h>

enum qam_gain_result {
	QAM_GAIN_OK,
	QAM_GAIN_INVALID_ARGUMENT,
};

/* A flat complex channel estimate. adaptation is the fraction of each new
 * observed gain incorporated by track(), from zero through one. */
struct qam_gain {
	struct qam_point channel_gain;
	double adaptation;
};

enum qam_gain_result qam_gain_init(
	struct qam_gain *tracker,
	const struct qam_point *channel_gain, double adaptation);
enum qam_gain_result qam_gain_init_from_reference(
	struct qam_gain *tracker,
	const struct qam_point *received,
	const struct qam_point *transmitted, double adaptation);
bool qam_gain_is_valid(
	const struct qam_gain *tracker);

/* Predict the received point for a transmitted point.
 * Rejected calls leave output unchanged. */
enum qam_gain_result qam_gain_predict(
	const struct qam_gain *tracker,
	const struct qam_point *transmitted,
	struct qam_point *received);

/* Update from a nonzero received observation of a known nonzero transmitted
 * point. Rejected calls leave the estimate unchanged. */
enum qam_gain_result qam_gain_track(
	struct qam_gain *tracker,
	const struct qam_point *received,
	const struct qam_point *transmitted);


#endif
