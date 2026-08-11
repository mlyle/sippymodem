// SPDX-License-Identifier: BSD-3-Clause

#include "lapm_v44_session.h"

#include <string.h>

enum prepared_action {
	PREPARED_COMMAND = 1,
	PREPARED_RETRANSMISSION,
	PREPARED_RESPONSE,
	PREPARED_DUPLICATE_RESPONSE,
};

static bool session_is_valid(
	const struct sippymodem_lapm_v44_session *session)
{
	return session != NULL && session->initialized;
}

static bool has_enabled_direction(
	const struct sippymodem_v44_offer *offer)
{
	return offer->transmit.enabled || offer->receive.enabled;
}

static struct sippymodem_v44_offer effective_offer(
	const struct sippymodem_lapm_v44_session *session)
{
	struct sippymodem_v44_offer offer = session->config.local_offer;

	if (!session->config.enabled || !session->config.native_v44_ready) {
		offer.transmit.enabled = false;
		offer.receive.enabled = false;
	}
	return offer;
}

static enum sippymodem_lapm_v44_result encode_envelope(
	const struct sippymodem_v42_xid_fields *base_fields,
	const struct sippymodem_v44_offer *v44_offer,
	bool include_v44,
	uint8_t *output,
	size_t *output_length)
{
	struct sippymodem_v42_xid_fields fields;
	uint8_t user_data[SIPPYMODEM_V44_XID_OFFER_SIZE];
	size_t user_data_length = 0;
	enum sippymodem_v42_xid_result v42_result;

	fields = base_fields != NULL ? *base_fields :
		(struct sippymodem_v42_xid_fields) { 0 };
	if (fields.user_data != NULL || fields.user_data_length != 0)
		return SIPPYMODEM_LAPM_V44_INVALID_ARGUMENT;
	if (include_v44) {
		if (sippymodem_v44_xid_encode(
			    v44_offer, user_data, sizeof(user_data),
			    &user_data_length) != SIPPYMODEM_V44_XID_OK)
			return SIPPYMODEM_LAPM_V44_INVALID_ARGUMENT;
		fields.user_data = user_data;
		fields.user_data_length = user_data_length;
	}
	v42_result = sippymodem_v42_xid_encode(
		&fields, output, SIPPYMODEM_LAPM_V44_MAX_XID_SIZE,
		output_length);
	if (v42_result == SIPPYMODEM_V42_XID_OK)
		return SIPPYMODEM_LAPM_V44_OK;
	if (v42_result == SIPPYMODEM_V42_XID_NEED_OUTPUT)
		return SIPPYMODEM_LAPM_V44_XID_TOO_LARGE;
	return SIPPYMODEM_LAPM_V44_INVALID_ARGUMENT;
}

static bool explicitly_claims_v44(const uint8_t *user_data, size_t length)
{
	static const uint8_t prefix[] = { 0xff, 0x40, 0x03, 'V', '4', '4' };

	return length >= sizeof(prefix) &&
	       memcmp(user_data, prefix, sizeof(prefix)) == 0;
}

static enum sippymodem_lapm_v44_result decode_peer_offer(
	const uint8_t *input,
	size_t input_length,
	bool *present,
	bool *private_present,
	struct sippymodem_v44_offer *offer)
{
	struct sippymodem_v42_xid_fields fields;

	if (input == NULL || input_length == 0 || present == NULL ||
	    private_present == NULL || offer == NULL)
		return SIPPYMODEM_LAPM_V44_INVALID_ARGUMENT;
	if (input_length > SIPPYMODEM_LAPM_V44_MAX_XID_SIZE)
		return SIPPYMODEM_LAPM_V44_XID_TOO_LARGE;
	if (sippymodem_v42_xid_decode(input, input_length, &fields) !=
	    SIPPYMODEM_V42_XID_OK)
		return SIPPYMODEM_LAPM_V44_MALFORMED;
	*private_present = fields.private_negotiation != NULL;
	if (fields.user_data == NULL ||
	    !explicitly_claims_v44(
		    fields.user_data, fields.user_data_length)) {
		*present = false;
		return SIPPYMODEM_LAPM_V44_OK;
	}
	if (sippymodem_v44_xid_decode(
		    fields.user_data, fields.user_data_length, offer) !=
	    SIPPYMODEM_V44_XID_OK)
		return SIPPYMODEM_LAPM_V44_MALFORMED;
	*present = true;
	return SIPPYMODEM_LAPM_V44_OK;
}

