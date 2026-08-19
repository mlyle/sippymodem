// SPDX-License-Identifier: BSD-3-Clause

#ifndef V32_LINE_RECEIVER_H
#define V32_LINE_RECEIVER_H

#include "agc.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V32_LINE_RECEIVER_EPOCH_SAMPLES = 36,
};

enum v32_line_receiver_result {
	V32_LINE_RECEIVER_OK,
	V32_LINE_RECEIVER_INVALID_ARGUMENT,
	V32_LINE_RECEIVER_INVALID_STATE,
};

/*
 * Persistent post-echo receive ownership.  This first slice retains exact
 * 36-sample gain epochs; symbol timing and the fractionally spaced equalizer
 * will consume its normalized stream without resetting this owner.
 */
struct v32_line_receiver {
	struct agc gain;
	int16_t epoch[V32_LINE_RECEIVER_EPOCH_SAMPLES];
	size_t epoch_fill;
	bool echo_adapting;
};

enum v32_line_receiver_result v32_line_receiver_init(
	struct v32_line_receiver *receiver);
bool v32_line_receiver_is_valid(
	const struct v32_line_receiver *receiver);
enum v32_line_receiver_result v32_line_receiver_freeze_gain(
	struct v32_line_receiver *receiver);
enum v32_line_receiver_result v32_line_receiver_reacquire_gain(
	struct v32_line_receiver *receiver);

/*
 * Consume finite 7.2 kHz post-echo samples.  Echo-adaptation samples are
 * deliberately discarded without clocking AGC.  Successful calls report
 * zero or 36 normalized samples; count may not exceed one 36-sample
 * epoch, and arbitrary smaller fragments do not change epoch boundaries.
 * Rejected calls change no owner or output.
 */
enum v32_line_receiver_result v32_line_receiver_process(
	struct v32_line_receiver *receiver,
	const double *cleaned, size_t count, bool echo_adapting,
	int16_t *normalized, size_t normalized_capacity,
	size_t *normalized_count);

const char *v32_line_receiver_result_name(
	enum v32_line_receiver_result result);

#endif
