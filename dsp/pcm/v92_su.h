// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_SU_H
#define V92_SU_H

#include "fractional_delay.h"
#include "v92_j.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V92_SU_PATTERN_SYMBOLS = 6,
	V92_SU_JD_RESPONSE_SYMBOLS = 144,
	/* Twenty-five input symbols followed by a half-symbol advance place the
	 * observable transition exactly 24.5 symbol intervals later. */
	V92_SU_JD_BAR_INPUT_SYMBOLS = 25,
	V92_SU_JP_BAR_INPUT_SYMBOLS = 24,
};

enum v92_su_stage {
	V92_SU_JD_RESPONSE,
	V92_SU_JD_BAR_RESPONSE,
	V92_SU_JP_WAIT,
	V92_SU_JP_BAR_RESPONSE,
	V92_SU_JP_PRIME_WAIT,
	V92_SU_DONE,
};

enum v92_su_result {
	V92_SU_OK,
	V92_SU_REACHED_JP_WAIT,
	V92_SU_REACHED_JP_PRIME_WAIT,
	V92_SU_COMPLETE,
	V92_SU_INVALID_ARGUMENT,
	V92_SU_INVALID_STATE,
	V92_SU_FILTER_ERROR,
};

/*
 * Own the analogue modem's Su response beginning when Jd is detected.  The
 * caller owns one fractional_delay across the complete upstream transmitter:
 * feed the preceding Phase 3 samples through it at phase zero, pass the same
 * instance here, and retain it for second TRN1u and Phase 4.  This preserves
 * both the common group delay and the selected post-Jp phase.
 */
struct v92_su {
	enum v92_su_stage stage;
	uint64_t stage_symbols;
	int32_t phase_q16;
	int32_t post_jp_phase_q16;
	int16_t level;
	uint8_t pattern_phase;
};

enum v92_su_result v92_su_init(struct v92_su *su, int16_t lu);
bool v92_su_is_valid(const struct v92_su *su);
bool v92_su_is_complete(const struct v92_su *su);

/* Emit one 8000-symbol/s signed-linear sample. Rejected calls leave the Su
 * state, delay state, and output unchanged. */
enum v92_su_result v92_su_next(
	struct v92_su *su, struct fractional_delay *delay,
	int16_t *output);

/* Begin the 24T inverted response after a complete, stable Jp record. */
enum v92_su_result v92_su_accept_jp(
	struct v92_su *su, const struct v92_jp *jp);

/* Complete Su after Jp-prime. An exact 8 kHz codec-grid transmitter restores
 * phase zero; an analogue backend preserves the selected fractional phase. */
enum v92_su_result v92_su_accept_jp_prime(
	struct v92_su *su, struct fractional_delay *delay,
	bool restore_codec_grid);

bool v92_su_get_phase(const struct v92_su *su, int32_t *phase_q16);
const char *v92_su_stage_name(enum v92_su_stage stage);
const char *v92_su_result_name(enum v92_su_result result);

#endif /* V92_SU_H */
