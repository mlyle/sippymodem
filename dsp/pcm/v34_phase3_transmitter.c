// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase3_transmitter.h"

#include <stddef.h>

static bool role_is_valid(enum v34_station_role role)
{
	return role == V34_CALLING_STATION ||
	       role == V34_ANSWERING_STATION;
}

static bool counters_are_valid(const struct v34_phase3_transmitter *transmitter)
{
	switch (transmitter->stage) {
	case V34_PHASE3_TRANSMITTER_S:
	case V34_PHASE3_TRANSMITTER_PP:
		return transmitter->trn_symbols == 0 && transmitter->j_symbols == 0;
	case V34_PHASE3_TRANSMITTER_TRN:
		return transmitter->j_symbols == 0 &&
		       transmitter->state.trn.has_last_symbol ==
			       (transmitter->trn_symbols != 0);
	case V34_PHASE3_TRANSMITTER_J:
		return transmitter->trn_symbols >= V34_PHASE3_TRN_MIN_SYMBOLS &&
		       transmitter->j_symbols % V34_PHASE3_J_PATTERN_SYMBOLS ==
			       transmitter->state.j.symbol_in_pattern;
	case V34_PHASE3_TRANSMITTER_COMPLETE:
		return transmitter->trn_symbols >= V34_PHASE3_TRN_MIN_SYMBOLS &&
		       transmitter->j_symbols >= V34_PHASE3_J_PATTERN_SYMBOLS &&
		       transmitter->j_symbols % V34_PHASE3_J_PATTERN_SYMBOLS == 0;
	}
	return false;
}

bool v34_phase3_transmitter_is_valid(
	const struct v34_phase3_transmitter *transmitter)
{
	if (transmitter == NULL ||
	    !role_is_valid(transmitter->local_role) ||
	    !v34_constellation_size_is_valid(
		    transmitter->remote_constellation) ||
	    !counters_are_valid(transmitter))
		return false;
	switch (transmitter->stage) {
	case V34_PHASE3_TRANSMITTER_S:
		return v34_phase3_s_sequence_is_valid(&transmitter->state.s) &&
		       !v34_phase3_s_sequence_is_complete(&transmitter->state.s);
	case V34_PHASE3_TRANSMITTER_PP:
		return v34_phase3_pp_sequence_is_valid(&transmitter->state.pp) &&
		       !v34_phase3_pp_sequence_is_complete(&transmitter->state.pp);
	case V34_PHASE3_TRANSMITTER_TRN:
		return v34_phase3_trn_is_valid(&transmitter->state.trn) &&
		       transmitter->state.trn.role == transmitter->local_role;
	case V34_PHASE3_TRANSMITTER_J:
		return v34_phase3_j_sequence_is_valid(&transmitter->state.j) &&
		       transmitter->state.j.role == transmitter->local_role &&
		       transmitter->state.j.remote_constellation ==
			       transmitter->remote_constellation;
	case V34_PHASE3_TRANSMITTER_COMPLETE:
		return v34_phase3_j_sequence_at_pattern_boundary(
			       &transmitter->state.j) &&
		       transmitter->state.j.role == transmitter->local_role &&
		       transmitter->state.j.remote_constellation ==
			       transmitter->remote_constellation;
	}
	return false;
}

enum v34_phase3_transmitter_result v34_phase3_transmitter_init(
	struct v34_phase3_transmitter *transmitter,
	enum v34_station_role local_role,
	enum v34_constellation_size remote_constellation)
{
	struct v34_phase3_transmitter initialized = {
		.remote_constellation = remote_constellation,
		.local_role = local_role,
		.stage = V34_PHASE3_TRANSMITTER_S,
	};

	if (transmitter == NULL || !role_is_valid(local_role) ||
	    !v34_constellation_size_is_valid(remote_constellation) ||
	    v34_phase3_s_sequence_init(&initialized.state.s) !=
		    V34_PHASE3_S_SEQUENCE_OK ||
	    !v34_phase3_transmitter_is_valid(&initialized))
		return V34_PHASE3_TRANSMITTER_INVALID_ARGUMENT;
	*transmitter = initialized;
	return V34_PHASE3_TRANSMITTER_OK;
}

static enum v34_phase3_transmitter_result next_s(
	struct v34_phase3_transmitter *transmitter, struct qam_point *point)
{
	enum v34_phase3_s_section section;

