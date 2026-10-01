// SPDX-License-Identifier: BSD-3-Clause

#ifndef V32_LINE_STARTUP_H
#define V32_LINE_STARTUP_H

#include "qam.h"
#include "single_tone.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V32_ANSWER_TONE_HZ = 2100,
	V32_CALLING_CARRIER_HZ = 1800,
	V32_LOW_TURNAROUND_TONE_HZ = 600,
	V32_HIGH_TURNAROUND_TONE_HZ = 3000,
	V32_STARTUP_SYMBOL_RATE = 2400,
	V32_ANSWER_GUARD_SAMPLES =
		SINGLE_TONE_SAMPLE_RATE,
	/* The caller changes carrier state 64 +/- 2 symbols after acquiring the
	 * answerer's turnaround marker. */
	V32_REVERSAL_RESPONSE_SYMBOLS = 64,
	V32_REVERSAL_RESPONSE_SAMPLES =
		(SINGLE_TONE_SAMPLE_RATE * V32_REVERSAL_RESPONSE_SYMBOLS +
		 V32_STARTUP_SYMBOL_RATE / 2) /
		V32_STARTUP_SYMBOL_RATE,
	V32_REVERSAL_RESPONSE_TOLERANCE_SAMPLES =
		(SINGLE_TONE_SAMPLE_RATE * 2 +
		 V32_STARTUP_SYMBOL_RATE - 1) /
		V32_STARTUP_SYMBOL_RATE,
	/* The V.25 answer sequence used by the interoperable answer path. */
	V32_ANSWERER_TONE_SAMPLES =
		SINGLE_TONE_SAMPLE_RATE * 33 / 10,
	V32_ANSWERER_SILENCE_SAMPLES =
		SINGLE_TONE_SAMPLE_RATE * 3 / 40,
	/* Use an even AC span above the 128-symbol minimum so the calling
	 * carrier detector is settled before the first reversal. */
	V32_ANSWERER_AC_SYMBOLS = 160,
	V32_ANSWERER_AC_SAMPLES =
		(SINGLE_TONE_SAMPLE_RATE * V32_ANSWERER_AC_SYMBOLS +
		 V32_STARTUP_SYMBOL_RATE - 1) /
		V32_STARTUP_SYMBOL_RATE,
	V32_ANSWERER_DROP_SILENCE_SAMPLES =
		(SINGLE_TONE_SAMPLE_RATE * 16 +
		 V32_STARTUP_SYMBOL_RATE - 1) /
		V32_STARTUP_SYMBOL_RATE,
	V32_ANSWERER_CARRIER_WAIT_SAMPLES =
		SINGLE_TONE_SAMPLE_RATE * 3,
};

enum v32_line_startup_state {
	V32_LINE_WAIT_ANSWER_TONE,
	V32_LINE_ANSWER_GUARD,
	V32_LINE_SEND_CALLING_CARRIER,
	V32_LINE_DELAY_CALLING_CARRIER_REVERSAL,
	V32_LINE_SEND_REVERSED_CALLING_CARRIER,
	V32_LINE_STARTUP_COMPLETE,
	V32_LINE_STARTUP_FAILED,
};

enum v32_line_startup_result {
	V32_LINE_STARTUP_OK,
	V32_LINE_STARTUP_INVALID_ARGUMENT,
	V32_LINE_STARTUP_PROTOCOL_ERROR,
};

/* The measured turnaround owns two independent clocks: the caller's full,
 * even S extension and the canceller's bounded reference geometry. */
struct v32_line_timing {
	uint16_t echo_round_trip_symbols;
	uint16_t caller_s_extension_symbols;
};

/* Convert NT (caller) or MT (answerer) into protocol S duration and the
 * physical far-echo window. Delays outside the bounded predictor range are
 * rejected; rejection leaves timing unchanged. */
bool v32_line_timing_from_samples(
	uint32_t samples, bool caller, struct v32_line_timing *timing);

/* Carrier states A and C use the same pulse-shaped 1800 Hz transmit lane as
 * the later V.32 training and data signals.  The answerer alternates A/C;
 * the caller holds one state until the scheduled reversal. */
struct v32_line_carrier {
	struct qam_modulator modulator;
	struct qam_point point;
	uint8_t current_quadrant;
	uint8_t next_quadrant;
	bool alternating;
	bool current_active;
	bool reversed;
};

struct v32_line_startup {
	struct single_tone_detector answer_tone;
	struct single_tone_detector low_turnaround_tone;
	struct single_tone_detector high_turnaround_tone;
	struct v32_line_carrier calling_carrier;
	uint32_t state_samples;
	uint32_t round_trip_samples;
	uint16_t selected_turnaround_tone_hz;
	enum v32_line_startup_state state;
};

enum v32_answerer_line_state {
	V32_ANSWERER_LINE_ANSWER_TONE,
	V32_ANSWERER_LINE_SILENCE,
	V32_ANSWERER_LINE_SEND_AC,
	V32_ANSWERER_LINE_WAIT_CALLER_REVERSAL,
	V32_ANSWERER_LINE_DELAY_RESPONSE,
	V32_ANSWERER_LINE_HOLD_RESPONSE,
	V32_ANSWERER_LINE_POST_DROP_SILENCE,
	V32_ANSWERER_LINE_COMPLETE,
	V32_ANSWERER_LINE_FAILED,
};

struct v32_answerer_line_startup {
	struct single_tone_transmitter answer_tone;
	struct v32_line_carrier turnaround_carrier;
	struct single_tone_detector calling_carrier;
	uint32_t state_samples;
	uint32_t round_trip_samples;
	enum v32_answerer_line_state state;
	bool caller_reversed_early;
};

enum v32_line_startup_result v32_line_startup_init(
	struct v32_line_startup *startup,
	int16_t transmit_amplitude);
enum v32_line_startup_result v32_line_retrain_init(
	struct v32_line_startup *startup,
	int16_t transmit_amplitude);
bool v32_line_startup_is_valid(
	const struct v32_line_startup *startup);
bool v32_line_startup_is_complete(
	const struct v32_line_startup *startup);
uint32_t v32_line_startup_round_trip_samples(
	const struct v32_line_startup *startup);

enum v32_line_startup_result
v32_line_startup_process(
	struct v32_line_startup *startup, int16_t receive_sample,
	int16_t *transmit_sample, bool *complete);

enum v32_line_startup_result v32_answerer_line_startup_init(
	struct v32_answerer_line_startup *startup,
	int16_t transmit_amplitude);
enum v32_line_startup_result v32_answerer_line_retrain_init(
	struct v32_answerer_line_startup *startup,
	int16_t transmit_amplitude);
/* Continue after an automatic answer tone, without repeating that tone. */
enum v32_line_startup_result v32_answerer_line_resume(
	struct v32_answerer_line_startup *startup, int16_t transmit_amplitude,
	const struct single_tone_detector *calling_carrier);
bool v32_answerer_line_startup_is_valid(
	const struct v32_answerer_line_startup *startup);
bool v32_answerer_line_startup_is_complete(
	const struct v32_answerer_line_startup *startup);
uint32_t v32_answerer_line_startup_round_trip_samples(
	const struct v32_answerer_line_startup *startup);
enum v32_line_startup_result v32_answerer_line_startup_process(
	struct v32_answerer_line_startup *startup,
	int16_t receive_sample, int16_t *transmit_sample,
	bool *complete);

const char *v32_line_startup_state_name(
	enum v32_line_startup_state state);

#endif