enum sippymodem_lapm_v44_result sippymodem_lapm_v44_session_init(
	struct sippymodem_lapm_v44_session *session,
	const struct sippymodem_lapm_v44_config *config)
{
	struct sippymodem_lapm_v44_session initialized = { 0 };
	if (session == NULL || config == NULL ||
	    !sippymodem_v44_parameters_are_valid(
		    &config->local_offer.transmit.parameters) ||
	    !sippymodem_v44_parameters_are_valid(
		    &config->local_offer.receive.parameters))
		return SIPPYMODEM_LAPM_V44_INVALID_ARGUMENT;
	initialized.config = *config;
	initialized.phase = SIPPYMODEM_LAPM_V44_READY;
	initialized.revision = 1;
	initialized.initialized = true;
	*session = initialized;
	return SIPPYMODEM_LAPM_V44_OK;
}

enum sippymodem_lapm_v44_result sippymodem_lapm_v44_session_call_reset(
	struct sippymodem_lapm_v44_session *session)
{
	struct sippymodem_lapm_v44_config config;
	uint64_t revision;

	if (!session_is_valid(session))
		return SIPPYMODEM_LAPM_V44_INVALID_ARGUMENT;
	config = session->config;
	revision = session->revision + 1U;
	memset(session, 0, sizeof(*session));
	session->config = config;
	session->phase = SIPPYMODEM_LAPM_V44_READY;
	session->revision = revision;
	session->initialized = true;
	return SIPPYMODEM_LAPM_V44_OK;
}

enum sippymodem_lapm_v44_result sippymodem_lapm_v44_prepare_command(
	const struct sippymodem_lapm_v44_session *session,
	const struct sippymodem_v42_xid_fields *base_fields,
	struct sippymodem_lapm_v44_prepared *prepared)
{
	struct sippymodem_lapm_v44_prepared candidate = { 0 };
	enum sippymodem_lapm_v44_result result;

	if (!session_is_valid(session) || prepared == NULL)
		return SIPPYMODEM_LAPM_V44_INVALID_ARGUMENT;
	if (session->phase != SIPPYMODEM_LAPM_V44_READY)
		return SIPPYMODEM_LAPM_V44_INVALID_STATE;
	candidate.owner = session;
	candidate.revision = session->revision;
	candidate.action = PREPARED_COMMAND;
	candidate.offer = effective_offer(session);
	result = encode_envelope(
		base_fields, &candidate.offer, session->config.enabled,
		candidate.data, &candidate.length);
	if (result != SIPPYMODEM_LAPM_V44_OK)
		return result;
	*prepared = candidate;
	return SIPPYMODEM_LAPM_V44_OK;
}

enum sippymodem_lapm_v44_result sippymodem_lapm_v44_prepare_retransmission(
	const struct sippymodem_lapm_v44_session *session,
	struct sippymodem_lapm_v44_prepared *prepared)
{
	struct sippymodem_lapm_v44_prepared candidate = { 0 };

	if (!session_is_valid(session) || prepared == NULL)
		return SIPPYMODEM_LAPM_V44_INVALID_ARGUMENT;
	if (session->phase != SIPPYMODEM_LAPM_V44_COMMAND_PENDING)
		return SIPPYMODEM_LAPM_V44_INVALID_STATE;
	candidate.owner = session;
	candidate.revision = session->revision;
	candidate.action = PREPARED_RETRANSMISSION;
	candidate.offer = session->command_offer;
	candidate.length = session->command_length;
	memcpy(candidate.data, session->command, candidate.length);
	*prepared = candidate;
	return SIPPYMODEM_LAPM_V44_OK;
}

