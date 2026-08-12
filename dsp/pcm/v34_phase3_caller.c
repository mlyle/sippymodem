// SPDX-License-Identifier: BSD-3-Clause

#include "v34_phase3_caller.h"

#include <stddef.h>

static bool counters_are_valid(const struct v34_phase3_caller *caller)
{
	switch (caller->stage) {
	case V34_PHASE3_CALLER_S:
	case V34_PHASE3_CALLER_PP:
		return caller->trn_symbols == 0 && caller->j_symbols == 0;
	case V34_PHASE3_CALLER_TRN:
		return caller->j_symbols == 0 &&
		       caller->state.trn.has_last_symbol ==
			       (caller->trn_symbols != 0);
	case V34_PHASE3_CALLER_J:
		return caller->trn_symbols >= V34_PHASE3_TRN_MIN_SYMBOLS &&
		       caller->j_symbols % V34_PHASE3_J_PATTERN_SYMBOLS ==
			       caller->state.j.symbol_in_pattern;
	case V34_PHASE3_CALLER_COMPLETE:
		return caller->trn_symbols >= V34_PHASE3_TRN_MIN_SYMBOLS &&
		       caller->j_symbols >= V34_PHASE3_J_PATTERN_SYMBOLS &&
		       caller->j_symbols % V34_PHASE3_J_PATTERN_SYMBOLS == 0;
	}
	return false;
}

bool v34_phase3_caller_is_valid(
	const struct v34_phase3_caller *caller)
{
	if (caller == NULL ||
	    !v34_constellation_size_is_valid(
		    caller->remote_constellation) ||
	    !counters_are_valid(caller))
		return false;
	switch (caller->stage) {
	case V34_PHASE3_CALLER_S:
		return v34_phase3_s_sequence_is_valid(&caller->state.s) &&
		       !v34_phase3_s_sequence_is_complete(&caller->state.s);
	case V34_PHASE3_CALLER_PP:
		return v34_phase3_pp_sequence_is_valid(&caller->state.pp) &&
		       !v34_phase3_pp_sequence_is_complete(&caller->state.pp);
	case V34_PHASE3_CALLER_TRN:
		return v34_phase3_trn_is_valid(&caller->state.trn) &&
		       caller->state.trn.role == V34_CALLING_STATION;
	case V34_PHASE3_CALLER_J:
		return v34_phase3_j_sequence_is_valid(&caller->state.j) &&
		       caller->state.j.role == V34_CALLING_STATION &&
		       caller->state.j.remote_constellation ==
			       caller->remote_constellation;
	case V34_PHASE3_CALLER_COMPLETE:
		return v34_phase3_j_sequence_at_pattern_boundary(
			       &caller->state.j) &&
		       caller->state.j.role == V34_CALLING_STATION &&
		       caller->state.j.remote_constellation ==
			       caller->remote_constellation;
	}
	return false;
}

enum v34_phase3_caller_result v34_phase3_caller_init(
	struct v34_phase3_caller *caller,
	enum v34_constellation_size remote_constellation)
{
	struct v34_phase3_caller initialized = {
		.remote_constellation = remote_constellation,
		.stage = V34_PHASE3_CALLER_S,
	};

	if (caller == NULL ||
	    !v34_constellation_size_is_valid(remote_constellation) ||
	    v34_phase3_s_sequence_init(&initialized.state.s) !=
		    V34_PHASE3_S_SEQUENCE_OK ||
	    !v34_phase3_caller_is_valid(&initialized))
		return V34_PHASE3_CALLER_INVALID_ARGUMENT;
	*caller = initialized;
	return V34_PHASE3_CALLER_OK;
}

static enum v34_phase3_caller_result next_s(
	struct v34_phase3_caller *caller, struct qam_point *point)
{
	enum v34_phase3_s_section section;

	if (v34_phase3_s_sequence_next(
		    &caller->state.s, point, &section) !=
	    V34_PHASE3_S_SEQUENCE_OK)
		return V34_PHASE3_CALLER_NUMERIC_FAILURE;
	if (v34_phase3_s_sequence_is_complete(&caller->state.s)) {
		caller->state = (union v34_phase3_caller_state){ 0 };
		if (v34_phase3_pp_sequence_init(&caller->state.pp) !=
		    V34_PHASE3_PP_SEQUENCE_OK)
			return V34_PHASE3_CALLER_NUMERIC_FAILURE;
		caller->stage = V34_PHASE3_CALLER_PP;
	}
	return V34_PHASE3_CALLER_OK;
}

static enum v34_phase3_caller_result next_pp(
	struct v34_phase3_caller *caller, struct qam_point *point)
{
	if (v34_phase3_pp_sequence_next(&caller->state.pp, point) !=
	    V34_PHASE3_PP_SEQUENCE_OK)
		return V34_PHASE3_CALLER_NUMERIC_FAILURE;
	if (v34_phase3_pp_sequence_is_complete(&caller->state.pp)) {
		caller->state = (union v34_phase3_caller_state){ 0 };
		if (v34_phase3_trn_init(
			    &caller->state.trn, V34_CALLING_STATION) !=
		    V34_PHASE3_TRN_OK)
			return V34_PHASE3_CALLER_NUMERIC_FAILURE;
		caller->stage = V34_PHASE3_CALLER_TRN;
	}
	return V34_PHASE3_CALLER_OK;
}

static enum v34_phase3_caller_result next_trn(
	struct v34_phase3_caller *caller, struct qam_point *point)
{
	uint8_t rotation;

	if (caller->trn_symbols == UINT32_MAX ||
	    v34_phase3_trn_next(
		    &caller->state.trn, point, &rotation) !=
		    V34_PHASE3_TRN_OK)
		return V34_PHASE3_CALLER_NUMERIC_FAILURE;
	caller->trn_symbols++;
	return V34_PHASE3_CALLER_OK;
}

