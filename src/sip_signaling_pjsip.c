// SPDX-License-Identifier: BSD-3-Clause

#include "sip_signaling.h"
#include "sip_uri.h"
#include "sdp.h"
#include "sdp_pjsip.h"
#include "call_number.h"

#include <pjsua-lib/pjsua.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum {
	SIP_ACCOUNT_URI_MAX = APP_CONFIG_TEXT_MAX * 2 + 16,
	SIP_CALL_IDENTITY_MAX = 256,
	SIP_CALL_REASON_MAX = 128,
	SIP_MEDIA_ADDRESS_MAX = 64,
	SIP_REGISTRATION_REASON_MAX = 128,
	SIP_UNREGISTER_TIMEOUT_MS = 1000,
};

struct sip_signaling {
	struct app_config config;
	struct sip_signaling_callbacks callbacks;
	struct sip_server *server;
	struct sip_endpoint *endpoint;
	struct sip_endpoint *call_endpoint;
	bool owns_server;
	pjsua_call_id call_id;
	uint64_t call_generation;
	char called_number[128];
	char caller_number[128];
	int last_library_status;
	unsigned int callback_depth;
	char target_uri[SIP_ACCOUNT_URI_MAX];
	char call_remote_identity[SIP_CALL_IDENTITY_MAX];
	char call_reason[SIP_CALL_REASON_MAX];
	char media_remote_address[SIP_MEDIA_ADDRESS_MAX];
	enum sip_call_direction call_direction;
	enum sip_call_state call_state;
	enum sip_media_state media_state;
	enum sip_media_address_family media_remote_family;
	unsigned int incoming_response_code;
	unsigned int media_remote_port;
	bool call_starting;
	bool opened;
	bool closing;
	bool close_requested;
};

struct sip_transport {
	struct sip_config config;
	pjsua_transport_id id;
	pj_sockaddr local_address;
	unsigned int references;
};

struct sip_endpoint {
	struct sip_server *server;
	struct app_config config;
	struct sip_transport *transport;
	unsigned int id;
	char request_user[APP_CONFIG_TEXT_MAX];
	char request_host[APP_CONFIG_TEXT_MAX];
	bool (*accept_line)(void *, void *);
	void *admission_context;
	bool opened, starting;
	pjsua_acc_id account_id;
	unsigned int local_port;
	char login_buffer[APP_CONFIG_TEXT_MAX];
	char identity_uri[SIP_ACCOUNT_URI_MAX];
	char registrar_uri[SIP_ACCOUNT_URI_MAX];
	char proxy_uri[SIP_ACCOUNT_URI_MAX];
	char contact_uri[SIP_ACCOUNT_URI_MAX];
	char registration_reason[SIP_REGISTRATION_REASON_MAX];
	const char *account_user;
	const char *account_password;
	const char *account_domain;
	enum sip_registration_state registration_state;
	unsigned int registration_code;
	int registration_library_status;
	uint64_t state_sequence;
	bool registration_may_exist;
	bool account_configured;
};

/* PJSUA has process callbacks and one endpoint. This bridge selects the owned
 * server; call/account ids select a line within that server. */
struct sip_server {
	struct sip_signaling *administrative;
	struct sip_signaling *lines[32];
	unsigned int max_lines;
	struct sip_endpoint *endpoint;
	struct sip_endpoint *endpoints[PJSUA_MAX_ACC];
	struct sip_transport transports[PJSUA_MAX_ACC];
	pjsip_module incoming_filter;
	unsigned int next_endpoint_id;
	bool default_enabled;
	bool library_created;
	bool opened;
	bool closing;
	bool retired[PJSUA_MAX_CALLS];
};

static struct sip_server *pjsua_owner;
static void pjsip_trace(int level, const char *data, int length);

/* PJSUA logging is process-global. Scope it to each native SDK entry and
 * restore the embedding logger on every return, including failure/unwind.
 * The owned endpoint has no SDK worker threads. */
struct sdk_log_scope { pj_log_func *writer; int level; bool active; };
static pj_bool_t filter_initial_invite(pjsip_rx_data *request);
static void find_incoming_account(const pjsip_rx_data *request, pjsua_acc_id *account_id);

static struct sdk_log_scope enter_log_scope(const struct sip_server *server)
{
	struct sdk_log_scope scope = { 0 };
	if (server == NULL || (pjsua_owner != NULL && pjsua_owner != server))
		return scope;
	scope.writer = pj_log_get_log_func();
	scope.level = pj_log_get_level();
	scope.active = true;
	pj_log_set_log_func(pjsip_trace);
	pj_log_set_level((int)server->administrative->config.sip.log_level);
	return scope;
}
static void leave_log_scope(struct sdk_log_scope *scope)
{
	if (scope->active) {
		pj_log_set_log_func(scope->writer);
		pj_log_set_level(scope->level);
	}
}
#define SDK_LOG_SCOPE(server) \
	struct sdk_log_scope log_scope __attribute__((cleanup(leave_log_scope))) = enter_log_scope(server)


static bool line_is_owned(const struct sip_server *server,
	const struct sip_signaling *line)
{
	if (server == NULL || line == NULL)
		return false;
	for (unsigned int index = 0; index < server->max_lines; ++index)
		if (server->lines[index] == line)
			return true;
	return false;
}

static bool incoming_allowed(struct sip_signaling *line)
{
	if (line->callbacks.admit == NULL)
		return true;
	line->callback_depth++;
	bool allowed = line->callbacks.admit(line->callbacks.context);
	line->callback_depth--;
	return allowed && line->opened && !line->closing && !line->close_requested;
}

static bool endpoint_allows_line(struct sip_endpoint *endpoint, struct sip_signaling *line)
{
	if (endpoint->accept_line == NULL)
		return true;
	line->callback_depth++;
	bool allowed = endpoint->accept_line(endpoint->admission_context, line->callbacks.line_owner);
	line->callback_depth--;
	return allowed && endpoint->opened && line->opened && !line->closing && !line->close_requested;
}

static bool callback_active(const struct sip_server *server)
{
	if (server->administrative->callback_depth != 0)
		return true;
	for (unsigned int index = 0; index < server->max_lines; ++index)
		if (server->lines[index] != NULL && server->lines[index]->callback_depth != 0)
			return true;
	return false;
}

static struct sip_endpoint *owner_from_account(pjsua_acc_id account_id)
{
	struct sip_server *server = pjsua_owner;
	if (server == NULL || server->closing || account_id < 0 ||
	    (unsigned int)account_id >= PJSUA_MAX_ACC || !pjsua_acc_is_valid(account_id))
		return NULL;
	struct sip_endpoint *owner = pjsua_acc_get_user_data(account_id);
	for (unsigned int index = 0; index < PJSUA_MAX_ACC; ++index) {
		struct sip_endpoint *endpoint = server->endpoints[index];
		if (endpoint == owner && endpoint != NULL &&
		    (endpoint->account_id == account_id || endpoint->starting))
			return endpoint;
	}
	return NULL;
}

static struct sip_signaling *owner_from_call(pjsua_call_id call_id, bool incoming_creation)
{
	struct sip_server *server = pjsua_owner;
	if (server == NULL || server->closing)
		return NULL;
	struct sip_signaling *line = pjsua_call_get_user_data(call_id);
	if (line_is_owned(server, line)) {
		if (!line->opened || (line->call_id != call_id && !line->call_starting))
			return NULL;
		line->call_id = call_id;
		return line;
	}
	pjsua_call_info info;
	if (pjsua_call_get_info(call_id, &info) != PJ_SUCCESS ||
	    info.state == PJSIP_INV_STATE_DISCONNECTED)
		return NULL;
	struct sip_endpoint *endpoint = owner_from_account(info.acc_id);
	if (endpoint == NULL || !endpoint->opened)
		return NULL;
	/* An unowned UAS call can only be attached by its creation callbacks.
	 * Rejected calls and late media/state changes must not claim an idle line. */
	if (info.role == PJSIP_ROLE_UAS && (!incoming_creation ||
	    (info.state != PJSIP_INV_STATE_NULL && info.state != PJSIP_INV_STATE_INCOMING)))
		return NULL;
	if (call_id >= 0 && (unsigned int)call_id < PJSUA_MAX_CALLS && server->retired[call_id]) {
		/* SDK slots are reused. Only the fresh UAS creation callbacks may
		 * admit a NULL-userdata slot again; late state/media callbacks from
		 * the retired incarnation cannot assign it to an idle line. */
		if (!incoming_creation || info.role != PJSIP_ROLE_UAS ||
		    (info.state != PJSIP_INV_STATE_NULL && info.state != PJSIP_INV_STATE_INCOMING))
			return NULL;
		server->retired[call_id] = false;
	}
	for (unsigned int index = 0; index < server->max_lines; ++index) {
		line = server->lines[index];
		if (line == NULL || !line->opened || line->closing || line->close_requested)
			continue;
		if (info.role == PJSIP_ROLE_UAC) {
			if (!line->call_starting)
				continue;
		} else {
			if (line->call_id != PJSUA_INVALID_ID || line->call_starting ||
			    !endpoint_allows_line(endpoint, line) ||
			    !incoming_allowed(line))
				continue;
			line->call_direction = SIP_CALL_DIRECTION_INCOMING;
			line->incoming_response_code = 0;
			line->call_generation++;
			line->called_number[0] = '\0';
			line->caller_number[0] = '\0';
		}
		line->call_endpoint = endpoint;
		line->call_id = call_id;
		(void)pjsua_call_set_user_data(call_id, line);
		return line;
	}
	return NULL;
}



static void callback_enter(struct sip_signaling *signaling)
{
	signaling->callback_depth++;
}

static void callback_leave(struct sip_signaling *signaling)
{
	if (signaling->callback_depth != 0)
		signaling->callback_depth--;
}

