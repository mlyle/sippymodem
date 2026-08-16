// SPDX-License-Identifier: BSD-3-Clause

#include "training_session.h"

#include <limits.h>
#include <stddef.h>

static bool origin_is_valid(enum training_origin origin)
{
	return origin == TRAINING_INITIAL ||
	       origin == TRAINING_LOCAL_RETRAIN ||
	       origin == TRAINING_REMOTE_RETRAIN;
}

static bool state_is_valid(enum training_session_state state)
{
	return state == TRAINING_SESSION_ACTIVE ||
	       state == TRAINING_SESSION_ONLINE ||
	       state == TRAINING_SESSION_FAILED;
}

bool training_session_is_valid(const struct training_session *session)
{
	if (session == NULL || session->deadline_ticks == 0 ||
	    session->elapsed_ticks > session->deadline_ticks ||
	    session->stalled_ticks > session->elapsed_ticks ||
	    session->generation == 0 || !origin_is_valid(session->origin) ||
	    !state_is_valid(session->state))
		return false;
	return session->generation == 1 ?
		       session->origin == TRAINING_INITIAL :
		       session->origin != TRAINING_INITIAL;
}

bool training_session_is_online(const struct training_session *session)
{
	return training_session_is_valid(session) &&
	       session->state == TRAINING_SESSION_ONLINE;
}

bool training_session_has_failed(const struct training_session *session)
{
	return training_session_is_valid(session) &&
	       session->state == TRAINING_SESSION_FAILED;
}

bool training_session_is_retraining(const struct training_session *session)
{
	return training_session_is_valid(session) &&
	       session->state == TRAINING_SESSION_ACTIVE &&
	       session->generation > 1;
}

enum training_session_result training_session_init(
	struct training_session *session, uint64_t deadline_ticks)
{
	struct training_session initialized = {
		.deadline_ticks = deadline_ticks,
		.generation = 1,
		.origin = TRAINING_INITIAL,
		.state = TRAINING_SESSION_ACTIVE,
	};

	if (session == NULL || deadline_ticks == 0)
		return TRAINING_SESSION_INVALID_ARGUMENT;
	*session = initialized;
	return TRAINING_SESSION_OK;
}

enum training_session_result training_session_advance(
	struct training_session *session, uint64_t ticks)
{
	uint64_t remaining;

	if (!training_session_is_valid(session))
		return TRAINING_SESSION_INVALID_ARGUMENT;
	if (session->state != TRAINING_SESSION_ACTIVE)
		return TRAINING_SESSION_INVALID_STATE;
	remaining = session->deadline_ticks - session->elapsed_ticks;
	if (ticks > remaining) {
		session->elapsed_ticks = session->deadline_ticks;
		session->stalled_ticks += remaining;
		session->state = TRAINING_SESSION_FAILED;
		return TRAINING_SESSION_DEADLINE;
	}
	session->elapsed_ticks += ticks;
	session->stalled_ticks += ticks;
	return TRAINING_SESSION_OK;
}

enum training_session_result training_session_note_progress(
	struct training_session *session)
{
	if (!training_session_is_valid(session))
		return TRAINING_SESSION_INVALID_ARGUMENT;
	if (session->state != TRAINING_SESSION_ACTIVE)
		return TRAINING_SESSION_INVALID_STATE;
	session->stalled_ticks = 0;
	if (session->progress_events != UINT32_MAX)
		session->progress_events++;
	return TRAINING_SESSION_OK;
}

enum training_session_result training_session_connect(
	struct training_session *session)
{
	if (!training_session_is_valid(session))
		return TRAINING_SESSION_INVALID_ARGUMENT;
	if (session->state != TRAINING_SESSION_ACTIVE)
		return TRAINING_SESSION_INVALID_STATE;
	session->state = TRAINING_SESSION_ONLINE;
	return TRAINING_SESSION_OK;
}

enum training_session_result training_session_start_retrain(
	struct training_session *session, enum training_origin origin)
{
	if (!training_session_is_valid(session) || !origin_is_valid(origin))
		return TRAINING_SESSION_INVALID_ARGUMENT;
	if (session->state != TRAINING_SESSION_ONLINE ||
	    origin == TRAINING_INITIAL || session->generation == UINT32_MAX)
		return TRAINING_SESSION_INVALID_STATE;
	session->elapsed_ticks = 0;
	session->stalled_ticks = 0;
	session->progress_events = 0;
	session->generation++;
	session->origin = origin;
	session->state = TRAINING_SESSION_ACTIVE;
	return TRAINING_SESSION_OK;
}

enum training_session_result training_session_fail(
	struct training_session *session)
{
	if (!training_session_is_valid(session))
		return TRAINING_SESSION_INVALID_ARGUMENT;
	if (session->state == TRAINING_SESSION_FAILED)
		return TRAINING_SESSION_OK;
	session->state = TRAINING_SESSION_FAILED;
	return TRAINING_SESSION_OK;
}
