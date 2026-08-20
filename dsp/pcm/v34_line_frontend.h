// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_LINE_FRONTEND_H
#define V34_LINE_FRONTEND_H

#include "qam.h"
#include "v34_phase3_profile.h"
#include "v34_receive_path.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_line_frontend_mode {
	V34_LINE_FRONTEND_QAM,
	V34_LINE_FRONTEND_FIXED,
};

enum v34_line_frontend_result {
	V34_LINE_FRONTEND_OK,
	V34_LINE_FRONTEND_SYMBOL_READY,
	V34_LINE_FRONTEND_INVALID_ARGUMENT,
	V34_LINE_FRONTEND_INVALID_STATE,
};

/* Own one continuous line-sample timeline. The QAM member is retained only
 * for protocol paths that have not yet moved to the fixed V.34 receiver. */
struct v34_line_frontend {
	union {
		struct qam_demodulator qam;
		struct v34_receive_path fixed;
	} state;
	enum v34_line_frontend_mode mode;
	bool fixed_signal_started;
};

enum v34_line_frontend_result v34_line_frontend_init_qam(
	struct v34_line_frontend *frontend,
	const struct qam_demodulator *demodulator);
enum v34_line_frontend_result v34_line_frontend_init_fixed(
	struct v34_line_frontend *frontend,
	const struct v34_phase3_profile *transmitting_profile);
bool v34_line_frontend_is_valid(
	const struct v34_line_frontend *frontend);
bool v34_line_frontend_timing_acquired(
	const struct v34_line_frontend *frontend);
bool v34_line_frontend_matches_profile(
	const struct v34_line_frontend *frontend,
	const struct v34_phase3_profile *transmitting_profile);
uint32_t v34_line_frontend_sample_rate(
	const struct v34_line_frontend *frontend);

/* Freeze the fixed AGC at a qualified S/S-bar boundary. This is a no-op for
 * the compatibility QAM owner, whose gain remains in its point receiver. */
enum v34_line_frontend_result v34_line_frontend_freeze_gain(
	struct v34_line_frontend *frontend);

/* Consume one line sample and publish a unit-training-power point. The fixed
 * receiver uses the exact half-scale training amplitude of 3238. */
enum v34_line_frontend_result v34_line_frontend_sample(
	struct v34_line_frontend *frontend, int16_t sample,
	struct qam_point *point, bool *ready);

#endif /* V34_LINE_FRONTEND_H */