static void emit_library_error(struct sip_signaling *signaling,
			       const char *operation, pj_status_t status)
{
	char message[PJ_ERR_MSG_SIZE];
	struct sip_signaling_error error;

	signaling->last_library_status = (int)status;
	if (signaling->callbacks.error == NULL)
		return;
	pj_strerror(status, message, sizeof(message));
	error.operation = operation;
	error.library_status = (int)status;
	error.message = message;
	callback_enter(signaling);
	signaling->callbacks.error(signaling->callbacks.context, &error);
	callback_leave(signaling);
}

static void emit_trace(
	struct sip_signaling *signaling,
	enum sip_trace_origin origin, int level,
	const char *data, size_t size)
{
	struct sip_trace_record record;

	if (signaling == NULL || signaling->callbacks.trace == NULL ||
	    data == NULL || size == 0)
		return;
	record.origin = origin;
	record.level = level;
	record.data = data;
	record.size = size;
	callback_enter(signaling);
	signaling->callbacks.trace(signaling->callbacks.context, &record);
	callback_leave(signaling);
}

static void pjsip_trace(int level, const char *data, int length)
{
	if (length > 0)
		emit_trace(pjsua_owner != NULL ? pjsua_owner->administrative : NULL, SIP_TRACE_LIBRARY, level,
			   data, (size_t)length);
}

static void copy_pj_text(char *output, size_t capacity,
			 const pj_str_t *text)
{
	size_t size = 0;

	if (text != NULL && text->ptr != NULL && text->slen > 0) {
		size = (size_t)text->slen;
		if (size >= capacity)
			size = capacity - 1;
		memcpy(output, text->ptr, size);
	}
	output[size] = '\0';
}

static void emit_call_event(struct sip_signaling *signaling,
			    enum sip_call_state state,
			    const pjsua_call_info *info)
{
	struct sip_call_event event;

	if (signaling == NULL)
		return;
	copy_pj_text(signaling->call_remote_identity,
		     sizeof(signaling->call_remote_identity),
		     info != NULL ? &info->remote_info : NULL);
	copy_pj_text(signaling->call_reason, sizeof(signaling->call_reason),
		     info != NULL ? &info->last_status_text : NULL);
	signaling->call_state = state;
	if (signaling->callbacks.call_changed == NULL)
		return;
	event.generation = signaling->call_generation;
	event.endpoint_id = signaling->call_endpoint != NULL ? signaling->call_endpoint->id : 0;
	event.called_number = signaling->called_number;
	event.caller_number = signaling->caller_number;
	event.direction = signaling->call_direction;
	event.state = state;
	event.status_code =
		info != NULL && info->last_status > 0
			? (unsigned int)info->last_status
			: 0;
	event.reason = signaling->call_reason;
	event.remote_identity = signaling->call_remote_identity;
	callback_enter(signaling);
	signaling->callbacks.call_changed(signaling->callbacks.context, &event);
	callback_leave(signaling);
}

static void emit_media_event(
	struct sip_signaling *signaling,
	enum sip_media_state state,
	enum sip_media_address_family family,
	const char *remote_address, unsigned int remote_port)
{
	struct sip_media_event event;
	int payload_type;

	if (signaling == NULL)
		return;
	if (state == SIP_MEDIA_INACTIVE) {
		family = SIP_MEDIA_ADDRESS_UNSPECIFIED;
		remote_address = "";
		remote_port = 0;
	}
	if (remote_address == NULL)
		return;
	if (signaling->media_state == state &&
	    signaling->media_remote_family == family &&
	    signaling->media_remote_port == remote_port &&
	    strcmp(signaling->media_remote_address, remote_address) == 0)
		return;
	(void)snprintf(signaling->media_remote_address,
		       sizeof(signaling->media_remote_address), "%s",
		       remote_address);
	signaling->media_state = state;
	signaling->media_remote_family = family;
	signaling->media_remote_port = remote_port;
	if (signaling->callbacks.media_changed == NULL)
		return;
	payload_type = media_codec_default_payload_type(
		signaling->config.media.codec);
	event.state = state;
	event.remote_address_family = family;
	event.codec = signaling->config.media.codec;
	event.payload_type = payload_type >= 0 ? (unsigned int)payload_type : 0;
	event.sample_rate = MEDIA_SAMPLE_RATE;
	event.channels = 1;
	event.local_port = signaling->config.media.rtp_listen_port;
	event.remote_address = signaling->media_remote_address;
	event.remote_port = remote_port;
	callback_enter(signaling);
	signaling->callbacks.media_changed(signaling->callbacks.context, &event);
	callback_leave(signaling);
}

static void emit_media_inactive(struct sip_signaling *signaling)
{
	if (signaling != NULL &&
	    signaling->media_state != SIP_MEDIA_INACTIVE)
		emit_media_event(signaling, SIP_MEDIA_INACTIVE,
				 SIP_MEDIA_ADDRESS_UNSPECIFIED, "", 0);
}

static void pjsip_call_changed(pjsua_call_id call_id, pjsip_event *event)
{
	struct sip_signaling *signaling = owner_from_call(call_id, false);
	pjsua_call_info info;

	(void)event;
	if (signaling == NULL ||
	    (call_id != signaling->call_id && !signaling->call_starting) ||
	    pjsua_call_get_info(call_id, &info) != PJ_SUCCESS)
		return;
	if (signaling->call_starting)
		signaling->call_id = call_id;
	switch (info.state) {
	case PJSIP_INV_STATE_CALLING:
		emit_call_event(signaling, SIP_CALL_DIALING, &info);
		break;
	case PJSIP_INV_STATE_EARLY:
		emit_call_event(signaling, SIP_CALL_EARLY, &info);
		break;
	case PJSIP_INV_STATE_CONFIRMED:
		emit_call_event(signaling, SIP_CALL_CONNECTED, &info);
		break;
	case PJSIP_INV_STATE_DISCONNECTED:
		emit_media_inactive(signaling);
		signaling->call_id = PJSUA_INVALID_ID;
		signaling->call_starting = false;
		signaling->incoming_response_code = 0;
		emit_call_event(signaling, SIP_CALL_IDLE, &info);
		break;
	case PJSIP_INV_STATE_NULL:
	case PJSIP_INV_STATE_INCOMING:
	case PJSIP_INV_STATE_CONNECTING:
		break;
	}
}

static void pjsip_stream_precreate(
	pjsua_call_id call_id, pjsua_on_stream_precreate_param *parameters)
{
	struct sip_signaling *signaling = owner_from_call(call_id, false);
	const pj_sockaddr *remote;
	enum sip_media_address_family family;
	char address[SIP_MEDIA_ADDRESS_MAX];
	unsigned int port;

	if (signaling == NULL || parameters == NULL ||
	    signaling->config.media.rtp_listen_port == 0 ||
	    parameters->stream_info.type != PJMEDIA_TYPE_AUDIO ||
	    (call_id != signaling->call_id && !signaling->call_starting))
		return;
	remote = &parameters->stream_info.info.aud.rem_addr;
	port = pj_sockaddr_get_port(remote);
	if (port == 0 || !pj_sockaddr_has_addr(remote)) {
		emit_media_inactive(signaling);
		return;
	}
	if (remote->addr.sa_family == pj_AF_INET())
		family = SIP_MEDIA_ADDRESS_IPV4;
	else if (remote->addr.sa_family == pj_AF_INET6())
		family = SIP_MEDIA_ADDRESS_IPV6;
	else {
		emit_library_error(signaling, "read negotiated RTP address",
				   PJ_EAFNOTSUP);
		return;
	}
	if (pj_sockaddr_print(remote, address, sizeof(address), 0) == NULL) {
		emit_library_error(signaling, "format negotiated RTP address",
				   PJ_EINVAL);
		return;
	}
	emit_media_event(signaling, SIP_MEDIA_CONFIGURED, family,
			 address, port);
}

static void pjsip_sdp_created(pjsua_call_id call_id,
			      pjmedia_sdp_session *local_sdp,
			      pj_pool_t *pool,
			      const pjmedia_sdp_session *remote_sdp)
{
	struct sip_signaling *signaling = owner_from_call(call_id, remote_sdp != NULL);
	struct sdp_audio_description description;
	pj_status_t status;

	if (signaling == NULL || signaling->config.media.rtp_listen_port == 0)
		return;
	if (sdp_audio_description_init(
		    &description, signaling->config.media.codec,
		    (uint16_t)signaling->config.media.rtp_listen_port,
		    signaling->config.media.frame_duration_ms, true) !=
	    SDP_OK) {
		emit_library_error(signaling, "describe modem audio", PJ_EINVAL);
		return;
	}
	status = sdp_pjsip_apply(
		local_sdp, pool, remote_sdp != NULL, &description,
		signaling->call_endpoint != NULL ? signaling->call_endpoint->config.sip.public_address :
		signaling->config.sip.public_address);
	if (status != PJ_SUCCESS)
		emit_library_error(signaling, "apply modem audio SDP", status);
}

static void copy_called_number(char *number, size_t capacity, const pj_str_t *user)
{
	number[0] = '\0';
	if (user == NULL || user->ptr == NULL || user->slen <= 0 ||
	    (size_t)user->slen >= capacity || (user->slen == 1 && user->ptr[0] == '+'))
		return;
	for (pj_ssize_t index = 0; index < user->slen; ++index) {
		char ch = user->ptr[index];
		if (!((ch >= '0' && ch <= '9') || (index == 0 && ch == '+')))
			return;
	}
	copy_pj_text(number, capacity, user);
}

