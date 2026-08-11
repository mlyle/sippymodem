// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_LAPM_V44_SESSION_H
#define SIPPYMODEM_LAPM_V44_SESSION_H

#include "sippymodem_v42_xid.h"
#include "sippymodem_v44_xid.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	/* The maximum XID information field accepted from the LAPM transport. */
	SIPPYMODEM_LAPM_V44_MAX_XID_SIZE = 128,
};

enum sippymodem_lapm_v44_result {
	SIPPYMODEM_LAPM_V44_OK = 0,
	SIPPYMODEM_LAPM_V44_XID_TOO_LARGE,
	SIPPYMODEM_LAPM_V44_MALFORMED,
	SIPPYMODEM_LAPM_V44_INVALID_STATE,
	SIPPYMODEM_LAPM_V44_STALE_TRANSACTION,
	SIPPYMODEM_LAPM_V44_INVALID_ARGUMENT,
};

enum sippymodem_lapm_v44_phase {
	SIPPYMODEM_LAPM_V44_READY = 0,
	SIPPYMODEM_LAPM_V44_COMMAND_PENDING,
	SIPPYMODEM_LAPM_V44_NEGOTIATED,
	SIPPYMODEM_LAPM_V44_DECLINED,
};

struct sippymodem_lapm_v44_config {
	/* enabled is the opt-in policy; native_v44_ready is a safety interlock. */
	bool enabled;
	bool native_v44_ready;
	struct sippymodem_v44_offer local_offer;
};

struct sippymodem_lapm_v44_session;

/*
 * A prepared value owns the complete XID information field. The caller first
 * copies data[0..length) into a queued control frame and then calls commit.
 * Dropping this value after queue backpressure leaves the session unchanged.
 * Do not modify or reuse a prepared value, or move its session, before commit.
 */
struct sippymodem_lapm_v44_prepared {
	const struct sippymodem_lapm_v44_session *owner;
	uint64_t revision;
	unsigned int action;
	size_t length;
	uint8_t data[SIPPYMODEM_LAPM_V44_MAX_XID_SIZE];
	size_t command_length;
	uint8_t command[SIPPYMODEM_LAPM_V44_MAX_XID_SIZE];
	struct sippymodem_v44_offer offer;
};

/*
 * The object is an owned value and contains no pointers into received frames.
 * One session performs either the command or response side of an initial XID
 * exchange. Mid-link renegotiation and simultaneous commands are deliberately
 * outside this slice.
 */
struct sippymodem_lapm_v44_session {
	struct sippymodem_lapm_v44_config config;
	enum sippymodem_lapm_v44_phase phase;
	uint64_t revision;
	bool initialized;
	bool response_committed;
	struct sippymodem_v44_offer command_offer;
	struct sippymodem_v44_offer negotiated_offer;
	size_t command_length;
	uint8_t command[SIPPYMODEM_LAPM_V44_MAX_XID_SIZE];
	size_t peer_command_length;
	uint8_t peer_command[SIPPYMODEM_LAPM_V44_MAX_XID_SIZE];
	size_t response_length;
	uint8_t response[SIPPYMODEM_LAPM_V44_MAX_XID_SIZE];
};

enum sippymodem_lapm_v44_result sippymodem_lapm_v44_session_init(
	struct sippymodem_lapm_v44_session *session,
	const struct sippymodem_lapm_v44_config *config);

/* Clears call negotiation while retaining the configured offer and readiness. */
enum sippymodem_lapm_v44_result sippymodem_lapm_v44_session_call_reset(
	struct sippymodem_lapm_v44_session *session);

/*
 * base_fields supplies optional non-V.44 groups and must not contain user data.
 * A normal LAPM sequence reset needs no call here, so negotiated state survives
 * SABME/UA sequence-number initialization.
 */
enum sippymodem_lapm_v44_result sippymodem_lapm_v44_prepare_command(
	const struct sippymodem_lapm_v44_session *session,
	const struct sippymodem_v42_xid_fields *base_fields,
	struct sippymodem_lapm_v44_prepared *prepared);

enum sippymodem_lapm_v44_result sippymodem_lapm_v44_prepare_retransmission(
	const struct sippymodem_lapm_v44_session *session,
	struct sippymodem_lapm_v44_prepared *prepared);

/* Exact duplicate commands replay the committed response; base_fields is ignored. */
enum sippymodem_lapm_v44_result sippymodem_lapm_v44_prepare_response(
	const struct sippymodem_lapm_v44_session *session,
	const uint8_t *command,
	size_t command_length,
	const struct sippymodem_v42_xid_fields *base_fields,
	struct sippymodem_lapm_v44_prepared *prepared);

enum sippymodem_lapm_v44_result sippymodem_lapm_v44_commit(
	struct sippymodem_lapm_v44_session *session,
	const struct sippymodem_lapm_v44_prepared *prepared);

/* A malformed response leaves the outstanding command pending for retry. */
enum sippymodem_lapm_v44_result sippymodem_lapm_v44_accept_response(
	struct sippymodem_lapm_v44_session *session,
	const uint8_t *response,
	size_t response_length);

#endif
