// SPDX-License-Identifier: BSD-3-Clause

#ifndef V32_ECHO_TRAINING_H
#define V32_ECHO_TRAINING_H

#include "conditioning.h"
#include "receive_path.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V32_ECHO_TRAINING_PREFILL_SYMBOLS = 256,
	V32_ECHO_TRAINING_COARSE_SYMBOLS = 1024,
	V32_CALLING_ECHO_TRAINING_FINE_SYMBOLS = 6912,
	V32_ANSWERING_ECHO_TRAINING_FINE_SYMBOLS = 6764,
	V32_CALLING_ECHO_TRAINING_SYMBOLS =
		V32_ECHO_TRAINING_PREFILL_SYMBOLS +
		V32_ECHO_TRAINING_COARSE_SYMBOLS +
		V32_CALLING_ECHO_TRAINING_FINE_SYMBOLS,
	V32_ANSWERING_ECHO_TRAINING_SYMBOLS =
		V32_ECHO_TRAINING_PREFILL_SYMBOLS +
		V32_ECHO_TRAINING_COARSE_SYMBOLS +
		V32_ANSWERING_ECHO_TRAINING_FINE_SYMBOLS,
};

enum v32_echo_training_result {
	V32_ECHO_TRAINING_OK,
	V32_ECHO_TRAINING_COMPLETE,
	V32_ECHO_TRAINING_INVALID_ARGUMENT,
	V32_ECHO_TRAINING_INVALID_STATE,
};

/* Emit the complete local conditioning stream and own the matching receive-
 * path echo lifecycle. The caller carries a frozen learned receive gain into
 * this interval and reacquires it when local TRN ends; the answerer leaves
 * gain tracking in place. The role-specific duration remains within the
 * standard TRN interval while preserving the exact prefill/coarse/fine
 * boundaries. */
struct v32_echo_training {
	struct v32_conditioning_transmitter conditioning;
	bool complete;
};

enum v32_echo_training_result v32_echo_training_init(
	struct v32_echo_training *training,
	enum v32_transmitter_role role);
bool v32_echo_training_is_valid(
	const struct v32_echo_training *training);
bool v32_echo_training_is_complete(
	const struct v32_echo_training *training);
bool v32_echo_training_path_is_valid(
	const struct v32_echo_training *training,
	const struct v32_receive_path *path);

/* Call at each accepted local transmit epoch. OK publishes one S, S-bar, or
 * TRN quadrant. COMPLETE publishes no quadrant: it freezes echo adaptation
 * and, for the caller, starts gain reacquisition before the first following
 * rate symbol is generated. Rejected and complete calls leave the owners and
 * output unchanged. */
enum v32_echo_training_result v32_echo_training_next(
	struct v32_echo_training *training,
	struct v32_receive_path *path, uint8_t *quadrant);

/* Export the exact TRN framing state after the COMPLETE handoff. */
enum v32_echo_training_result v32_echo_training_framing_state(
	const struct v32_echo_training *training,
	struct v32_symbol_encoder *encoder);

#endif
