// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_PHASE3_TRANSMITTER_H
#define V34_PHASE3_TRANSMITTER_H

#include "v34_phase3_j_sequence.h"
#include "v34_phase3_pp_sequence.h"
#include "v34_phase3_s_sequence.h"
#include "v34_training_sequence.h"
#include "v34_phase4_j_prime_sequence.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_phase3_transmitter_stage {
	V34_PHASE3_TRANSMITTER_S,
	V34_PHASE3_TRANSMITTER_PP,
	V34_PHASE3_TRANSMITTER_TRN,
	V34_PHASE3_TRANSMITTER_J,
	V34_PHASE3_TRANSMITTER_COMPLETE,
};

enum v34_phase3_transmitter_result {
	V34_PHASE3_TRANSMITTER_OK,
	V34_PHASE3_TRANSMITTER_DONE,
	V34_PHASE3_TRANSMITTER_INVALID_ARGUMENT,
	V34_PHASE3_TRANSMITTER_INVALID_STATE,
	V34_PHASE3_TRANSMITTER_NUMERIC_FAILURE,
};

union v34_phase3_transmitter_state {
	struct v34_phase3_s_sequence s;
	struct v34_phase3_pp_sequence pp;
	struct v34_training_sequence trn;
	struct v34_phase3_j_sequence j;
};

/* Own one station's zero-MD Phase 3 symbol sequence. The call controller
 * decides when the open-ended TRN and J intervals may end; this object keeps
 * their role-specific scrambler and differential phase continuous. */
struct v34_phase3_transmitter {
	union v34_phase3_transmitter_state state;
	enum v34_constellation_size remote_constellation;
	enum v34_station_role local_role;
	enum v34_phase3_transmitter_stage stage;
	uint32_t trn_symbols;
	uint32_t j_symbols;
};

enum v34_phase3_transmitter_result v34_phase3_transmitter_init(
	struct v34_phase3_transmitter *transmitter,
	enum v34_station_role local_role,
	enum v34_constellation_size remote_constellation);

bool v34_phase3_transmitter_is_valid(
	const struct v34_phase3_transmitter *transmitter);

/* Emit the next normalized 2D symbol. S/S-bar and PP advance automatically;
 * TRN and J continue until their explicit transition functions are called.
 * Rejected calls leave both state and output unchanged. */
enum v34_phase3_transmitter_result v34_phase3_transmitter_next(
	struct v34_phase3_transmitter *transmitter, struct qam_point *point);

/* Enter J after at least 512 TRN symbols. */
enum v34_phase3_transmitter_result v34_phase3_transmitter_start_j(
	struct v34_phase3_transmitter *transmitter);

/* Copy the live boundary after at least 512 TRN symbols. This lets another
 * standards-defined continuation reuse the common S/PP/TRN prefix without
 * entering V.34 J. */
enum v34_phase3_transmitter_result v34_phase3_transmitter_trn_boundary(
	const struct v34_phase3_transmitter *transmitter,
	struct v34_training_sequence *trn);

/* Finish J only after at least one whole Table 18 pattern and at a pattern
 * boundary. The transmitter may keep emitting J while it waits for the remote
 * S/S-bar event and the next boundary. */
enum v34_phase3_transmitter_result v34_phase3_transmitter_finish_j(
	struct v34_phase3_transmitter *transmitter);

/* Initialize the calling station's one-pattern J-prime continuation without
 * changing the completed Phase 3 owner or the destination on failure. An
 * answering transmitter reports INVALID_STATE. */
enum v34_phase3_transmitter_result v34_phase3_transmitter_start_j_prime(
	const struct v34_phase3_transmitter *transmitter,
	struct v34_phase4_j_prime_sequence *j_prime);

#endif
