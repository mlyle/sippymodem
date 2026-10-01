// SPDX-License-Identifier: BSD-3-Clause

#ifndef LAPM_V44_SESSION_H
#define LAPM_V44_SESSION_H

#include "v42_xid.h"
#include "v44_xid.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	/* The maximum XID information field accepted from the LAPM transport. */
	LAPM_V44_MAX_XID_SIZE = 128,
};

enum lapm_v44_result {
	LAPM_V44_OK = 0,
	LAPM_V44_XID_TOO_LARGE,
	LAPM_V44_MALFORMED,
	LAPM_V44_INVALID_STATE,
	LAPM_V44_STALE_TRANSACTION,
	LAPM_V44_INVALID_ARGUMENT,
};

enum lapm_v44_phase {
	LAPM_V44_READY = 0,
	LAPM_V44_COMMAND_PENDING,
	LAPM_V44_NEGOTIATED,
	LAPM_V44_DECLINED,
};

struct lapm_v44_config {
	/* enabled is the opt-in policy; native_v44_ready is a safety interlock. */
	bool enabled;
	bool native_v44_ready;
	struct v44_offer local_offer;
};

struct lapm_v44_session;

/*
 * A prepared value owns the complete XID information field. The caller first
 * copies data[0..length) into a queued control frame and then calls commit.
 * Dropping this value after queue backpressure leaves the session unchanged.
 * Do not modify or reuse a prepared value, or move its session, before commit.
 */
struct lapm_v44_prepared {
	const struct lapm_v44_session *owner;
	uint64_t revision;
	unsigned int action;
	size_t length;
	uint8_t data[LAPM_V44_MAX_XID_SIZE];
	size_t command_length;
	uint8_t command[LAPM_V44_MAX_XID_SIZE];
	struct v44_offer offer;
};

/*
 * The object is an owned value and contains no pointers into received frames.
 * An initial exchange owns its outgoing command independently of its cached
 * response to the peer. Either direction can finish first. Repeated peer
 * commands replay that response without changing the negotiated selection.
 */
struct lapm_v44_session {
	struct lapm_v44_config config;
	enum lapm_v44_phase phase;
	uint64_t revision;
	bool initialized;
	struct v44_offer command_offer;
	struct v44_offer negotiated_offer;
	struct v44_offer response_offer;
	size_t command_length;
	uint8_t command[LAPM_V44_MAX_XID_SIZE];
	size_t peer_command_length;
	uint8_t peer_command[LAPM_V44_MAX_XID_SIZE];
	size_t response_length;
	uint8_t response[LAPM_V44_MAX_XID_SIZE];
};

enum lapm_v44_result lapm_v44_session_init(
	struct lapm_v44_session *session,
	const struct lapm_v44_config *config);

/* Clears call negotiation while retaining the configured offer and readiness. */
enum lapm_v44_result lapm_v44_session_call_reset(
	struct lapm_v44_session *session);

/*
 * base_fields supplies optional non-V.44 groups and must not contain user data.
 * A normal LAPM sequence reset needs no call here, so negotiated state survives
 * SABME/UA sequence-number initialization.
 */
enum lapm_v44_result lapm_v44_prepare_command(
	const struct lapm_v44_session *session,
	const struct v42_xid_fields *base_fields,
	struct lapm_v44_prepared *prepared);

enum lapm_v44_result lapm_v44_prepare_retransmission(
	const struct lapm_v44_session *session,
	struct lapm_v44_prepared *prepared);

/* An otherwise valid response selecting both algorithms can trigger one new
 * command retaining the frozen V.42bis offer and omitting V.44. Queue and
 * commit this command before restarting the response timer. */
enum lapm_v44_result lapm_v44_prepare_retry_without_v44(
	const struct lapm_v44_session *session,
	const uint8_t *response, size_t response_length,
	struct lapm_v44_prepared *prepared);

/* Exact duplicate commands replay the committed response; base_fields is ignored. */
enum lapm_v44_result lapm_v44_prepare_response(
	const struct lapm_v44_session *session,
	const uint8_t *command,
	size_t command_length,
	const struct v42_xid_fields *base_fields,
	struct lapm_v44_prepared *prepared);

enum lapm_v44_result lapm_v44_commit(
	struct lapm_v44_session *session,
	const struct lapm_v44_prepared *prepared);

/* A malformed response leaves the outstanding command pending for retry. */
enum lapm_v44_result lapm_v44_accept_response(
	struct lapm_v44_session *session,
	const uint8_t *response,
	size_t response_length);

/* Copies the local-perspective selection only after negotiation succeeds. */
enum lapm_v44_result
lapm_v44_get_negotiated_offer(
	const struct lapm_v44_session *session,
	struct v44_offer *offer);

#endif