static bool caller_withheld(const pjsip_msg *message)
{
	pj_str_t name = pj_str((char *)"Privacy");
	pjsip_generic_string_hdr *header = pjsip_msg_find_hdr_by_name(message, &name, NULL);
	for (; header != NULL; header = pjsip_msg_find_hdr_by_name(message, &name, header->next)) {
		if (header->type != PJSIP_H_OTHER)
			return true;
		const pj_str_t *value = &header->hvalue;
		for (pj_ssize_t index = 0; index < value->slen;) {
			while (index < value->slen && (value->ptr[index] == ' ' || value->ptr[index] == '\t' ||
			       value->ptr[index] == ';' || value->ptr[index] == ','))
				index++;
			pj_ssize_t begin = index;
			while (index < value->slen && value->ptr[index] != ';' && value->ptr[index] != ',' &&
			       value->ptr[index] != ' ' && value->ptr[index] != '\t')
				index++;
			pj_str_t token = { value->ptr + begin, index - begin };
			/* 'header' suppression is a conservative local display policy. */
			if (pj_stricmp2(&token, "user") == 0 || pj_stricmp2(&token, "id") == 0 ||
			    pj_stricmp2(&token, "header") == 0)
				return true;
		}
	}
	return false;
}

static void pjsip_incoming_call(pjsua_acc_id account_id,
				pjsua_call_id call_id,
				pjsip_rx_data *request)
{
	struct sip_signaling *signaling = owner_from_call(call_id, true);
	pjsua_call_info info;
	pj_status_t status;
	unsigned int response_code;

	(void)account_id;
	if (signaling == NULL) {
		(void)pjsua_call_answer(call_id, 486, NULL, NULL);
		return;
	}
	if (signaling->closing || signaling->close_requested ||
	    (signaling->call_id != PJSUA_INVALID_ID && signaling->call_id != call_id) || signaling->call_starting) {
		status = pjsua_call_answer(call_id, 486, NULL, NULL);
		if (status != PJ_SUCCESS)
			emit_library_error(signaling,
					   "reject competing SIP call", status);
		return;
	}
	status = pjsua_call_get_info(call_id, &info);
	if (status != PJ_SUCCESS) {
		emit_library_error(signaling, "inspect incoming SIP call", status);
		(void)pjsua_call_answer(call_id, 500, NULL, NULL);
		return;
	}

	signaling->call_id = call_id;
	/* Request-URI user is a private routing fact. Never route on a truncated
	 * value or an unsupported spelling; missing facts select a default route. */
	signaling->called_number[0] = '\0';
	signaling->caller_number[0] = '\0';
	if (request != NULL && request->msg_info.msg != NULL) {
		pjsip_uri *uri = pjsip_uri_get_uri(request->msg_info.msg->line.req.uri);
		if (uri != NULL && (PJSIP_URI_SCHEME_IS_SIP(uri) || PJSIP_URI_SCHEME_IS_SIPS(uri))) {
			copy_called_number(signaling->called_number, sizeof(signaling->called_number),
				&((pjsip_sip_uri *)uri)->user);
		}
		pjsip_from_hdr *from = request->msg_info.from;
		uri = from != NULL ? pjsip_uri_get_uri(from->uri) : NULL;
		if (!caller_withheld(request->msg_info.msg) && uri != NULL &&
		    (PJSIP_URI_SCHEME_IS_SIP(uri) || PJSIP_URI_SCHEME_IS_SIPS(uri))) {
			const pj_str_t *user = &((pjsip_sip_uri *)uri)->user;
			if (user->slen > 0)
				call_number_copy(signaling->caller_number, sizeof(signaling->caller_number),
					user->ptr, (size_t)user->slen);
		}
	}
	signaling->call_direction = SIP_CALL_DIRECTION_INCOMING;
	signaling->incoming_response_code = 0;
	emit_call_event(signaling, SIP_CALL_INCOMING_PENDING, &info);
	if (signaling->close_requested || signaling->call_id != call_id ||
	    signaling->incoming_response_code >= 200)
		return;
	response_code = signaling->config.sip.automatic_answer ? 200 : 180;
	signaling->incoming_response_code = response_code;
	status = pjsua_call_answer(call_id, response_code, NULL, NULL);
	if (status != PJ_SUCCESS) {
		emit_library_error(signaling, "answer incoming SIP call", status);
		(void)pjsua_call_hangup(call_id, 500, NULL, NULL);
	}
}

static void emit_registration_event(
	struct sip_endpoint *endpoint,
	enum sip_registration_state state,
	unsigned int status_code, pj_status_t library_status,
	unsigned int expires_seconds, const pj_str_t *reason)
{
	struct sip_registration_event event;
	if (endpoint == NULL)
		return;
	copy_pj_text(endpoint->registration_reason,
		     sizeof(endpoint->registration_reason), reason);
	endpoint->registration_state = state;
	endpoint->registration_code = status_code;
	endpoint->registration_library_status = library_status;
	endpoint->state_sequence++;
	if (state == SIP_REGISTRATION_REGISTERED)
		endpoint->registration_may_exist = true;
	else if (state == SIP_REGISTRATION_UNREGISTERED)
		endpoint->registration_may_exist = false;
	event.state = state;
	event.status_code = status_code;
	event.library_status = (int)library_status;
	event.expires_seconds = expires_seconds;
	event.reason = endpoint->registration_reason;
	for (unsigned int index = 0; index < endpoint->server->max_lines; ++index) {
		struct sip_signaling *line = endpoint->server->lines[index];
		if (line == NULL || line->endpoint != endpoint)
			continue;
		if (line->callbacks.registration_changed != NULL) {
			callback_enter(line);
			line->callbacks.registration_changed(line->callbacks.context, &event);
			callback_leave(line);
		}
	}
}

static void pjsip_registration_started(pjsua_acc_id account_id,
				       pj_bool_t renew)
{
	emit_registration_event(
		owner_from_account(account_id),
		renew ? SIP_REGISTRATION_REGISTERING
		      : SIP_REGISTRATION_UNREGISTERING,
		0, PJ_SUCCESS, 0, NULL);
}

static void pjsip_registration_changed(pjsua_acc_id account_id,
				       pjsua_reg_info *info)
{
	const struct pjsip_regc_cbparam *result;
	enum sip_registration_state state;
	unsigned int status_code;
	unsigned int expires_seconds;
	bool unregistering;
	bool succeeded;

	struct sip_endpoint *endpoint = owner_from_account(account_id);
	if (endpoint == NULL || info == NULL || info->cbparam == NULL)
		return;
	result = info->cbparam;
	unregistering = !info->renew || result->is_unreg;
	succeeded = result->status == PJ_SUCCESS && result->code >= 200 &&
		    result->code < 300;
	if (succeeded)
		state = unregistering
				? SIP_REGISTRATION_UNREGISTERED
				: SIP_REGISTRATION_REGISTERED;
	else
		state = unregistering
				? SIP_REGISTRATION_UNREGISTER_FAILED
				: SIP_REGISTRATION_FAILED;
	status_code = result->code > 0 ? (unsigned int)result->code : 0;
	expires_seconds =
		result->expiration == PJSIP_REGC_EXPIRATION_NOT_SPECIFIED
			? 0
			: result->expiration;
	emit_registration_event(endpoint, state, status_code,
				result->status, expires_seconds, &result->reason);
}

static bool text_is_terminated(const char *text, size_t capacity)
{
	return memchr(text, '\0', capacity) != NULL;
}

static bool account_text_is_terminated(const struct sip_config *sip)
{
	return text_is_terminated(sip->login, sizeof(sip->login)) &&
	       text_is_terminated(sip->user, sizeof(sip->user)) &&
	       text_is_terminated(sip->password, sizeof(sip->password)) &&
	       text_is_terminated(sip->domain, sizeof(sip->domain)) &&
	       text_is_terminated(sip->server, sizeof(sip->server)) &&
	       text_is_terminated(sip->identity_uri, sizeof(sip->identity_uri)) &&
	       text_is_terminated(sip->registrar_uri,
				  sizeof(sip->registrar_uri)) &&
	       text_is_terminated(sip->proxy_uri, sizeof(sip->proxy_uri)) &&
	       text_is_terminated(sip->realm, sizeof(sip->realm)) &&
	       text_is_terminated(sip->target, sizeof(sip->target));
}

static enum sip_signaling_result validate_config(
	const struct app_config *config)
{
	if (config->sip.listen_port > 65535 ||
	    config->media.rtp_listen_port > UINT16_MAX ||
	    config->media.frame_duration_ms == 0 ||
	    config->media.frame_duration_ms >
		    SDP_MAX_PACKET_DURATION_MS ||
	    config->media.jitter_buffer_ms > INT_MAX ||
	    media_codec_name(config->media.codec) == NULL ||
	    !account_text_is_terminated(&config->sip) ||
	    !text_is_terminated(config->sip.public_address,
				sizeof(config->sip.public_address)) ||
	    !text_is_terminated(config->sip.bound_address,
				sizeof(config->sip.bound_address)))
		return SIP_SIGNALING_INVALID_ARGUMENT;
	return SIP_SIGNALING_OK;
}

static bool format_account_uri(char *output, size_t capacity,
			       const char *value, const char *domain)
{
	return sip_uri_format_target(
		       output, capacity, value, NULL, domain) ==
	       SIP_URI_OK;
}

static enum sip_signaling_result resolve_account(
	struct sip_endpoint *endpoint)
{
	const struct sip_config *sip = &endpoint->config.sip;
	const char *user = sip->user;
	const char *password = sip->password;
	const char *domain = sip->domain[0] != '\0' ? sip->domain : sip->server;
	bool any_authentication;
	bool all_credentials;
	int length;