	if (v34_phase3_s_sequence_next(
		    &transmitter->state.s, point, &section) !=
	    V34_PHASE3_S_SEQUENCE_OK)
		return V34_PHASE3_TRANSMITTER_NUMERIC_FAILURE;
	if (v34_phase3_s_sequence_is_complete(&transmitter->state.s)) {
		transmitter->state = (union v34_phase3_transmitter_state){ 0 };
		if (v34_phase3_pp_sequence_init(&transmitter->state.pp) !=
		    V34_PHASE3_PP_SEQUENCE_OK)
			return V34_PHASE3_TRANSMITTER_NUMERIC_FAILURE;
		transmitter->stage = V34_PHASE3_TRANSMITTER_PP;
	}
	return V34_PHASE3_TRANSMITTER_OK;
}

static enum v34_phase3_transmitter_result next_pp(
	struct v34_phase3_transmitter *transmitter, struct qam_point *point)
{
	if (v34_phase3_pp_sequence_next(&transmitter->state.pp, point) !=
	    V34_PHASE3_PP_SEQUENCE_OK)
		return V34_PHASE3_TRANSMITTER_NUMERIC_FAILURE;
	if (v34_phase3_pp_sequence_is_complete(&transmitter->state.pp)) {
		transmitter->state = (union v34_phase3_transmitter_state){ 0 };
		if (v34_phase3_trn_init(
			    &transmitter->state.trn,
			    transmitter->local_role) !=
		    V34_PHASE3_TRN_OK)
			return V34_PHASE3_TRANSMITTER_NUMERIC_FAILURE;
		transmitter->stage = V34_PHASE3_TRANSMITTER_TRN;
	}
	return V34_PHASE3_TRANSMITTER_OK;
}

static enum v34_phase3_transmitter_result next_trn(
	struct v34_phase3_transmitter *transmitter, struct qam_point *point)
{
	uint8_t rotation;

	if (transmitter->trn_symbols == UINT32_MAX ||
	    v34_phase3_trn_next(
		    &transmitter->state.trn, point, &rotation) !=
		    V34_PHASE3_TRN_OK)
		return V34_PHASE3_TRANSMITTER_NUMERIC_FAILURE;
	transmitter->trn_symbols++;
	return V34_PHASE3_TRANSMITTER_OK;
}

static enum v34_phase3_transmitter_result next_j(
	struct v34_phase3_transmitter *transmitter, struct qam_point *point)
{
	uint8_t rotation;

	if (transmitter->j_symbols == UINT32_MAX ||
	    v34_phase3_j_sequence_next(
		    &transmitter->state.j, point, &rotation) !=
		    V34_PHASE3_J_SEQUENCE_OK)
		return V34_PHASE3_TRANSMITTER_NUMERIC_FAILURE;
	transmitter->j_symbols++;
	return V34_PHASE3_TRANSMITTER_OK;
}

enum v34_phase3_transmitter_result v34_phase3_transmitter_next(
	struct v34_phase3_transmitter *transmitter, struct qam_point *point)
{
	struct v34_phase3_transmitter next;
	struct qam_point next_point;
	enum v34_phase3_transmitter_result result;

	if (!v34_phase3_transmitter_is_valid(transmitter) || point == NULL)
		return V34_PHASE3_TRANSMITTER_INVALID_ARGUMENT;
	if (transmitter->stage == V34_PHASE3_TRANSMITTER_COMPLETE)
		return V34_PHASE3_TRANSMITTER_DONE;
	next = *transmitter;
	switch (next.stage) {
	case V34_PHASE3_TRANSMITTER_S:
		result = next_s(&next, &next_point);
		break;
	case V34_PHASE3_TRANSMITTER_PP:
		result = next_pp(&next, &next_point);
		break;
	case V34_PHASE3_TRANSMITTER_TRN:
		result = next_trn(&next, &next_point);
		break;
	case V34_PHASE3_TRANSMITTER_J:
		result = next_j(&next, &next_point);
		break;
	case V34_PHASE3_TRANSMITTER_COMPLETE:
		return V34_PHASE3_TRANSMITTER_DONE;
	default:
		return V34_PHASE3_TRANSMITTER_INVALID_ARGUMENT;
	}
	if (result != V34_PHASE3_TRANSMITTER_OK)
		return result;
	if (!v34_phase3_transmitter_is_valid(&next))
		return V34_PHASE3_TRANSMITTER_NUMERIC_FAILURE;
	*transmitter = next;
	*point = next_point;
	return V34_PHASE3_TRANSMITTER_OK;
}