enum sippymodem_lapm_v44_result sippymodem_lapm_v44_prepare_response(
	const struct sippymodem_lapm_v44_session *session,
	const uint8_t *command,
	size_t command_length,
	const struct sippymodem_v42_xid_fields *base_fields,
	struct sippymodem_lapm_v44_prepared *prepared)
{
	struct sippymodem_lapm_v44_prepared candidate = { 0 };
	struct sippymodem_v42_xid_fields selected_fields;
	struct sippymodem_v44_offer peer_offer;
	bool peer_present;
	bool private_present;
	bool select_v44;
	enum sippymodem_lapm_v44_result result;

	if (!session_is_valid(session) || prepared == NULL)
		return SIPPYMODEM_LAPM_V44_INVALID_ARGUMENT;
	if ((session->phase == SIPPYMODEM_LAPM_V44_NEGOTIATED ||
	     session->phase == SIPPYMODEM_LAPM_V44_DECLINED) &&
	    session->response_committed && command != NULL &&
	    command_length == session->peer_command_length &&
	    memcmp(command, session->peer_command, command_length) == 0) {
		candidate.owner = session;
		candidate.revision = session->revision;
		candidate.action = PREPARED_DUPLICATE_RESPONSE;
		candidate.offer = session->negotiated_offer;
		candidate.length = session->response_length;
		memcpy(candidate.data, session->response, candidate.length);
		*prepared = candidate;
		return SIPPYMODEM_LAPM_V44_OK;
	}
	if (session->phase != SIPPYMODEM_LAPM_V44_READY)
		return SIPPYMODEM_LAPM_V44_INVALID_STATE;
	result = decode_peer_offer(
		command, command_length, &peer_present, &private_present,
		&peer_offer);
	if (result != SIPPYMODEM_LAPM_V44_OK)
		return result;
	candidate.owner = session;
	candidate.revision = session->revision;
	candidate.action = PREPARED_RESPONSE;
	candidate.command_length = command_length;
	memcpy(candidate.command, command, command_length);
	candidate.offer = effective_offer(session);
	if (peer_present &&
	    sippymodem_v44_xid_negotiate(
		    &candidate.offer, &peer_offer, &candidate.offer) !=
		    SIPPYMODEM_V44_XID_OK)
		return SIPPYMODEM_LAPM_V44_INVALID_ARGUMENT;
	else if (!peer_present) {
		candidate.offer.transmit.enabled = false;
		candidate.offer.receive.enabled = false;
	}
	select_v44 = peer_present && has_enabled_direction(&candidate.offer);
	selected_fields = base_fields != NULL ? *base_fields :
		(struct sippymodem_v42_xid_fields) { 0 };
	if (select_v44) {
		selected_fields.private_negotiation = NULL;
		selected_fields.private_negotiation_length = 0;
	}
	result = encode_envelope(
		&selected_fields, &candidate.offer, select_v44,
		candidate.data, &candidate.length);
	if (result != SIPPYMODEM_LAPM_V44_OK)
		return result;
	*prepared = candidate;
	return SIPPYMODEM_LAPM_V44_OK;
}