	endpoint->account_configured = false;
	endpoint->identity_uri[0] = '\0';
	endpoint->registrar_uri[0] = '\0';
	endpoint->proxy_uri[0] = '\0';
	endpoint->contact_uri[0] = '\0';
	endpoint->account_user = NULL;
	endpoint->account_password = NULL;
	endpoint->account_domain = NULL;
	if (sip->login[0] != '\0') {
		char *password_separator;
		char *domain_separator;

		memcpy(endpoint->login_buffer, sip->login, strlen(sip->login) + 1);
		password_separator = strchr(endpoint->login_buffer, ':');
		domain_separator = strrchr(endpoint->login_buffer, '@');
		if (password_separator == NULL || domain_separator == NULL ||
		    password_separator == endpoint->login_buffer ||
		    domain_separator <= password_separator + 1 ||
		    domain_separator[1] == '\0')
			return SIP_SIGNALING_INVALID_ARGUMENT;
		*password_separator++ = '\0';
		*domain_separator++ = '\0';
		user = endpoint->login_buffer;
		password = password_separator;
		domain = domain_separator;
	}

	any_authentication = user[0] != '\0' || password[0] != '\0';
	all_credentials = user[0] != '\0' && password[0] != '\0' &&
			  domain[0] != '\0';
	if (!all_credentials) {
		if (any_authentication || sip->registration_enabled)
			return SIP_SIGNALING_INVALID_ARGUMENT;
		endpoint->account_domain = domain[0] != '\0' ? domain : NULL;
		if (sip->identity_uri[0] != '\0') {
			if (!format_account_uri(endpoint->identity_uri,
						sizeof(endpoint->identity_uri), sip->identity_uri, domain))
				return SIP_SIGNALING_INVALID_ARGUMENT;
			endpoint->account_configured = true;
			if (sip->proxy_uri[0] != '\0' &&
			    !format_account_uri(endpoint->proxy_uri,
						sizeof(endpoint->proxy_uri), sip->proxy_uri, NULL))
				return SIP_SIGNALING_INVALID_ARGUMENT;
		} else if (sip->proxy_uri[0] != '\0') {
			return SIP_SIGNALING_INVALID_ARGUMENT;
		}
		return SIP_SIGNALING_OK;
	}
	if (strchr(user, '@') != NULL || strchr(user, ':') != NULL ||
	    strchr(domain, '@') != NULL)
		return SIP_SIGNALING_INVALID_ARGUMENT;

	if (!format_account_uri(endpoint->identity_uri,
				sizeof(endpoint->identity_uri), user, domain))
		return SIP_SIGNALING_INVALID_ARGUMENT;
	if (sip->identity_uri[0] != '\0' &&
	    !format_account_uri(endpoint->identity_uri,
				sizeof(endpoint->identity_uri),
				sip->identity_uri, domain))
		return SIP_SIGNALING_INVALID_ARGUMENT;
	if (sip->registrar_uri[0] != '\0') {
		if (!format_account_uri(endpoint->registrar_uri,
					 sizeof(endpoint->registrar_uri),
					 sip->registrar_uri, NULL))
			return SIP_SIGNALING_INVALID_ARGUMENT;
	} else {
		length = snprintf(endpoint->registrar_uri,
				  sizeof(endpoint->registrar_uri), "sip:%s", domain);
		if (length < 0 ||
		    (size_t)length >= sizeof(endpoint->registrar_uri))
			return SIP_SIGNALING_INVALID_ARGUMENT;
	}
	if (sip->proxy_uri[0] != '\0' &&
	    !format_account_uri(endpoint->proxy_uri,
				sizeof(endpoint->proxy_uri), sip->proxy_uri, NULL))
		return SIP_SIGNALING_INVALID_ARGUMENT;

	endpoint->account_user = user;
	endpoint->account_password = password;
	endpoint->account_domain = domain;
	endpoint->account_configured = true;
	return SIP_SIGNALING_OK;
}

static void configure_media(const struct app_config *config,
			    pjsua_media_config *media)
{
	int jitter_buffer_ms = (int)config->media.jitter_buffer_ms;

	pjsua_media_config_default(media);
	media->clock_rate = MEDIA_SAMPLE_RATE;
	media->snd_clock_rate = MEDIA_SAMPLE_RATE;
	media->channel_count = 1;
	media->no_vad = PJ_TRUE;
	media->ptime = config->media.frame_duration_ms;
	media->audio_frame_ptime = config->media.frame_duration_ms;
	media->ec_tail_len = 0;
	media->jb_init = jitter_buffer_ms;
	media->jb_min_pre = jitter_buffer_ms;
	media->jb_max_pre = jitter_buffer_ms;
	media->jb_max = jitter_buffer_ms;
	media->jb_discard_algo = PJMEDIA_JB_DISCARD_NONE;
}

static pj_status_t configure_codecs(const struct sip_signaling *signaling)
{
	(void)signaling;
	pjsua_codec_info codecs[PJMEDIA_CODEC_MGR_MAX_CODECS];
	unsigned int count = PJ_ARRAY_SIZE(codecs);
	pj_status_t status = pjsua_enum_codecs(codecs, &count);
	if (status != PJ_SUCCESS)
		return status;
	unsigned int selected_count = 0;
	for (unsigned int index = 0; index < count; ++index) {
		bool selected = pj_strcmp2(&codecs[index].codec_id, "PCMU/8000/1") == 0 ||
			pj_strcmp2(&codecs[index].codec_id, "PCMA/8000/1") == 0;
		status = pjsua_codec_set_priority(&codecs[index].codec_id,
			selected ? PJMEDIA_CODEC_PRIO_NORMAL : PJMEDIA_CODEC_PRIO_DISABLED);
		if (status != PJ_SUCCESS)
			return status;
		if (!selected)
			continue;
		selected_count++;
		pjmedia_codec_param parameters;
		status = pjsua_codec_get_param(&codecs[index].codec_id, &parameters);
		if (status != PJ_SUCCESS)
			return status;
		parameters.setting.vad = parameters.setting.cng = parameters.setting.plc = 0;
		status = pjsua_codec_set_param(&codecs[index].codec_id, &parameters);
		if (status != PJ_SUCCESS)
			return status;
	}
	return selected_count == 2 ? PJ_SUCCESS : PJ_ENOTFOUND;
}

static enum sip_signaling_result initialize_library(
	struct sip_signaling *signaling)
{
	pjsua_config user_agent;
	pjsua_logging_config logging;
	pjsua_media_config media;
	pj_status_t status;

	status = pjsua_create();
	if (status != PJ_SUCCESS) {
		emit_library_error(signaling, "create signaling library", status);
		return SIP_SIGNALING_LIBRARY_ERROR;
	}
	signaling->server->library_created = true;

	pjsua_config_default(&user_agent);
	user_agent.thread_cnt = 0;
	user_agent.max_calls = PJSUA_MAX_CALLS;
	user_agent.user_agent = pj_str((char *)"sippymodem");
	user_agent.cb.on_call_state = &pjsip_call_changed;
	user_agent.cb.on_call_sdp_created = &pjsip_sdp_created;
	user_agent.cb.on_stream_precreate = &pjsip_stream_precreate;
	user_agent.cb.on_incoming_call = &pjsip_incoming_call;
	user_agent.cb.on_acc_find_for_incoming = &find_incoming_account;
	user_agent.cb.on_reg_started = &pjsip_registration_started;
	user_agent.cb.on_reg_state2 = &pjsip_registration_changed;
	pjsua_logging_config_default(&logging);
	logging.level = signaling->config.sip.log_level > PJ_LOG_MAX_LEVEL
			? PJ_LOG_MAX_LEVEL
			: signaling->config.sip.log_level;
	/* PJSUA gates even an application log callback by console_level. */
	logging.console_level = signaling->callbacks.trace != NULL ? logging.level : 0;
	logging.msg_logging = signaling->callbacks.trace != NULL;
	logging.cb = &pjsip_trace;
	configure_media(&signaling->config, &media);
	media.thread_cnt = 0;
	media.has_ioqueue = PJ_FALSE;

	status = pjsua_init(&user_agent, &logging, &media);
	if (status != PJ_SUCCESS) {
		emit_library_error(signaling, "initialize signaling library", status);
		return SIP_SIGNALING_LIBRARY_ERROR;
	}
	(void)pjsua_set_no_snd_dev();
	status = configure_codecs(signaling);
	if (status != PJ_SUCCESS) {
		emit_library_error(signaling, "configure modem audio codecs", status);
		return SIP_SIGNALING_LIBRARY_ERROR;
	}
	return SIP_SIGNALING_OK;
}

static enum sip_signaling_result create_transport(
	struct sip_endpoint *endpoint)
{
	struct sip_server *server = endpoint->server;
	struct sip_transport *owned = NULL;
	for (unsigned int index = 0; index < PJSUA_MAX_ACC; ++index) {
		struct sip_transport *candidate = &server->transports[index];
		if (candidate->references != 0 &&
		    candidate->config.listen_port == endpoint->config.sip.listen_port &&
		    strcmp(candidate->config.bound_address, endpoint->config.sip.bound_address) == 0 &&
		    strcmp(candidate->config.public_address, endpoint->config.sip.public_address) == 0) {
			endpoint->transport = candidate;
			candidate->references++;
			endpoint->local_port = pj_sockaddr_get_port(&candidate->local_address);
			return SIP_SIGNALING_OK;
		}
		if (candidate->references == 0 && owned == NULL)
			owned = candidate;
	}
	if (owned == NULL)
		return SIP_SIGNALING_BUSY;
	owned->config = endpoint->config.sip;
	owned->id = PJSUA_INVALID_ID;
	endpoint->transport = owned;
	pjsua_transport_config transport;
	pjsua_transport_info info;
	pj_status_t status;

