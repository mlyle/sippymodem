// SPDX-License-Identifier: BSD-3-Clause

#ifndef V32_LINE_RECEIVER_H
#define V32_LINE_RECEIVER_H

#include "agc.h"
#include "symbol_timing.h"

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
	struct v32_symbol_timing timing;
	int16_t epoch[V32_LINE_RECEIVER_EPOCH_SAMPLES];
	size_t epoch_fill;
	bool input_paused;
};

enum v32_line_receiver_result v32_line_receiver_init(
	struct v32_line_receiver *receiver);
bool v32_line_receiver_is_valid(
	const struct v32_line_receiver *receiver);
enum v32_line_receiver_result v32_line_receiver_freeze_gain(
	struct v32_line_receiver *receiver);
enum v32_line_receiver_result v32_line_receiver_reacquire_gain(
	struct v32_line_receiver *receiver);
/* A paused, empty owner may reset timing before its first resumed sample. */
enum v32_line_receiver_result v32_line_receiver_reacquire_timing(
	struct v32_line_receiver *receiver);
enum v32_line_receiver_result v32_line_receiver_set_timing_tracking(
	struct v32_line_receiver *receiver, bool enabled);

/*
 * Consume finite 7.2 kHz post-echo samples.  Paused samples are deliberately
 * discarded without clocking AGC or timing recovery.  Each
 * completed 36-sample gain epoch may report up to 42 timing-recovered
 * samples; arbitrary smaller fragments do not change epoch boundaries.
 * A call that completes an epoch must provide capacity for all 42 samples.
 * Output is withheld until the timing loop detects a signal.  Rejected calls
 * change no owner, output, or output count.
 */
enum v32_line_receiver_result v32_line_receiver_process(
	struct v32_line_receiver *receiver,
	const double *cleaned, size_t count, bool input_paused,
	int16_t *recovered, size_t recovered_capacity,
	size_t *recovered_count);

const char *v32_line_receiver_result_name(
	enum v32_line_receiver_result result);

#endif
