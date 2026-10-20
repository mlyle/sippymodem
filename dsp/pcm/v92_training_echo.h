// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_TRAINING_ECHO_H
#define V92_TRAINING_ECHO_H

#include "pcm_host_echo.h"
#include "v92_pcm_phase4_trn2u_line_rx.h"

enum {
	V92_TRAINING_ECHO_LAGS = 13,
	V92_TRAINING_ECHO_COLUMNS = V92_TRAINING_ECHO_LAGS + 1,
	V92_TRAINING_ECHO_MOMENTS = V92_TRAINING_ECHO_COLUMNS *
		(V92_TRAINING_ECHO_COLUMNS + 1) / 2,
};

/* Zero initialization starts an owned, serialized monitoring epoch. Fit only
 * an early known peer projection; never fit echo coefficients to peer data.
 * The lower triangular Gram matrix stores each exact integer product once. */
struct v92_training_echo {
	double normal[V92_TRAINING_ECHO_MOMENTS];
	double cross[V92_TRAINING_ECHO_COLUMNS];
	double energy, coefficient[4];
	int32_t history[V92_TRAINING_ECHO_LAGS];
	uint32_t last_symbol;
	uint16_t count;
	uint8_t fill, lag[3];
	bool active, ready, view;
};

enum v92_training_echo_result {
	V92_TRAINING_ECHO_UNOBSERVED,
	V92_TRAINING_ECHO_OBSERVED,
	V92_TRAINING_ECHO_RETIMED,
	V92_TRAINING_ECHO_ERROR,
};

/* Call after echo prediction and before consuming the same physical sample.
 * receiver is the valid, owned initial digital TRN2u line; NULL ends its epoch.
 * Missing/clipped input, discontinuities and short renegotiation discard the
 * projection. The receiver's current reference includes its inherited FIR
 * lookahead; preview it without advancing the receiver or its reference.
 *
 * OBSERVED/RETIMED have observed echo timing: finish with advance_reference,
 * not advance_predicted. Other successful calls leave ordinary observation to
 * the caller. RETIMED reports a signed shift: invalidate the old channel fit
 * through call-owned bounded recovery before it can commit parameters. Neither
 * actual receive samples nor filter coefficients are changed here. Storage is
 * disjoint; invalid arguments preserve delay_change. No pointer is retained. */
enum v92_training_echo_result v92_training_echo_observe(
	struct v92_training_echo *timing, struct pcm_host_echo *echo,
	const struct v92_pcm_phase4_trn2u_line_rx *receiver,
	enum pcm_host_echo_mode mode, int16_t received, int16_t cleaned,
	bool available, int *delay_change);

#endif