	pjsua_transport_config_default(&transport);
	transport.port = endpoint->config.sip.listen_port;
	if (endpoint->config.sip.public_address[0] != '\0')
		transport.public_addr =
			pj_str(endpoint->config.sip.public_address);
	if (endpoint->config.sip.bound_address[0] != '\0')
		transport.bound_addr = pj_str(endpoint->config.sip.bound_address);
	status = pjsua_transport_create(PJSIP_TRANSPORT_UDP, &transport,
					&endpoint->transport->id);
	if (status != PJ_SUCCESS) {
		emit_library_error(endpoint->server->administrative, "create SIP UDP transport", status);
		return SIP_SIGNALING_LIBRARY_ERROR;
	}
	endpoint->transport->references = 1;
	status = pjsua_transport_get_info(endpoint->transport->id, &info);
	if (status != PJ_SUCCESS) {
		emit_library_error(endpoint->server->administrative, "inspect SIP UDP transport", status);
		return SIP_SIGNALING_LIBRARY_ERROR;
	}
	endpoint->transport->local_address = info.local_addr;
	endpoint->transport->references = 1;
	endpoint->local_port = pj_sockaddr_get_port(&info.local_addr);
	if (endpoint->local_port == 0) {
		emit_library_error(endpoint->server->administrative, "resolve SIP UDP transport port",
				   PJ_EINVAL);
		return SIP_SIGNALING_LIBRARY_ERROR;
	}
	return SIP_SIGNALING_OK;
}

static pj_status_t make_local_account_media_ephemeral(pjsua_acc_id account_id)
{
	pjsua_acc_config account;
	pj_pool_t *pool = pjsua_pool_create("sip-local-account", 512, 512);
	pj_status_t status;

	if (pool == NULL)
		return PJ_ENOMEM;
	status = pjsua_acc_get_config(account_id, pool, &account);
	if (status == PJ_SUCCESS) {
		/* Media samples use the separately owned RTP endpoint advertised by
		 * the SDP callback. The library transport is therefore only an
		 * internal placeholder and must not claim a process-global fixed
		 * port. */
		account.rtp_cfg.port = 0;
		account.rtp_cfg.port_range = 0;
		status = pjsua_acc_modify(account_id, &account);
	}
	pj_pool_release(pool);
	return status;
}

static enum sip_signaling_result add_account(
	struct sip_endpoint *endpoint)
{
	pjsua_acc_config account;
	enum sip_uri_result contact_result;
	pj_status_t status;

	if (!endpoint->account_configured) {
		status = pjsua_acc_add_local(endpoint->transport->id, PJ_FALSE,
					     &endpoint->account_id);
		if (status == PJ_SUCCESS)
			status = make_local_account_media_ephemeral(
				endpoint->account_id);
	} else {
		pjsua_acc_config_default(&account);
		account.rtp_cfg.port = 0;
		account.id = pj_str(endpoint->identity_uri);
		if (endpoint->config.sip.registration_enabled)
			account.reg_uri = pj_str(endpoint->registrar_uri);
		account.transport_id = endpoint->transport->id;
		account.user_data = endpoint;
		account.register_on_acc_add =
			endpoint->config.sip.registration_enabled;
		account.unreg_timeout = SIP_UNREGISTER_TIMEOUT_MS;
		if (endpoint->account_user != NULL) {
			account.cred_count = 1;
			account.cred_info[0].realm = pj_str(
				endpoint->config.sip.realm[0] != '\0'
					? endpoint->config.sip.realm
					: (char *)"*");
			account.cred_info[0].scheme = pj_str((char *)"digest");
			account.cred_info[0].username =
				pj_str((char *)endpoint->account_user);
			account.cred_info[0].data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;
			account.cred_info[0].data =
				pj_str((char *)endpoint->account_password);
		}
		if (endpoint->proxy_uri[0] != '\0') {
			account.proxy_cnt = 1;
			account.proxy[0] = pj_str(endpoint->proxy_uri);
		}
		if (endpoint->account_user != NULL) {
			contact_result = sip_uri_format_contact(
				endpoint->contact_uri, sizeof(endpoint->contact_uri),
				strcmp(endpoint->request_user, "*") != 0 && endpoint->request_user[0] != '\0' ?
					endpoint->request_user : endpoint->account_user,
				endpoint->config.sip.public_address,
				(uint16_t)endpoint->local_port);
			if (contact_result == SIP_URI_OK)
				account.force_contact = pj_str(endpoint->contact_uri);
			else if (contact_result != SIP_URI_NOT_CONFIGURED)
				return SIP_SIGNALING_INVALID_ARGUMENT;
		}
		status = pjsua_acc_add(&account, PJ_FALSE, &endpoint->account_id);
	}
	if (status != PJ_SUCCESS) {
		emit_library_error(endpoint->server->administrative, "add SIP account", status);
		return SIP_SIGNALING_LIBRARY_ERROR;
	}
	(void)pjsua_acc_set_user_data(endpoint->account_id, endpoint);
	if (endpoint->account_configured)
		emit_trace(endpoint->server->administrative, SIP_TRACE_ADAPTER, 3,
			   "SIP account configured\n",
			   sizeof("SIP account configured\n") - 1);
	return SIP_SIGNALING_OK;
}

static struct sip_endpoint *match_incoming(const pjsip_rx_data *request)
{
	struct sip_server *server = pjsua_owner;
	if (server == NULL || !server->opened || server->closing || request == NULL ||
	    request->msg_info.msg == NULL || request->tp_info.transport == NULL)
		return NULL;
	pjsip_uri *uri = pjsip_uri_get_uri(request->msg_info.msg->line.req.uri);
	if (uri == NULL || !PJSIP_URI_SCHEME_IS_SIP(uri))
		return NULL;
	pjsip_sip_uri *sip_uri = (pjsip_sip_uri *)uri;
	for (unsigned int index = 0; index < PJSUA_MAX_ACC; ++index) {
		struct sip_endpoint *endpoint = server->endpoints[index];
		if (endpoint == NULL || !endpoint->opened || endpoint->transport == NULL ||
		    pj_sockaddr_cmp(&endpoint->transport->local_address,
			&request->tp_info.transport->local_addr) != 0)
			continue;
		if ((strcmp(endpoint->request_user, "*") == 0 ||
		     pj_strcmp2(&sip_uri->user, endpoint->request_user) == 0) &&
		    (strcmp(endpoint->request_host, "*") == 0 ||
		     pj_stricmp2(&sip_uri->host, endpoint->request_host) == 0))
			return endpoint;
	}
	return NULL;
}

static pj_bool_t filter_initial_invite(pjsip_rx_data *request)
{
	if (request->msg_info.msg->line.req.method.id != PJSIP_INVITE_METHOD ||
	    (request->msg_info.to != NULL && request->msg_info.to->tag.slen != 0))
		return PJ_FALSE;
	if (match_incoming(request) != NULL)
		return PJ_FALSE;
	(void)pjsip_endpt_respond_stateless(pjsua_get_pjsip_endpt(), request,
		PJSIP_SC_NOT_FOUND, NULL, NULL, NULL);
	return PJ_TRUE;
}

static void find_incoming_account(const pjsip_rx_data *request, pjsua_acc_id *account_id)
{
	struct sip_endpoint *endpoint = match_incoming(request);
	*account_id = endpoint != NULL ? endpoint->account_id : PJSUA_INVALID_ID;
}

struct sip_endpoint *sip_server_default_endpoint(struct sip_server *server)
{
	return server != NULL && server->default_enabled ? server->endpoint : NULL;
}

enum sip_signaling_result sip_server_disable_default(struct sip_server *server)
{
	if (server == NULL)
		return SIP_SIGNALING_INVALID_ARGUMENT;
	if (server->opened || callback_active(server))
		return SIP_SIGNALING_BUSY;
	if (!server->default_enabled)
		return SIP_SIGNALING_OK;
	for (unsigned int index = 0; index < server->max_lines; ++index)
		if (server->lines[index] != NULL)
			return SIP_SIGNALING_BUSY;
	free(server->endpoint);
	server->endpoint = NULL;
	server->endpoints[0] = NULL;
	server->default_enabled = false;
	return SIP_SIGNALING_OK;
}

struct sip_endpoint *sip_endpoint_create(struct sip_server *server,
	const struct app_config *config, const char *request_user, const char *request_host,
	bool (*accept_line)(void *, void *), void *context)
{
	if (server == NULL || config == NULL || callback_active(server) ||
	    request_user == NULL || request_host == NULL || request_user[0] == '\0' ||
	    request_host[0] == '\0' || strnlen(request_user, APP_CONFIG_TEXT_MAX) >= APP_CONFIG_TEXT_MAX ||
	    strnlen(request_host, APP_CONFIG_TEXT_MAX) >= APP_CONFIG_TEXT_MAX || server->next_endpoint_id == UINT_MAX ||
	    validate_config(config) != SIP_SIGNALING_OK)
		return NULL;
	unsigned int slot = 0;
	while (slot < PJSUA_MAX_ACC && server->endpoints[slot] != NULL)
		++slot;
	if (slot == PJSUA_MAX_ACC)
		return NULL;
	struct sip_endpoint *endpoint = calloc(1, sizeof(*endpoint));
	if (endpoint == NULL)
		return NULL;
	endpoint->server = server;
	endpoint->config = *config;
	endpoint->account_id = PJSUA_INVALID_ID;
	endpoint->id = ++server->next_endpoint_id;
	endpoint->accept_line = accept_line;
	endpoint->admission_context = context;
	strcpy(endpoint->request_user, request_user);
	strcpy(endpoint->request_host, request_host);
	endpoint->registration_state = config->sip.registration_enabled ?
		SIP_REGISTRATION_UNREGISTERED : SIP_REGISTRATION_DISABLED;
	if (resolve_account(endpoint) != SIP_SIGNALING_OK) {
		free(endpoint);
		return NULL;
	}
	server->endpoints[slot] = endpoint;
	return endpoint;
}

