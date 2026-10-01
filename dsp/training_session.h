// SPDX-License-Identifier: BSD-3-Clause

#ifndef TRAINING_SESSION_H
#define TRAINING_SESSION_H

#include <stdbool.h>
#include <stdint.h>

enum training_origin {
	TRAINING_INITIAL,
	TRAINING_LOCAL_RETRAIN,
	TRAINING_REMOTE_RETRAIN,
};

enum training_session_state {
	TRAINING_SESSION_ACTIVE,
	TRAINING_SESSION_ONLINE,
	TRAINING_SESSION_FAILED,
};

enum training_session_result {
	TRAINING_SESSION_OK,
	TRAINING_SESSION_DEADLINE,
	TRAINING_SESSION_INVALID_ARGUMENT,
	TRAINING_SESSION_INVALID_STATE,
};

/*
 * Protocol state remains with the datapump.  This object owns only the common
 * lifetime of one initial training or retraining transaction.  stalled_ticks
 * is reset when the protocol records meaningful progress; it is diagnostic
 * and does not impose a protocol-independent stage timeout.
 */
struct training_session {
	uint64_t deadline_ticks;
	uint64_t elapsed_ticks;
	uint64_t stalled_ticks;
	uint32_t generation;
	uint32_t progress_events;
	enum training_origin origin;
	enum training_session_state state;
};

bool training_session_is_valid(const struct training_session *session);
bool training_session_is_online(const struct training_session *session);
bool training_session_has_failed(const struct training_session *session);
bool training_session_is_retraining(const struct training_session *session);

enum training_session_result training_session_init(
	struct training_session *session, uint64_t deadline_ticks);

enum training_session_result training_session_advance(
	struct training_session *session, uint64_t ticks);

enum training_session_result training_session_note_progress(
	struct training_session *session);

/* Transfer the active generation to a new datapump owner. Initial training
 * receives a fresh interval; retraining retains its elapsed-time deadline.
 * Per-owner progress diagnostics restart in either case. */
enum training_session_result training_session_transfer(
	struct training_session *session);

enum training_session_result training_session_connect(
	struct training_session *session);

enum training_session_result training_session_start_retrain(
	struct training_session *session, enum training_origin origin);

enum training_session_result training_session_fail(
	struct training_session *session);

#endif
