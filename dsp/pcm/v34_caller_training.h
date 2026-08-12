// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_CALLER_TRAINING_H
#define V34_CALLER_TRAINING_H

#include "v34_phase3_caller.h"
#include "v34_phase4_j_prime_sequence.h"
#include "v34_phase4_trn.h"
#include "v34_training_line.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_caller_training_stage {
	V34_CALLER_TRAINING_PHASE3,
	V34_CALLER_TRAINING_J_PRIME,
	V34_CALLER_TRAINING_PHASE4_TRN,
};

enum v34_caller_training_result {
	V34_CALLER_TRAINING_OK,
	V34_CALLER_TRAINING_INVALID_ARGUMENT,
	V34_CALLER_TRAINING_INVALID_STATE,
	V34_CALLER_TRAINING_OUT_OF_RANGE,
	V34_CALLER_TRAINING_CLIPPING_RISK,
	V34_CALLER_TRAINING_NUMERIC_FAILURE,
};

union v34_caller_phase4_state {
	struct v34_phase4_j_prime_sequence j_prime;
	struct v34_phase4_trn trn;
};

/* Calling-station normalized training and its uninterrupted line waveform.
 * The call controller selects the remote training constellation and decides
 * when the open-ended Phase 3 TRN and J intervals may end. */
struct v34_caller_training {
	struct v34_training_line line;
	struct v34_phase3_caller phase3;
	union v34_caller_phase4_state phase4;
	uint32_t phase4_trn_symbols;
	enum v34_caller_training_stage stage;
};

enum v34_caller_training_result v34_caller_training_init(
	struct v34_caller_training *training,
	const struct v34_phase3_profile *profile,
	enum v34_constellation_size remote_constellation,
	uint32_t sample_rate, int16_t nominal_peak_amplitude);

bool v34_caller_training_is_valid(
	const struct v34_caller_training *training);

/* These controls apply at the next unconsumed symbol boundary. Calls before
 * the Phase 3 minimum/pattern boundary leave the complete instance unchanged. */
enum v34_caller_training_result v34_caller_training_start_j(
	struct v34_caller_training *training);
enum v34_caller_training_result v34_caller_training_finish_j(
	struct v34_caller_training *training);

/* Emit one line sample. J-prime advances automatically into a freshly
 * scrambled Phase 4 TRN stream without resetting any line-waveform state. */
enum v34_caller_training_result v34_caller_training_sample(
	struct v34_caller_training *training, int16_t *sample);

bool v34_caller_training_phase4_trn_ready(
	const struct v34_caller_training *training);

const char *v34_caller_training_result_name(
	enum v34_caller_training_result result);

#endif
