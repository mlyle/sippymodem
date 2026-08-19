// SPDX-License-Identifier: BSD-3-Clause

#ifndef V32_ECHO_CANCELLER_H
#define V32_ECHO_CANCELLER_H

#include "qam.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V32_ECHO_PHASE_COUNT = 3,
	V32_ECHO_WINDOW_SYMBOLS = 40,
	/* A full window of separation keeps the two predictors disjoint. */
	V32_ECHO_MIN_ROUND_TRIP_SYMBOLS = 40,
	V32_ECHO_MAX_ROUND_TRIP_SYMBOLS = 480,
	/* The transmit-reference alignment reserve includes the near window. */
	V32_ECHO_ALIGNMENT_RESERVE_SYMBOLS = 48,
	V32_ECHO_MAX_NEAR_DELAY_SYMBOLS =
		V32_ECHO_ALIGNMENT_RESERVE_SYMBOLS -
		V32_ECHO_WINDOW_SYMBOLS,
	V32_ECHO_REFERENCE_CAPACITY =
		V32_ECHO_MAX_ROUND_TRIP_SYMBOLS +
		V32_ECHO_ALIGNMENT_RESERVE_SYMBOLS,
	V32_ECHO_NEAR_WINDOW = 0,
	V32_ECHO_DELAYED_WINDOW = 1,
	V32_ECHO_WINDOW_COUNT,
};

enum v32_echo_result {
	V32_ECHO_OK,
	V32_ECHO_INVALID_ARGUMENT,
	V32_ECHO_INVALID_STATE,
};

/*
 * Own the local-transmit reference and the three matched-sample phase filters
 * used to predict two separated echo regions.  One finite reference point
 * must be pushed before each group of three matched receive samples.  The
 * first window starts near_delay_symbols behind that point; the second starts
 * round_trip_symbols farther back.  Missing history is treated as silence.
 *
 * All mutable state is instance-owned and bounded.  Freezing stops coefficient
 * updates without resetting either the learned path or the reference clock.
 */
struct v32_echo_canceller {
	struct qam_point reference[V32_ECHO_REFERENCE_CAPACITY];
	struct qam_point
		coefficients[V32_ECHO_PHASE_COUNT]
			    [V32_ECHO_WINDOW_COUNT]
			    [V32_ECHO_WINDOW_SYMBOLS];
	size_t reference_write;
	size_t reference_fill;
	uint16_t near_delay_symbols;
	uint16_t round_trip_symbols;
	double adaptation;
	uint8_t phase;
	bool reference_pending;
	bool adapting;
};

enum v32_echo_result v32_echo_canceller_init(
	struct v32_echo_canceller *canceller,
	uint16_t near_delay_symbols, uint16_t round_trip_symbols);
bool v32_echo_canceller_is_valid(
	const struct v32_echo_canceller *canceller);

/* Begin or resume adaptation between symbol groups without clearing state. */
enum v32_echo_result v32_echo_canceller_start_adaptation(
	struct v32_echo_canceller *canceller, double adaptation);
/* Keep cancelling and advancing the reference while preserving coefficients. */
enum v32_echo_result v32_echo_canceller_freeze(
	struct v32_echo_canceller *canceller);

/* Push exactly one actual mapped transmit point at its symbol epoch. */
enum v32_echo_result v32_echo_canceller_push_reference(
	struct v32_echo_canceller *canceller,
	const struct qam_point *reference);

/*
 * Cancel one to the remaining samples in the current three-phase group.
 * received and cleaned may be the same array.  Other overlap, or overlap with
 * the owner, is rejected.  Invalid calls leave state and output unchanged.
 */
enum v32_echo_result v32_echo_canceller_process(
	struct v32_echo_canceller *canceller,
	const double *received, double *cleaned, size_t count);

const char *v32_echo_result_name(enum v32_echo_result result);

#endif