enum sip_signaling_result sip_endpoint_open(struct sip_endpoint *endpoint)
{
	if (endpoint == NULL)
		return SIP_SIGNALING_INVALID_ARGUMENT;
	struct sip_server *server = endpoint->server;
	SDK_LOG_SCOPE(server);
	if (!server->opened)
		return SIP_SIGNALING_NOT_OPEN;
	if (endpoint->opened)
		return SIP_SIGNALING_OK;
	if (endpoint->account_id != PJSUA_INVALID_ID || endpoint->transport != NULL) {
		enum sip_signaling_result cleaned = sip_endpoint_close(endpoint);
		if (cleaned != SIP_SIGNALING_OK)
			return cleaned;
	}
	if (callback_active(server))
		return SIP_SIGNALING_BUSY;
	enum sip_signaling_result result = resolve_account(endpoint);
	if (result != SIP_SIGNALING_OK)
		return result;
	/* Overlapping policies on a shared transport cannot silently select the
	 * first account. Require an unambiguous route before accepting calls. */
	for (unsigned int index = 0; index < PJSUA_MAX_ACC; ++index) {
		struct sip_endpoint *other = server->endpoints[index];
		if (other == NULL || other == endpoint || !other->opened ||
		    other->config.sip.listen_port != endpoint->config.sip.listen_port ||
		    strcmp(other->config.sip.bound_address, endpoint->config.sip.bound_address) != 0 ||
		    strcmp(other->config.sip.public_address, endpoint->config.sip.public_address) != 0)
			continue;
		bool user_overlap = strcmp(other->request_user, "*") == 0 ||
			strcmp(endpoint->request_user, "*") == 0 ||
			strcmp(other->request_user, endpoint->request_user) == 0;
		bool host_overlap = strcmp(other->request_host, "*") == 0 ||
			strcmp(endpoint->request_host, "*") == 0 ||
			strcasecmp(other->request_host, endpoint->request_host) == 0;
		if (user_overlap && host_overlap)
			return SIP_SIGNALING_BUSY;
	}
	result = create_transport(endpoint);
	if (result != SIP_SIGNALING_OK) {
		(void)sip_endpoint_close(endpoint);
		return result;
	}
	endpoint->starting = true;
	result = add_account(endpoint);
	endpoint->starting = false;
	if (result != SIP_SIGNALING_OK) {
		(void)sip_endpoint_close(endpoint);
		return result;
	}
	endpoint->opened = true;
	endpoint->state_sequence++;
	return SIP_SIGNALING_OK;
}

enum sip_signaling_result sip_endpoint_close(struct sip_endpoint *endpoint)
{
	if (endpoint == NULL)
		return SIP_SIGNALING_INVALID_ARGUMENT;
	struct sip_server *server = endpoint->server;
	SDK_LOG_SCOPE(server);
	if (callback_active(server))
		return SIP_SIGNALING_BUSY;
	if (endpoint->opened)
		endpoint->state_sequence++;
	endpoint->opened = false;
	enum sip_signaling_result result = SIP_SIGNALING_OK;
	for (unsigned int index = 0; index < server->max_lines; ++index) {
		struct sip_signaling *line = server->lines[index];
		if (line != NULL && line->call_endpoint == endpoint && line->call_id != PJSUA_INVALID_ID) {
			enum sip_signaling_result stopped = sip_signaling_hangup(line);
			if (stopped != SIP_SIGNALING_OK && stopped != SIP_SIGNALING_NO_CALL)
				result = stopped;
		}
	}
	bool registered = endpoint->config.sip.registration_enabled && endpoint->account_id != PJSUA_INVALID_ID;
	if (registered)
		emit_registration_event(endpoint, SIP_REGISTRATION_UNREGISTERING, 0, PJ_SUCCESS, 0, NULL);
	pj_status_t status = PJ_SUCCESS;
	if (endpoint->account_id != PJSUA_INVALID_ID) {
		(void)pjsua_acc_set_user_data(endpoint->account_id, NULL);
		status = pjsua_acc_del(endpoint->account_id);
		if (status == PJ_SUCCESS || !pjsua_acc_is_valid(endpoint->account_id))
			endpoint->account_id = PJSUA_INVALID_ID;
	}
	if (endpoint->transport != NULL) {
		struct sip_transport *transport = endpoint->transport;
		if (transport->references == 1) {
			pj_status_t closed = pjsua_transport_close(transport->id, PJ_FALSE);
			if (status == PJ_SUCCESS)
				status = closed;
			if (closed == PJ_SUCCESS) {
				transport->references = 0;
				transport->id = PJSUA_INVALID_ID;
				endpoint->transport = NULL;
			}
		} else {
			if (transport->references != 0)
				transport->references--;
			endpoint->transport = NULL;
		}
	}
	endpoint->local_port = 0;
	endpoint->registration_may_exist = false;
	if (registered)
		emit_registration_event(endpoint, SIP_REGISTRATION_UNREGISTERED, 0, status, 0, NULL);
	if (status != PJ_SUCCESS) {
		emit_library_error(server->administrative, "close SIP endpoint", status);
		return SIP_SIGNALING_LIBRARY_ERROR;
	}
	return result;
}

void sip_endpoint_destroy(struct sip_endpoint *endpoint)
{
	if (endpoint == NULL || endpoint == endpoint->server->endpoint ||
	    callback_active(endpoint->server))
		return;
	struct sip_server *server = endpoint->server;
	(void)sip_endpoint_close(endpoint);
	for (unsigned int index = 0; index < server->max_lines; ++index) {
		struct sip_signaling *line = server->lines[index];
		if (line == NULL)
			continue;
		if (line->endpoint == endpoint)
			line->endpoint = server->endpoint;
		if (line->call_endpoint == endpoint)
			line->call_endpoint = NULL;
	}
	for (unsigned int index = 0; index < PJSUA_MAX_ACC; ++index)
		if (server->endpoints[index] == endpoint)
			server->endpoints[index] = NULL;
	free(endpoint);
}

unsigned int sip_endpoint_id(const struct sip_endpoint *endpoint)
{
	return endpoint != NULL ? endpoint->id : 0;
}

unsigned int sip_endpoint_local_port(const struct sip_endpoint *endpoint)
{
	return endpoint != NULL && endpoint->opened ? endpoint->local_port : 0;
}

void sip_endpoint_get_status(const struct sip_endpoint *endpoint, struct sip_endpoint_status *status)
{
	if (endpoint == NULL || status == NULL)
		return;
	*status = (struct sip_endpoint_status){
		.id = endpoint->id, .local_port = sip_endpoint_local_port(endpoint),
		.sequence = endpoint->state_sequence, .opened = endpoint->opened,
		.registration_state = endpoint->registration_state,
		.registration_code = endpoint->registration_code,
		.library_status = endpoint->registration_library_status,
	};
}

enum sip_signaling_result sip_signaling_set_endpoint(struct sip_signaling *line,
	struct sip_endpoint *endpoint)
{
	if (line == NULL || endpoint == NULL || line->server != endpoint->server)
		return SIP_SIGNALING_INVALID_ARGUMENT;
	if (line->call_id != PJSUA_INVALID_ID || line->call_starting)
		return SIP_SIGNALING_CALL_ACTIVE;
	if (!endpoint->opened)
		return SIP_SIGNALING_NOT_OPEN;
	line->endpoint = endpoint;
	return SIP_SIGNALING_OK;
}

static enum sip_signaling_result close_now(struct sip_signaling *signaling)
{
	SDK_LOG_SCOPE(signaling != NULL ? signaling->server : NULL);
	if (signaling->closing)
		return SIP_SIGNALING_OK;
	signaling->closing = true;
	enum sip_signaling_result result = SIP_SIGNALING_OK;
	bool was_active = signaling->call_id != PJSUA_INVALID_ID;
	pjsua_call_id call_id = signaling->call_id;
	signaling->opened = false;
	signaling->call_id = PJSUA_INVALID_ID;
	signaling->call_starting = false;
	signaling->incoming_response_code = 0;
	if (was_active && pjsua_owner == signaling->server) {
		if (call_id >= 0 && (unsigned int)call_id < PJSUA_MAX_CALLS)
			signaling->server->retired[call_id] = true;
		(void)pjsua_call_set_user_data(call_id, NULL);
		if (pjsua_call_hangup(call_id, 0, NULL, NULL) != PJ_SUCCESS)
			result = SIP_SIGNALING_LIBRARY_ERROR;
	}
	emit_media_inactive(signaling);
	if (signaling->call_state != SIP_CALL_IDLE)
		emit_call_event(signaling, SIP_CALL_IDLE, NULL);
	signaling->close_requested = false;
	signaling->closing = false;
	if (signaling->owns_server) {
		enum sip_signaling_result closed = sip_server_close(signaling->server);
		if (result == SIP_SIGNALING_OK)
			result = closed;
	}
	return result;
}

static void administrative_error(void *context, const struct sip_signaling_error *error)
{
	struct sip_server *server = context;
	for (unsigned int index = 0; index < server->max_lines; ++index) {
		struct sip_signaling *line = server->lines[index];
		if (line != NULL && line->callbacks.error != NULL) {
			line->last_library_status = error->library_status;
			callback_enter(line);
			line->callbacks.error(line->callbacks.context, error);
			callback_leave(line);
		}
	}
}

static void administrative_trace(void *context, const struct sip_trace_record *record)
{
	struct sip_server *server = context;
	for (unsigned int index = 0; index < server->max_lines; ++index) {
		struct sip_signaling *line = server->lines[index];
		if (line != NULL && line->callbacks.trace != NULL) {
			callback_enter(line);
			line->callbacks.trace(line->callbacks.context, record);
			callback_leave(line);
		}
	}
}

