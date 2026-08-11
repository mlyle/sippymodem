// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_QAM_EQUALIZER_H
#define SIPPYMODEM_QAM_EQUALIZER_H

#include "sippymodem_qam.h"

#include <stdbool.h>

enum {
	SIPPYMODEM_QAM_EQUALIZER_TAP_COUNT = 7,
	SIPPYMODEM_QAM_EQUALIZER_DELAY = 3,
};

enum sippymodem_qam_equalizer_result {
	SIPPYMODEM_QAM_EQUALIZER_OK,
	SIPPYMODEM_QAM_EQUALIZER_INVALID_ARGUMENT,
	SIPPYMODEM_QAM_EQUALIZER_INVALID_STATE,
};

/* A symbol-spaced, complex normalized-LMS equalizer.  The fixed seven-tap
 * span is deliberately small and gives a three-symbol decision delay. */
struct sippymodem_qam_equalizer {
	struct sippymodem_qam_point coefficients[
		SIPPYMODEM_QAM_EQUALIZER_TAP_COUNT];
	struct sippymodem_qam_point history[
		SIPPYMODEM_QAM_EQUALIZER_TAP_COUNT];
	struct sippymodem_qam_point last_output;
	unsigned int fill;
	bool can_adapt;
};

enum sippymodem_qam_equalizer_result sippymodem_qam_equalizer_init(
	struct sippymodem_qam_equalizer *equalizer,
	const struct sippymodem_qam_point *received_reference,
	const struct sippymodem_qam_point *transmitted_reference);
bool sippymodem_qam_equalizer_is_valid(
	const struct sippymodem_qam_equalizer *equalizer);

/* Push one received symbol.  ready is false until the decision delay has
 * filled.  Invalid calls leave the equalizer and outputs unchanged. */
enum sippymodem_qam_equalizer_result sippymodem_qam_equalizer_push(
	struct sippymodem_qam_equalizer *equalizer,
	const struct sippymodem_qam_point *received,
	struct sippymodem_qam_point *output, bool *ready);

/* Adapt the most recently produced output toward a known or decided symbol.
 * Each output may be adapted at most once. */
enum sippymodem_qam_equalizer_result sippymodem_qam_equalizer_adapt(
	struct sippymodem_qam_equalizer *equalizer,
	const struct sippymodem_qam_point *target, double adaptation);

const char *sippymodem_qam_equalizer_result_name(
	enum sippymodem_qam_equalizer_result result);

#endif