static enum v34_phase3_caller_result next_j(
	struct v34_phase3_caller *caller, struct qam_point *point)
{
	uint8_t rotation;

	if (caller->j_symbols == UINT32_MAX ||
	    v34_phase3_j_sequence_next(
		    &caller->state.j, point, &rotation) !=
		    V34_PHASE3_J_SEQUENCE_OK)
		return V34_PHASE3_CALLER_NUMERIC_FAILURE;
	caller->j_symbols++;
	return V34_PHASE3_CALLER_OK;
}

enum v34_phase3_caller_result v34_phase3_caller_next(
	struct v34_phase3_caller *caller, struct qam_point *point)
{
	struct v34_phase3_caller next;
	struct qam_point next_point;
	enum v34_phase3_caller_result result;

	if (!v34_phase3_caller_is_valid(caller) || point == NULL)
		return V34_PHASE3_CALLER_INVALID_ARGUMENT;
	if (caller->stage == V34_PHASE3_CALLER_COMPLETE)
		return V34_PHASE3_CALLER_DONE;
	next = *caller;
	switch (next.stage) {
	case V34_PHASE3_CALLER_S:
		result = next_s(&next, &next_point);
		break;
	case V34_PHASE3_CALLER_PP:
		result = next_pp(&next, &next_point);
		break;
	case V34_PHASE3_CALLER_TRN:
		result = next_trn(&next, &next_point);
		break;
	case V34_PHASE3_CALLER_J:
		result = next_j(&next, &next_point);
		break;
	case V34_PHASE3_CALLER_COMPLETE:
		return V34_PHASE3_CALLER_DONE;
	default:
		return V34_PHASE3_CALLER_INVALID_ARGUMENT;
	}
	if (result != V34_PHASE3_CALLER_OK)
		return result;
	if (!v34_phase3_caller_is_valid(&next))
		return V34_PHASE3_CALLER_NUMERIC_FAILURE;
	*caller = next;
	*point = next_point;
	return V34_PHASE3_CALLER_OK;
}

enum v34_phase3_caller_result v34_phase3_caller_start_j(
	struct v34_phase3_caller *caller)
{
	struct v34_phase3_caller next;
	struct v34_phase3_j_sequence j;

	if (!v34_phase3_caller_is_valid(caller))
		return V34_PHASE3_CALLER_INVALID_ARGUMENT;
	if (caller->stage != V34_PHASE3_CALLER_TRN ||
	    caller->trn_symbols < V34_PHASE3_TRN_MIN_SYMBOLS)
		return V34_PHASE3_CALLER_INVALID_STATE;
	if (v34_phase3_j_sequence_init_from_trn(
		    &j, &caller->state.trn,
		    caller->remote_constellation) !=
	    V34_PHASE3_J_SEQUENCE_OK)
		return V34_PHASE3_CALLER_NUMERIC_FAILURE;
	next = *caller;
	next.state = (union v34_phase3_caller_state){ 0 };
	next.state.j = j;
	next.stage = V34_PHASE3_CALLER_J;
	if (!v34_phase3_caller_is_valid(&next))
		return V34_PHASE3_CALLER_NUMERIC_FAILURE;
	*caller = next;
	return V34_PHASE3_CALLER_OK;
}

enum v34_phase3_caller_result v34_phase3_caller_finish_j(
	struct v34_phase3_caller *caller)
{
	struct v34_phase3_caller next;

	if (!v34_phase3_caller_is_valid(caller))
		return V34_PHASE3_CALLER_INVALID_ARGUMENT;
	if (caller->stage != V34_PHASE3_CALLER_J ||
	    caller->j_symbols < V34_PHASE3_J_PATTERN_SYMBOLS ||
	    !v34_phase3_j_sequence_at_pattern_boundary(&caller->state.j))
		return V34_PHASE3_CALLER_INVALID_STATE;
	next = *caller;
	next.stage = V34_PHASE3_CALLER_COMPLETE;
	if (!v34_phase3_caller_is_valid(&next))
		return V34_PHASE3_CALLER_NUMERIC_FAILURE;
	*caller = next;
	return V34_PHASE3_CALLER_OK;
}

enum v34_phase3_caller_result v34_phase3_caller_start_j_prime(
	const struct v34_phase3_caller *caller,
	struct v34_phase4_j_prime_sequence *j_prime)
{
	if (!v34_phase3_caller_is_valid(caller) || j_prime == NULL)
		return V34_PHASE3_CALLER_INVALID_ARGUMENT;
	if (caller->stage != V34_PHASE3_CALLER_COMPLETE)
		return V34_PHASE3_CALLER_INVALID_STATE;
	if (v34_phase4_j_prime_sequence_init_from_j(
		    j_prime, &caller->state.j) !=
	    V34_PHASE4_J_PRIME_SEQUENCE_OK)
		return V34_PHASE3_CALLER_NUMERIC_FAILURE;
	return V34_PHASE3_CALLER_OK;
}

const char *v34_phase3_caller_result_name(
	enum v34_phase3_caller_result result)
{
	switch (result) {
	case V34_PHASE3_CALLER_OK:
		return "ok";
	case V34_PHASE3_CALLER_DONE:
		return "complete";
	case V34_PHASE3_CALLER_INVALID_ARGUMENT:
		return "invalid argument";
	case V34_PHASE3_CALLER_INVALID_STATE:
		return "invalid state";
	case V34_PHASE3_CALLER_NUMERIC_FAILURE:
		return "numeric failure";
	}
	return "unknown";
}
