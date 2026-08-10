// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_QAM_GAIN_H
#define SIPPYMODEM_QAM_GAIN_H

#include "sippymodem_qam.h"

#include <stdbool.h>

enum sippymodem_qam_gain_result {
	SIPPYMODEM_QAM_GAIN_OK,
	SIPPYMODEM_QAM_GAIN_INVALID_ARGUMENT,
};

/* A flat complex channel estimate. adaptation is the fraction of each new
 * observed gain incorporated by track(), from zero through one. */
struct sippymodem_qam_gain {
	struct sippymodem_qam_point channel_gain;
	double adaptation;
};

enum sippymodem_qam_gain_result sippymodem_qam_gain_init(
	struct sippymodem_qam_gain *tracker,
	const struct sippymodem_qam_point *channel_gain, double adaptation);
enum sippymodem_qam_gain_result sippymodem_qam_gain_init_from_reference(
	struct sippymodem_qam_gain *tracker,
	const struct sippymodem_qam_point *received,
	const struct sippymodem_qam_point *transmitted, double adaptation);
bool sippymodem_qam_gain_is_valid(
	const struct sippymodem_qam_gain *tracker);

/* Predict the received point for a transmitted point, or remove the tracked
 * gain from a received point. Rejected calls leave output unchanged. */
enum sippymodem_qam_gain_result sippymodem_qam_gain_predict(
	const struct sippymodem_qam_gain *tracker,
	const struct sippymodem_qam_point *transmitted,
	struct sippymodem_qam_point *received);
enum sippymodem_qam_gain_result sippymodem_qam_gain_equalize(
	const struct sippymodem_qam_gain *tracker,
	const struct sippymodem_qam_point *received,
	struct sippymodem_qam_point *equalized);

/* Update from a known nonzero transmitted point. Rejected calls leave the
 * estimate unchanged. */
enum sippymodem_qam_gain_result sippymodem_qam_gain_track(
	struct sippymodem_qam_gain *tracker,
	const struct sippymodem_qam_point *received,
	const struct sippymodem_qam_point *transmitted);

const char *sippymodem_qam_gain_result_name(
	enum sippymodem_qam_gain_result result);

#endif