struct sip_server *sip_server_create(const struct app_config *config, unsigned int max_lines)
{
	if (config == NULL || max_lines == 0 || max_lines > 32 || max_lines > PJSUA_MAX_CALLS)
		return NULL;
	struct sip_server *server = calloc(1, sizeof(*server));
	if (server == NULL)
		return NULL;
	server->administrative = calloc(1, sizeof(*server->administrative));
	if (server->administrative == NULL) {
		free(server);
		return NULL;
	}
	server->endpoint = calloc(1, sizeof(*server->endpoint));
	if (server->endpoint == NULL) {
		free(server->administrative);
		free(server);
		return NULL;
	}
	server->endpoint->server = server;
	server->endpoint->config = *config;
	server->max_lines = max_lines;
	server->endpoint->account_id = PJSUA_INVALID_ID;
	server->endpoint->id = ++server->next_endpoint_id;
	server->endpoints[0] = server->endpoint;
	server->default_enabled = true;
	strcpy(server->endpoint->request_user, "*");
	strcpy(server->endpoint->request_host, "*");
	server->administrative->config = *config;
	server->administrative->server = server;
	server->administrative->call_id = PJSUA_INVALID_ID;
	server->administrative->callbacks.context = server;
	server->administrative->callbacks.error = administrative_error;
	server->administrative->callbacks.trace = administrative_trace;
	server->endpoint->registration_state = config->sip.registration_enabled ?
		SIP_REGISTRATION_UNREGISTERED : SIP_REGISTRATION_DISABLED;
	return server;
}

enum sip_signaling_result sip_server_open(struct sip_server *server)
{
	SDK_LOG_SCOPE(server);
	if (server == NULL)
		return SIP_SIGNALING_INVALID_ARGUMENT;
	if (server->opened)
		return SIP_SIGNALING_OK;
	if (pjsua_owner != NULL || pjsua_get_state() != PJSUA_STATE_NULL) {
		emit_library_error(server->administrative, "concurrent signaling owner", PJ_EBUSY);
		return SIP_SIGNALING_BUSY;
	}
	struct sip_signaling *administrative = server->administrative;
	enum sip_signaling_result result = validate_config(&administrative->config);

	if (result != SIP_SIGNALING_OK)
		return result;
	memset(server->retired, 0, sizeof(server->retired));
	pjsua_owner = server;
	result = initialize_library(administrative);
	if (result == SIP_SIGNALING_OK) {
		memset(&server->incoming_filter, 0, sizeof(server->incoming_filter));
		server->incoming_filter.name = pj_str((char *)"mod-line-route");
		server->incoming_filter.id = -1;
		server->incoming_filter.priority = PJSIP_MOD_PRIORITY_APPLICATION - 1;
		server->incoming_filter.on_rx_request = filter_initial_invite;
		pj_status_t filter_status = pjsip_endpt_register_module(pjsua_get_pjsip_endpt(), &server->incoming_filter);
		if (filter_status != PJ_SUCCESS) {
			emit_library_error(administrative, "register incoming call policy", filter_status);
			result = SIP_SIGNALING_LIBRARY_ERROR;
		}
	}
	if (result != SIP_SIGNALING_OK)
		goto fail;
	pj_status_t status = pjsua_start();
	if (status != PJ_SUCCESS) {
		emit_library_error(administrative, "start signaling library", status);
		result = SIP_SIGNALING_LIBRARY_ERROR;
		goto fail;
	}
	server->opened = true;
	if (server->default_enabled) {
		result = sip_endpoint_open(server->endpoint);
		if (result != SIP_SIGNALING_OK)
			goto fail;
	}
	return SIP_SIGNALING_OK;
fail:
	(void)sip_server_close(server);
	return result;
}

enum sip_signaling_result sip_server_close(struct sip_server *server)
{
	SDK_LOG_SCOPE(server);
	if (server == NULL)
		return SIP_SIGNALING_INVALID_ARGUMENT;
	if (server->closing)
		return SIP_SIGNALING_OK;
	if (callback_active(server))
		return SIP_SIGNALING_BUSY;
	server->closing = true;
	/* Stop admission before any hangup callbacks; per-line close clears its
	 * userdata before releasing the line. No network wait belongs in stop. */
	server->opened = false;
	enum sip_signaling_result result = SIP_SIGNALING_OK;
	for (unsigned int index = 0; index < server->max_lines; ++index)
		if (server->lines[index] != NULL) {
			enum sip_signaling_result closed = close_now(server->lines[index]);
			if (result == SIP_SIGNALING_OK)
				result = closed;
		}
	pj_status_t status = PJ_SUCCESS;
	if (server->library_created) {
		for (unsigned int index = 0; index < PJSUA_MAX_ACC; ++index)
			if (server->endpoints[index] != NULL) {
				enum sip_signaling_result stopped = sip_endpoint_close(server->endpoints[index]);
				if (result == SIP_SIGNALING_OK)
					result = stopped;
			}
		status = pjsua_destroy2(PJSUA_DESTROY_NO_NETWORK);
	}
	server->library_created = false;
	for (unsigned int index = 0; index < PJSUA_MAX_ACC; ++index)
		if (server->endpoints[index] != NULL) {
			server->endpoints[index]->account_id = PJSUA_INVALID_ID;
			server->endpoints[index]->transport = NULL;
			server->endpoints[index]->local_port = 0;
		}
	memset(server->transports, 0, sizeof(server->transports));
	server->closing = false;
	if (pjsua_owner == server)
		pjsua_owner = NULL;
	if (status != PJ_SUCCESS) {
		emit_library_error(server->administrative, "close signaling library", status);
		return SIP_SIGNALING_LIBRARY_ERROR;
	}
	return result;
}

void sip_server_destroy(struct sip_server *server)
{
	if (server == NULL || callback_active(server))
		return;
	for (unsigned int index = 0; index < server->max_lines; ++index)
		if (server->lines[index] != NULL)
			return;
	(void)sip_server_close(server);
	for (unsigned int index = 0; index < PJSUA_MAX_ACC; ++index)
		free(server->endpoints[index]);
	free(server->administrative);
	free(server);
}

enum sip_signaling_result sip_server_step(struct sip_server *server, unsigned int timeout_ms)
{
	SDK_LOG_SCOPE(server);
	if (server == NULL)
		return SIP_SIGNALING_INVALID_ARGUMENT;
	if (!server->opened)
		return SIP_SIGNALING_NOT_OPEN;
	int events = pjsua_handle_events(timeout_ms);
	if (events < 0) {
		emit_library_error(server->administrative, "poll SIP events", (pj_status_t)-events);
		return SIP_SIGNALING_LIBRARY_ERROR;
	}
	for (unsigned int index = 0; index < server->max_lines; ++index) {
		struct sip_signaling *line = server->lines[index];
		if (line != NULL && line->close_requested)
			(void)close_now(line);
	}
	return SIP_SIGNALING_OK;
}

unsigned int sip_server_local_port(const struct sip_server *server)
{
	return server != NULL && server->default_enabled ? server->endpoint->local_port : 0;
}

struct sip_signaling *sip_signaling_create_shared(struct sip_server *server,
	const struct app_config *config, const struct sip_signaling_callbacks *callbacks)
{
	if (server == NULL || config == NULL)
		return NULL;
	unsigned int slot = 0;
	while (slot < server->max_lines && server->lines[slot] != NULL)
		++slot;
	if (slot == server->max_lines)
		return NULL;
	struct sip_signaling *signaling = calloc(1, sizeof(*signaling));
	if (signaling == NULL)
		return NULL;
	signaling->server = server;
	signaling->config = *config;
	signaling->endpoint = server->endpoint;
	if (server->endpoint != NULL)
		signaling->config.sip = server->endpoint->config.sip;
	if (callbacks != NULL)
		signaling->callbacks = *callbacks;
	signaling->call_id = PJSUA_INVALID_ID;
	server->lines[slot] = signaling;
	return signaling;
}

struct sip_signaling *sip_signaling_create(const struct app_config *config,
	const struct sip_signaling_callbacks *callbacks)
{
	struct sip_server *server = sip_server_create(config, 1);
	if (server == NULL)
		return NULL;
	struct sip_signaling *signaling = sip_signaling_create_shared(server, config, callbacks);
	if (signaling == NULL) {
		sip_server_destroy(server);
		return NULL;
	}
	signaling->owns_server = true;
	return signaling;
}

void sip_signaling_destroy(struct sip_signaling *signaling)
{
	if (signaling == NULL)
		return;
	if (signaling->callback_depth != 0) {
		signaling->close_requested = true;
		return;
	}
	(void)close_now(signaling);
	struct sip_server *server = signaling->server;
	bool owned = signaling->owns_server;
	for (unsigned int index = 0; index < server->max_lines; ++index)
		if (server->lines[index] == signaling)
			server->lines[index] = NULL;
	free(signaling);
	if (owned)
		sip_server_destroy(server);
}

enum sip_signaling_result sip_signaling_open(struct sip_signaling *signaling)
{
	if (signaling == NULL)
		return SIP_SIGNALING_INVALID_ARGUMENT;
	if (signaling->opened)
		return SIP_SIGNALING_OK;
	enum sip_signaling_result result = validate_config(&signaling->config);
	if (result == SIP_SIGNALING_OK)
		result = sip_server_open(signaling->server);
	if (result != SIP_SIGNALING_OK)
		return result;
	if (signaling->close_requested)
		return close_now(signaling);
	signaling->opened = true;
	signaling->last_library_status = PJ_SUCCESS;
	emit_trace(signaling, SIP_TRACE_ADAPTER, 3,
		"SIP signaling ready\n", sizeof("SIP signaling ready\n") - 1);
	if (signaling->callbacks.ready != NULL && !signaling->close_requested) {
		callback_enter(signaling);
		signaling->callbacks.ready(signaling->callbacks.context);
		callback_leave(signaling);
	}
	return signaling->close_requested ? close_now(signaling) : SIP_SIGNALING_OK;
}

enum sip_signaling_result sip_signaling_close(struct sip_signaling *signaling)
{
	if (signaling == NULL)
		return SIP_SIGNALING_INVALID_ARGUMENT;
	if (signaling->callback_depth != 0) {
		signaling->close_requested = true;
		return SIP_SIGNALING_OK;
	}
	return close_now(signaling);
}

