// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_CALLER_TRAINING_H
#define V34_CALLER_TRAINING_H

#include "v34_phase3_transmitter.h"
#include "v34_phase4_e_sequence.h"
#include "v34_phase4_j_prime_sequence.h"
#include "v34_phase4_mp.h"
#include "v34_phase4_trn.h"
#include "v34_training_line.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_caller_training_stage {
	V34_CALLER_TRAINING_PHASE3,
	V34_CALLER_TRAINING_J_PRIME,
	V34_CALLER_TRAINING_PHASE4_TRN,
	V34_CALLER_TRAINING_PHASE4_MP,
	V34_CALLER_TRAINING_PHASE4_E,
};

enum v34_caller_training_result {
	V34_CALLER_TRAINING_OK,
	V34_CALLER_TRAINING_INVALID_ARGUMENT,
	V34_CALLER_TRAINING_INVALID_STATE,
	V34_CALLER_TRAINING_INVALID_PARAMETERS,
	V34_CALLER_TRAINING_OUT_OF_RANGE,
	V34_CALLER_TRAINING_CLIPPING_RISK,
	V34_CALLER_TRAINING_NUMERIC_FAILURE,
};

union v34_caller_phase4_state {
	struct v34_phase4_j_prime_sequence j_prime;
	struct v34_phase4_trn trn;
	struct v34_phase4_mp mp;
	struct v34_phase4_e_sequence e;
};

/* Calling-station normalized training and its uninterrupted line waveform.
 * The call controller selects the remote training constellation, decides when
 * open-ended intervals may end, and owns MP/MP-prime negotiation policy. The
 * current sequence owner accepts only a negotiated zero-length MD interval. */
struct v34_caller_training {
	struct v34_training_line line;
	struct v34_phase3_transmitter phase3;
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

/* Start the first typed MP after the Phase 4 TRN minimum, or replace a
 * completed MP/MP-prime record. The parameters determine whether the record
 * acknowledges the peer; handshake policy remains with the call controller. */
enum v34_caller_training_result v34_caller_training_start_mp_type0(
	struct v34_caller_training *training,
	const struct v34_mp_parameters *parameters);
enum v34_caller_training_result v34_caller_training_start_mp_type1(
	struct v34_caller_training *training,
	const struct v34_mp_type1_parameters *parameters);

/* E may begin only at a completed MP record boundary. */
enum v34_caller_training_result v34_caller_training_start_e(
	struct v34_caller_training *training);

/* Emit one line sample. J-prime advances automatically into a freshly
 * scrambled Phase 4 TRN stream without resetting any line-waveform state.
 * Completed MP and E sections drain through the current pulse until the next
 * symbol boundary, then report INVALID_STATE without consuming that boundary. */
enum v34_caller_training_result v34_caller_training_sample(
	struct v34_caller_training *training, int16_t *sample);

bool v34_caller_training_phase4_trn_ready(
	const struct v34_caller_training *training);
bool v34_caller_training_phase4_mp_record_complete(
	const struct v34_caller_training *training);
bool v34_caller_training_phase4_e_complete(
	const struct v34_caller_training *training);

const char *v34_caller_training_result_name(
	enum v34_caller_training_result result);

#endif