enum v34_phase3_transmitter_result v34_phase3_transmitter_start_j(
	struct v34_phase3_transmitter *transmitter)
{
	struct v34_phase3_transmitter next;
	struct v34_phase3_j_sequence j;

	if (!v34_phase3_transmitter_is_valid(transmitter))
		return V34_PHASE3_TRANSMITTER_INVALID_ARGUMENT;
	if (transmitter->stage != V34_PHASE3_TRANSMITTER_TRN ||
	    transmitter->trn_symbols < V34_PHASE3_TRN_MIN_SYMBOLS)
		return V34_PHASE3_TRANSMITTER_INVALID_STATE;
	if (v34_phase3_j_sequence_init_from_trn(
		    &j, &transmitter->state.trn,
		    transmitter->remote_constellation) !=
	    V34_PHASE3_J_SEQUENCE_OK)
		return V34_PHASE3_TRANSMITTER_NUMERIC_FAILURE;
	next = *transmitter;
	next.state = (union v34_phase3_transmitter_state){ 0 };
	next.state.j = j;
	next.stage = V34_PHASE3_TRANSMITTER_J;
	if (!v34_phase3_transmitter_is_valid(&next))
		return V34_PHASE3_TRANSMITTER_NUMERIC_FAILURE;
	*transmitter = next;
	return V34_PHASE3_TRANSMITTER_OK;
}

enum v34_phase3_transmitter_result v34_phase3_transmitter_trn_boundary(
	const struct v34_phase3_transmitter *transmitter,
	struct v34_phase3_trn *trn)
{
	if (!v34_phase3_transmitter_is_valid(transmitter) || trn == NULL)
		return V34_PHASE3_TRANSMITTER_INVALID_ARGUMENT;
	if (transmitter->stage != V34_PHASE3_TRANSMITTER_TRN ||
	    transmitter->trn_symbols < V34_PHASE3_TRN_MIN_SYMBOLS)
		return V34_PHASE3_TRANSMITTER_INVALID_STATE;
	*trn = transmitter->state.trn;
	return V34_PHASE3_TRANSMITTER_OK;
}

enum v34_phase3_transmitter_result v34_phase3_transmitter_finish_j(
	struct v34_phase3_transmitter *transmitter)
{
	struct v34_phase3_transmitter next;

	if (!v34_phase3_transmitter_is_valid(transmitter))
		return V34_PHASE3_TRANSMITTER_INVALID_ARGUMENT;
	if (transmitter->stage != V34_PHASE3_TRANSMITTER_J ||
	    transmitter->j_symbols < V34_PHASE3_J_PATTERN_SYMBOLS ||
	    !v34_phase3_j_sequence_at_pattern_boundary(&transmitter->state.j))
		return V34_PHASE3_TRANSMITTER_INVALID_STATE;
	next = *transmitter;
	next.stage = V34_PHASE3_TRANSMITTER_COMPLETE;
	if (!v34_phase3_transmitter_is_valid(&next))
		return V34_PHASE3_TRANSMITTER_NUMERIC_FAILURE;
	*transmitter = next;
	return V34_PHASE3_TRANSMITTER_OK;
}

enum v34_phase3_transmitter_result v34_phase3_transmitter_start_j_prime(
	const struct v34_phase3_transmitter *transmitter,
	struct v34_phase4_j_prime_sequence *j_prime)
{
	if (!v34_phase3_transmitter_is_valid(transmitter) || j_prime == NULL)
		return V34_PHASE3_TRANSMITTER_INVALID_ARGUMENT;
	if (transmitter->stage != V34_PHASE3_TRANSMITTER_COMPLETE ||
	    transmitter->local_role != V34_CALLING_STATION)
		return V34_PHASE3_TRANSMITTER_INVALID_STATE;
	if (v34_phase4_j_prime_sequence_init_from_j(
		    j_prime, &transmitter->state.j) !=
	    V34_PHASE4_J_PRIME_SEQUENCE_OK)
		return V34_PHASE3_TRANSMITTER_NUMERIC_FAILURE;
	return V34_PHASE3_TRANSMITTER_OK;
}
