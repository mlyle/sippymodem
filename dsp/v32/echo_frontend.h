// SPDX-License-Identifier: BSD-3-Clause

#ifndef V32_ECHO_FRONTEND_H
#define V32_ECHO_FRONTEND_H

#include "echo_canceller.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V32_ECHO_FRONTEND_INPUT_RATE = 8000,
	V32_ECHO_FRONTEND_OUTPUT_RATE = 7200,
	V32_ECHO_FRONTEND_FILTER_PHASES = 9,
	V32_ECHO_FRONTEND_FILTER_TAPS = 40,
	V32_ECHO_FRONTEND_FILTER_STEP = 10,
	/* Phase six aligns each three-output group with a transmit epoch. */
	V32_ECHO_FRONTEND_INITIAL_PHASE = 6,
};

struct v32_echo_frontend_filter {
	int16_t history[V32_ECHO_FRONTEND_FILTER_TAPS];
	size_t history_write;
	uint8_t phase;
	uint8_t inputs_needed;
};

/* A mapped transmit point rotated into its passband symbol epoch. */
struct v32_echo_reference {
	struct qam_point point;
};

/*
 * Own the 8 kHz to 7.2 kHz receive filter and the three-phase echo owner.
 * Initialize this owner together with the local QAM modulator at the first
 * continuously clocked QAM sample.  Each call must provide the typed echo
 * reference constructed for that sample's accepted point, and none otherwise.
 */
struct v32_echo_frontend {
	struct v32_echo_canceller echo;
	struct v32_echo_frontend_filter filter;
};

enum v32_echo_result v32_echo_frontend_init(
	struct v32_echo_frontend *frontend,
	uint16_t near_delay_symbols, uint16_t round_trip_symbols);
/* Bounds/control checks for an admitted streaming owner. The full
 * is_valid predicate also audits stored echo samples and coefficients. */
bool v32_echo_frontend_header_is_valid(
	const struct v32_echo_frontend *frontend);
bool v32_echo_frontend_is_valid(
	const struct v32_echo_frontend *frontend);

/* Capture carrier_phase before generating the sample that accepts mapped. */
enum v32_echo_result v32_echo_reference_init(
	struct v32_echo_reference *reference,
	const struct qam_point *mapped, uint32_t carrier_phase);

enum v32_echo_result v32_echo_frontend_start_adaptation(
	struct v32_echo_frontend *frontend, double adaptation);
enum v32_echo_result v32_echo_frontend_freeze(
	struct v32_echo_frontend *frontend);

/*
 * Consume one 8 kHz line sample after the matching transmit sample has been
 * generated.  accepted_reference is non-NULL exactly when the transmitter
 * accepted a new point.  On success, cleaned_ready reports whether one
 * 7.2 kHz sample was produced.  Invalid calls change neither owner nor output.
 */
enum v32_echo_result v32_echo_frontend_sample(
	struct v32_echo_frontend *frontend, int16_t received,
	const struct v32_echo_reference *accepted_reference,
	double *cleaned, bool *cleaned_ready);

#endif