enum sip_signaling_result sip_signaling_step(struct sip_signaling *signaling, unsigned int timeout_ms)
{
	return signaling == NULL ? SIP_SIGNALING_INVALID_ARGUMENT :
		!signaling->opened ? SIP_SIGNALING_NOT_OPEN :
		sip_server_step(signaling->server, timeout_ms);
}

enum sip_signaling_result sip_signaling_dial(
	struct sip_signaling *signaling, const char *target)
{
	SDK_LOG_SCOPE(signaling != NULL ? signaling->server : NULL);
	enum sip_uri_result uri_result;
	pjsua_call_info info;
	pjsua_call_id call_id = PJSUA_INVALID_ID;
	pj_str_t target_uri;
	pj_status_t status;

	if (signaling == NULL)
		return SIP_SIGNALING_INVALID_ARGUMENT;
	if (!signaling->opened)
		return SIP_SIGNALING_NOT_OPEN;
	if (signaling->call_id != PJSUA_INVALID_ID || signaling->call_starting)
		return SIP_SIGNALING_CALL_ACTIVE;
	if (signaling->endpoint == NULL || !signaling->endpoint->opened)
		return SIP_SIGNALING_NOT_OPEN;
	uri_result = sip_uri_format_target(
		signaling->target_uri, sizeof(signaling->target_uri), target,
		signaling->config.sip.target, signaling->endpoint->account_domain);
	if (uri_result != SIP_URI_OK)
		return SIP_SIGNALING_INVALID_ARGUMENT;

	target_uri = pj_str(signaling->target_uri);
	signaling->call_direction = SIP_CALL_DIRECTION_OUTGOING;
	signaling->call_generation++;
	signaling->called_number[0] = '\0';
	signaling->caller_number[0] = '\0';
	signaling->call_starting = true;
	signaling->call_endpoint = signaling->endpoint;
	status = pjsua_call_make_call(signaling->call_endpoint->account_id, &target_uri, NULL,
				      signaling, NULL, &call_id);
	if (status != PJ_SUCCESS) {
		signaling->call_starting = false;
		signaling->call_id = PJSUA_INVALID_ID;
		if (signaling->call_state != SIP_CALL_IDLE)
			emit_call_event(signaling, SIP_CALL_IDLE,
					NULL);
		emit_library_error(signaling, "place SIP call", status);
		if (signaling->close_requested)
			(void)close_now(signaling);
		return SIP_SIGNALING_LIBRARY_ERROR;
	}
	if (signaling->call_starting) {
		signaling->call_starting = false;
		if (signaling->call_id == PJSUA_INVALID_ID)
			signaling->call_id = call_id;
		if (signaling->call_state == SIP_CALL_IDLE) {
			if (pjsua_call_get_info(call_id, &info) == PJ_SUCCESS)
				emit_call_event(signaling,
						SIP_CALL_DIALING,
						&info);
			else
				emit_call_event(signaling,
						SIP_CALL_DIALING,
						NULL);
		}
	}
	if (signaling->close_requested)
		return close_now(signaling);
	return SIP_SIGNALING_OK;
}

enum sip_signaling_result sip_signaling_answer(
	struct sip_signaling *signaling)
{
	SDK_LOG_SCOPE(signaling != NULL ? signaling->server : NULL);
	pjsua_call_id call_id;
	pj_status_t status;
	unsigned int previous_response_code;

	if (signaling == NULL)
		return SIP_SIGNALING_INVALID_ARGUMENT;
	if (!signaling->opened)
		return SIP_SIGNALING_NOT_OPEN;
	call_id = signaling->call_id;
	if (call_id == PJSUA_INVALID_ID ||
	    signaling->call_direction != SIP_CALL_DIRECTION_INCOMING)
		return SIP_SIGNALING_NO_CALL;
	if (signaling->call_state == SIP_CALL_CONNECTED ||
	    signaling->incoming_response_code == 200)
		return SIP_SIGNALING_OK;
	if (signaling->incoming_response_code >= 300)
		return SIP_SIGNALING_NO_CALL;
	previous_response_code = signaling->incoming_response_code;
	signaling->incoming_response_code = 200;
	status = pjsua_call_answer(call_id, 200, NULL, NULL);
	if (status != PJ_SUCCESS) {
		signaling->incoming_response_code = previous_response_code;
		emit_library_error(signaling, "answer incoming SIP call", status);
		if (signaling->close_requested)
			(void)close_now(signaling);
		return SIP_SIGNALING_LIBRARY_ERROR;
	}
	if (signaling->close_requested)
		return close_now(signaling);
	return SIP_SIGNALING_OK;
}

enum sip_signaling_result sip_signaling_hangup(
	struct sip_signaling *signaling)
{
	SDK_LOG_SCOPE(signaling != NULL ? signaling->server : NULL);
	pjsua_call_id call_id;
	pj_status_t status;

	if (signaling == NULL)
		return SIP_SIGNALING_INVALID_ARGUMENT;
	if (!signaling->opened)
		return SIP_SIGNALING_NOT_OPEN;
	call_id = signaling->call_id;
	if (call_id == PJSUA_INVALID_ID)
		return SIP_SIGNALING_NO_CALL;
	if (signaling->call_direction ==
	    SIP_CALL_DIRECTION_INCOMING &&
	    signaling->call_state != SIP_CALL_CONNECTED)
		signaling->incoming_response_code = 603;
	signaling->call_id = PJSUA_INVALID_ID;
	signaling->call_starting = false;
	if (call_id >= 0 && (unsigned int)call_id < PJSUA_MAX_CALLS)
		signaling->server->retired[call_id] = true;
	(void)pjsua_call_set_user_data(call_id, NULL);
	status = pjsua_call_hangup(call_id, 0, NULL, NULL);
	emit_media_inactive(signaling);
	if (signaling->call_state != SIP_CALL_IDLE)
		emit_call_event(signaling, SIP_CALL_IDLE, NULL);
	if (status != PJ_SUCCESS) {
		emit_library_error(signaling, "hang up SIP call", status);
		if (signaling->close_requested)
			(void)close_now(signaling);
		return SIP_SIGNALING_LIBRARY_ERROR;
	}
	if (signaling->close_requested)
		return close_now(signaling);
	return SIP_SIGNALING_OK;
}

bool sip_signaling_is_open(
	const struct sip_signaling *signaling)
{
	return signaling != NULL && signaling->opened;
}

unsigned int sip_signaling_local_port(
	const struct sip_signaling *signaling)
{
	return signaling != NULL && signaling->opened && signaling->endpoint != NULL ? signaling->endpoint->local_port : 0;
}

const char *sip_signaling_identity_uri(
	const struct sip_signaling *signaling)
{
	return signaling != NULL && signaling->endpoint != NULL && signaling->endpoint->account_configured
		       ? signaling->endpoint->identity_uri
		       : NULL;
}

enum sip_registration_state
sip_signaling_registration_state(
	const struct sip_signaling *signaling)
{
	return signaling != NULL && signaling->endpoint != NULL ? signaling->endpoint->registration_state
				 : SIP_REGISTRATION_DISABLED;
}

enum sip_call_state sip_signaling_call_state(
	const struct sip_signaling *signaling)
{
	return signaling != NULL ? signaling->call_state
				 : SIP_CALL_IDLE;
}

enum sip_media_state sip_signaling_media_state(
	const struct sip_signaling *signaling)
{
	return signaling != NULL ? signaling->media_state :
				  SIP_MEDIA_INACTIVE;
}

int sip_signaling_last_library_status(
	const struct sip_signaling *signaling)
{
	return signaling != NULL ? signaling->last_library_status : 0;
}

const char *sip_signaling_result_name(
	enum sip_signaling_result result)
{
	switch (result) {
	case SIP_SIGNALING_OK:
		return "ok";
	case SIP_SIGNALING_INVALID_ARGUMENT:
		return "invalid argument";
	case SIP_SIGNALING_NOT_OPEN:
		return "signaling is not open";
	case SIP_SIGNALING_BUSY:
		return "another signaling instance is open";
	case SIP_SIGNALING_CALL_ACTIVE:
		return "a SIP call is already active";
	case SIP_SIGNALING_NO_CALL:
		return "no SIP call is active";
	case SIP_SIGNALING_LIBRARY_ERROR:
		return "signaling library error";
	}
	return "unknown signaling result";
}

const char *sip_registration_state_name(
	enum sip_registration_state state)
{
	switch (state) {
	case SIP_REGISTRATION_DISABLED:
		return "disabled";
	case SIP_REGISTRATION_REGISTERING:
		return "registering";
	case SIP_REGISTRATION_REGISTERED:
		return "registered";
	case SIP_REGISTRATION_UNREGISTERING:
		return "unregistering";
	case SIP_REGISTRATION_UNREGISTERED:
		return "unregistered";
	case SIP_REGISTRATION_FAILED:
		return "registration failed";
	case SIP_REGISTRATION_UNREGISTER_FAILED:
		return "unregistration failed";
	}
	return "unknown registration state";
}

const char *sip_call_state_name(
	enum sip_call_state state)
{
	switch (state) {
	case SIP_CALL_IDLE:
		return "idle";
	case SIP_CALL_DIALING:
		return "dialing";
	case SIP_CALL_INCOMING_PENDING:
		return "incoming";
	case SIP_CALL_EARLY:
		return "early";
	case SIP_CALL_CONNECTED:
		return "connected";
	}
	return "unknown call state";
}

const char *sip_media_state_name(
	enum sip_media_state state)
{
	switch (state) {
	case SIP_MEDIA_INACTIVE:
		return "inactive";
	case SIP_MEDIA_CONFIGURED:
		return "configured";
	}
	return "unknown media state";
}