enum sippymodem_lapm_v44_result sippymodem_lapm_v44_commit(
	struct sippymodem_lapm_v44_session *session,
	const struct sippymodem_lapm_v44_prepared *prepared)
{
	if (!session_is_valid(session) || prepared == NULL)
		return SIPPYMODEM_LAPM_V44_INVALID_ARGUMENT;
	if (prepared->length > SIPPYMODEM_LAPM_V44_MAX_XID_SIZE ||
	    prepared->command_length > SIPPYMODEM_LAPM_V44_MAX_XID_SIZE)
		return SIPPYMODEM_LAPM_V44_INVALID_ARGUMENT;
	if (prepared->owner != session || prepared->revision != session->revision)
		return SIPPYMODEM_LAPM_V44_STALE_TRANSACTION;
	switch (prepared->action) {
	case PREPARED_COMMAND:
		if (session->phase != SIPPYMODEM_LAPM_V44_READY)
			return SIPPYMODEM_LAPM_V44_STALE_TRANSACTION;
		session->command_offer = prepared->offer;
		session->command_length = prepared->length;
		memcpy(session->command, prepared->data, prepared->length);
		session->phase = SIPPYMODEM_LAPM_V44_COMMAND_PENDING;
		break;
	case PREPARED_RETRANSMISSION:
		if (session->phase != SIPPYMODEM_LAPM_V44_COMMAND_PENDING ||
		    prepared->length != session->command_length ||
		    memcmp(prepared->data, session->command, prepared->length) != 0)
			return SIPPYMODEM_LAPM_V44_STALE_TRANSACTION;
		break;
	case PREPARED_RESPONSE:
		if (session->phase != SIPPYMODEM_LAPM_V44_READY)
			return SIPPYMODEM_LAPM_V44_STALE_TRANSACTION;
		session->negotiated_offer = prepared->offer;
		session->response_committed = true;
		session->peer_command_length = prepared->command_length;
		memcpy(session->peer_command, prepared->command,
		       prepared->command_length);
		session->response_length = prepared->length;
		memcpy(session->response, prepared->data, prepared->length);
		session->phase = has_enabled_direction(&prepared->offer) ?
			SIPPYMODEM_LAPM_V44_NEGOTIATED :
			SIPPYMODEM_LAPM_V44_DECLINED;
		break;
	case PREPARED_DUPLICATE_RESPONSE:
		if ((session->phase != SIPPYMODEM_LAPM_V44_NEGOTIATED &&
		     session->phase != SIPPYMODEM_LAPM_V44_DECLINED) ||
		    !session->response_committed ||
		    prepared->length != session->response_length ||
		    memcmp(prepared->data, session->response, prepared->length) != 0)
			return SIPPYMODEM_LAPM_V44_STALE_TRANSACTION;
		break;
	default:
		return SIPPYMODEM_LAPM_V44_INVALID_ARGUMENT;
	}
	session->revision++;
	return SIPPYMODEM_LAPM_V44_OK;
}

enum sippymodem_lapm_v44_result sippymodem_lapm_v44_accept_response(
	struct sippymodem_lapm_v44_session *session,
	const uint8_t *response,
	size_t response_length)
{
	struct sippymodem_v44_offer peer_offer;
	struct sippymodem_v44_offer selected;
	bool peer_present;
	bool private_present;
	enum sippymodem_lapm_v44_result result;

	if (!session_is_valid(session))
		return SIPPYMODEM_LAPM_V44_INVALID_ARGUMENT;
	if (session->phase != SIPPYMODEM_LAPM_V44_COMMAND_PENDING)
		return SIPPYMODEM_LAPM_V44_INVALID_STATE;
	selected = session->command_offer;
	result = decode_peer_offer(
		response, response_length, &peer_present, &private_present,
		&peer_offer);
	if (result != SIPPYMODEM_LAPM_V44_OK)
		return result;
	if (peer_present && private_present)
		return SIPPYMODEM_LAPM_V44_MALFORMED;
	if (peer_present &&
	    sippymodem_v44_xid_negotiate(
		    &session->command_offer, &peer_offer, &selected) !=
		    SIPPYMODEM_V44_XID_OK)
		return SIPPYMODEM_LAPM_V44_MALFORMED;
	else if (!peer_present) {
		selected.transmit.enabled = false;
		selected.receive.enabled = false;
	}
	session->negotiated_offer = selected;
	session->phase = has_enabled_direction(&selected) ?
		SIPPYMODEM_LAPM_V44_NEGOTIATED :
		SIPPYMODEM_LAPM_V44_DECLINED;
	session->revision++;
	return SIPPYMODEM_LAPM_V44_OK;
}

enum sippymodem_lapm_v44_result
sippymodem_lapm_v44_get_negotiated_offer(
	const struct sippymodem_lapm_v44_session *session,
	struct sippymodem_v44_offer *offer)
{
	if (!session_is_valid(session) || offer == NULL)
		return SIPPYMODEM_LAPM_V44_INVALID_ARGUMENT;
	if (session->phase != SIPPYMODEM_LAPM_V44_NEGOTIATED)
		return SIPPYMODEM_LAPM_V44_INVALID_STATE;
	*offer = session->negotiated_offer;
	return SIPPYMODEM_LAPM_V44_OK;
}
