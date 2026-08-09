// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_sip_signaling.h"
#include "sippymodem_sip_uri.h"
#include "sippymodem_sdp.h"

#include <pjsua-lib/pjsua.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
	SIPPYMODEM_SIP_ACCOUNT_URI_MAX = SIPPYMODEM_CONFIG_TEXT_MAX * 2 + 16,
	SIPPYMODEM_SIP_CALL_IDENTITY_MAX = 256,
	SIPPYMODEM_SIP_CALL_REASON_MAX = 128,
	SIPPYMODEM_SIP_REGISTRATION_REASON_MAX = 128,
	SIPPYMODEM_SIP_UNREGISTER_TIMEOUT_MS = 1000,
};

struct sippymodem_sip_signaling {
	struct sippymodem_config config;
	struct sippymodem_sip_signaling_callbacks callbacks;
	pjsua_transport_id transport_id;
	pjsua_acc_id account_id;
	pjsua_call_id call_id;
	int last_library_status;
	unsigned int local_port;
	unsigned int callback_depth;
	char login_buffer[SIPPYMODEM_CONFIG_TEXT_MAX];
	char identity_uri[SIPPYMODEM_SIP_ACCOUNT_URI_MAX];
	char registrar_uri[SIPPYMODEM_SIP_ACCOUNT_URI_MAX];
	char proxy_uri[SIPPYMODEM_SIP_ACCOUNT_URI_MAX];
	char contact_uri[SIPPYMODEM_SIP_ACCOUNT_URI_MAX];
	char target_uri[SIPPYMODEM_SIP_ACCOUNT_URI_MAX];
	char call_remote_identity[SIPPYMODEM_SIP_CALL_IDENTITY_MAX];
	char call_reason[SIPPYMODEM_SIP_CALL_REASON_MAX];
	char registration_reason[SIPPYMODEM_SIP_REGISTRATION_REASON_MAX];
	const char *account_user;
	const char *account_password;
	const char *account_domain;
	enum sippymodem_sip_call_direction call_direction;
	enum sippymodem_sip_call_state call_state;
	enum sippymodem_sip_registration_state registration_state;
	unsigned int incoming_response_code;
	bool call_starting;
	bool registration_may_exist;
	bool account_configured;
	bool library_created;
	bool opened;
	bool closing;
	bool close_requested;
};

static struct sippymodem_sip_signaling *active_signaling;

static void callback_enter(struct sippymodem_sip_signaling *signaling)
{
	signaling->callback_depth++;
}

static void callback_leave(struct sippymodem_sip_signaling *signaling)
{
	if (signaling->callback_depth != 0)
		signaling->callback_depth--;
}

static void emit_library_error(struct sippymodem_sip_signaling *signaling,
			       const char *operation, pj_status_t status)
{
	char message[PJ_ERR_MSG_SIZE];
	struct sippymodem_sip_signaling_error error;

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
	struct sippymodem_sip_signaling *signaling,
	enum sippymodem_sip_trace_origin origin, int level,
	const char *data, size_t size)
{
	struct sippymodem_sip_trace_record record;

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
		emit_trace(active_signaling, SIPPYMODEM_SIP_TRACE_LIBRARY, level,
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

static void emit_call_event(struct sippymodem_sip_signaling *signaling,
			    enum sippymodem_sip_call_state state,
			    const pjsua_call_info *info)
{
	struct sippymodem_sip_call_event event;

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

static void pjsip_call_changed(pjsua_call_id call_id, pjsip_event *event)
{
	struct sippymodem_sip_signaling *signaling = active_signaling;
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
		emit_call_event(signaling, SIPPYMODEM_SIP_CALL_DIALING, &info);
		break;
	case PJSIP_INV_STATE_EARLY:
		emit_call_event(signaling, SIPPYMODEM_SIP_CALL_EARLY, &info);
		break;
	case PJSIP_INV_STATE_CONFIRMED:
		emit_call_event(signaling, SIPPYMODEM_SIP_CALL_CONNECTED, &info);
		break;
	case PJSIP_INV_STATE_DISCONNECTED:
		signaling->call_id = PJSUA_INVALID_ID;
		signaling->call_starting = false;
		signaling->incoming_response_code = 0;
		emit_call_event(signaling, SIPPYMODEM_SIP_CALL_IDLE, &info);
		break;
	case PJSIP_INV_STATE_NULL:
	case PJSIP_INV_STATE_INCOMING:
	case PJSIP_INV_STATE_CONNECTING:
		break;
	}
}

static void pjsip_incoming_call(pjsua_acc_id account_id,
				pjsua_call_id call_id,
				pjsip_rx_data *request)
{
	struct sippymodem_sip_signaling *signaling = active_signaling;
	pjsua_call_info info;
	pj_status_t status;
	unsigned int response_code;

	(void)account_id;
	(void)request;
	if (signaling == NULL)
		return;
	if (signaling->closing || signaling->close_requested ||
	    signaling->call_id != PJSUA_INVALID_ID || signaling->call_starting) {
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
	signaling->call_direction = SIPPYMODEM_SIP_CALL_DIRECTION_INCOMING;
	signaling->incoming_response_code = 0;
	emit_call_event(signaling, SIPPYMODEM_SIP_CALL_INCOMING_PENDING, &info);
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
	struct sippymodem_sip_signaling *signaling,
	enum sippymodem_sip_registration_state state,
	unsigned int status_code, pj_status_t library_status,
	unsigned int expires_seconds, const pj_str_t *reason)
{
	struct sippymodem_sip_registration_event event;
	if (signaling == NULL)
		return;
	copy_pj_text(signaling->registration_reason,
		     sizeof(signaling->registration_reason), reason);
	signaling->registration_state = state;
	if (state == SIPPYMODEM_SIP_REGISTRATION_REGISTERED)
		signaling->registration_may_exist = true;
	else if (state == SIPPYMODEM_SIP_REGISTRATION_UNREGISTERED)
		signaling->registration_may_exist = false;
	if (signaling->callbacks.registration_changed == NULL)
		return;
	event.state = state;
	event.status_code = status_code;
	event.library_status = (int)library_status;
	event.expires_seconds = expires_seconds;
	event.reason = signaling->registration_reason;
	callback_enter(signaling);
	signaling->callbacks.registration_changed(
		signaling->callbacks.context, &event);
	callback_leave(signaling);
}

static void pjsip_registration_started(pjsua_acc_id account_id,
				       pj_bool_t renew)
{
	(void)account_id;
	emit_registration_event(
		active_signaling,
		renew ? SIPPYMODEM_SIP_REGISTRATION_REGISTERING
		      : SIPPYMODEM_SIP_REGISTRATION_UNREGISTERING,
		0, PJ_SUCCESS, 0, NULL);
}

static void pjsip_registration_changed(pjsua_acc_id account_id,
				       pjsua_reg_info *info)
{
	const struct pjsip_regc_cbparam *result;
	enum sippymodem_sip_registration_state state;
	unsigned int status_code;
	unsigned int expires_seconds;
	bool unregistering;
	bool succeeded;

	(void)account_id;
	if (active_signaling == NULL || info == NULL || info->cbparam == NULL)
		return;
	result = info->cbparam;
	unregistering = !info->renew || result->is_unreg;
	succeeded = result->status == PJ_SUCCESS && result->code >= 200 &&
		    result->code < 300;
	if (succeeded)
		state = unregistering
				? SIPPYMODEM_SIP_REGISTRATION_UNREGISTERED
				: SIPPYMODEM_SIP_REGISTRATION_REGISTERED;
	else
		state = unregistering
				? SIPPYMODEM_SIP_REGISTRATION_UNREGISTER_FAILED
				: SIPPYMODEM_SIP_REGISTRATION_FAILED;
	status_code = result->code > 0 ? (unsigned int)result->code : 0;
	expires_seconds =
		result->expiration == PJSIP_REGC_EXPIRATION_NOT_SPECIFIED
			? 0
			: result->expiration;
	emit_registration_event(active_signaling, state, status_code,
				result->status, expires_seconds, &result->reason);
}

static bool text_is_terminated(const char *text, size_t capacity)
{
	return memchr(text, '\0', capacity) != NULL;
}

static bool account_text_is_terminated(const struct sippymodem_sip_config *sip)
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

static enum sippymodem_sip_signaling_result validate_config(
	const struct sippymodem_config *config)
{
	if (config->sip.listen_port > 65535 ||
	    config->media.frame_duration_ms == 0 ||
	    config->media.frame_duration_ms >
		    SIPPYMODEM_SDP_MAX_PACKET_DURATION_MS ||
	    config->media.jitter_buffer_ms > INT_MAX ||
	    sippymodem_codec_name(config->media.codec) == NULL ||
	    !account_text_is_terminated(&config->sip) ||
	    !text_is_terminated(config->sip.public_address,
				sizeof(config->sip.public_address)) ||
	    !text_is_terminated(config->sip.bound_address,
				sizeof(config->sip.bound_address)))
		return SIPPYMODEM_SIP_SIGNALING_INVALID_ARGUMENT;
	return SIPPYMODEM_SIP_SIGNALING_OK;
}

static bool format_account_uri(char *output, size_t capacity,
			       const char *value, const char *domain)
{
	return sippymodem_sip_uri_format_target(
		       output, capacity, value, NULL, domain) ==
	       SIPPYMODEM_SIP_URI_OK;
}

static enum sippymodem_sip_signaling_result resolve_account(
	struct sippymodem_sip_signaling *signaling)
{
	const struct sippymodem_sip_config *sip = &signaling->config.sip;
	const char *user = sip->user;
	const char *password = sip->password;
	const char *domain = sip->domain[0] != '\0' ? sip->domain : sip->server;
	bool any_authentication;
	bool all_credentials;
	int length;

	signaling->account_configured = false;
	signaling->identity_uri[0] = '\0';
	signaling->registrar_uri[0] = '\0';
	signaling->proxy_uri[0] = '\0';
	signaling->contact_uri[0] = '\0';
	signaling->target_uri[0] = '\0';
	signaling->account_user = NULL;
	signaling->account_password = NULL;
	signaling->account_domain = NULL;
	if (sip->login[0] != '\0') {
		char *password_separator;
		char *domain_separator;

		memcpy(signaling->login_buffer, sip->login, strlen(sip->login) + 1);
		password_separator = strchr(signaling->login_buffer, ':');
		domain_separator = strrchr(signaling->login_buffer, '@');
		if (password_separator == NULL || domain_separator == NULL ||
		    password_separator == signaling->login_buffer ||
		    domain_separator <= password_separator + 1 ||
		    domain_separator[1] == '\0')
			return SIPPYMODEM_SIP_SIGNALING_INVALID_ARGUMENT;
		*password_separator++ = '\0';
		*domain_separator++ = '\0';
		user = signaling->login_buffer;
		password = password_separator;
		domain = domain_separator;
	}

	any_authentication = user[0] != '\0' || password[0] != '\0';
	all_credentials = user[0] != '\0' && password[0] != '\0' &&
			  domain[0] != '\0';
	if (!all_credentials) {
		if (any_authentication || sip->registration_enabled)
			return SIPPYMODEM_SIP_SIGNALING_INVALID_ARGUMENT;
		signaling->account_domain =
			domain[0] != '\0' ? domain : NULL;
		return SIPPYMODEM_SIP_SIGNALING_OK;
	}
	if (strchr(user, '@') != NULL || strchr(user, ':') != NULL ||
	    strchr(domain, '@') != NULL)
		return SIPPYMODEM_SIP_SIGNALING_INVALID_ARGUMENT;

	if (!format_account_uri(signaling->identity_uri,
				sizeof(signaling->identity_uri), user, domain))
		return SIPPYMODEM_SIP_SIGNALING_INVALID_ARGUMENT;
	if (sip->identity_uri[0] != '\0' &&
	    !format_account_uri(signaling->identity_uri,
				sizeof(signaling->identity_uri),
				sip->identity_uri, domain))
		return SIPPYMODEM_SIP_SIGNALING_INVALID_ARGUMENT;
	if (sip->registrar_uri[0] != '\0') {
		if (!format_account_uri(signaling->registrar_uri,
					 sizeof(signaling->registrar_uri),
					 sip->registrar_uri, NULL))
			return SIPPYMODEM_SIP_SIGNALING_INVALID_ARGUMENT;
	} else {
		length = snprintf(signaling->registrar_uri,
				  sizeof(signaling->registrar_uri), "sip:%s", domain);
		if (length < 0 ||
		    (size_t)length >= sizeof(signaling->registrar_uri))
			return SIPPYMODEM_SIP_SIGNALING_INVALID_ARGUMENT;
	}
	if (sip->proxy_uri[0] != '\0' &&
	    !format_account_uri(signaling->proxy_uri,
				sizeof(signaling->proxy_uri), sip->proxy_uri, NULL))
		return SIPPYMODEM_SIP_SIGNALING_INVALID_ARGUMENT;

	signaling->account_user = user;
	signaling->account_password = password;
	signaling->account_domain = domain;
	signaling->account_configured = true;
	return SIPPYMODEM_SIP_SIGNALING_OK;
}

static void configure_media(const struct sippymodem_config *config,
			    pjsua_media_config *media)
{
	int jitter_buffer_ms = (int)config->media.jitter_buffer_ms;

	pjsua_media_config_default(media);
	media->clock_rate = SIPPYMODEM_MEDIA_SAMPLE_RATE;
	media->snd_clock_rate = SIPPYMODEM_MEDIA_SAMPLE_RATE;
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

static enum sippymodem_sip_signaling_result initialize_library(
	struct sippymodem_sip_signaling *signaling)
{
	pjsua_config user_agent;
	pjsua_logging_config logging;
	pjsua_media_config media;
	pj_status_t status;

	status = pjsua_create();
	if (status != PJ_SUCCESS) {
		emit_library_error(signaling, "create signaling library", status);
		return SIPPYMODEM_SIP_SIGNALING_LIBRARY_ERROR;
	}
	signaling->library_created = true;

	pjsua_config_default(&user_agent);
	user_agent.thread_cnt = 0;
	user_agent.max_calls = 1;
	user_agent.user_agent = pj_str((char *)"sippymodem");
	user_agent.cb.on_call_state = &pjsip_call_changed;
	user_agent.cb.on_incoming_call = &pjsip_incoming_call;
	user_agent.cb.on_reg_started = &pjsip_registration_started;
	user_agent.cb.on_reg_state2 = &pjsip_registration_changed;
	pjsua_logging_config_default(&logging);
	logging.level = signaling->config.sip.log_level > PJ_LOG_MAX_LEVEL
			? PJ_LOG_MAX_LEVEL
			: signaling->config.sip.log_level;
	logging.console_level = 0;
	logging.msg_logging = signaling->callbacks.trace != NULL;
	logging.cb = &pjsip_trace;
	configure_media(&signaling->config, &media);

	status = pjsua_init(&user_agent, &logging, &media);
	if (status != PJ_SUCCESS) {
		emit_library_error(signaling, "initialize signaling library", status);
		return SIPPYMODEM_SIP_SIGNALING_LIBRARY_ERROR;
	}
	(void)pjsua_set_no_snd_dev();
	return SIPPYMODEM_SIP_SIGNALING_OK;
}

static enum sippymodem_sip_signaling_result create_transport(
	struct sippymodem_sip_signaling *signaling)
{
	pjsua_transport_config transport;
	pjsua_transport_info info;
	pj_status_t status;

	pjsua_transport_config_default(&transport);
	transport.port = signaling->config.sip.listen_port;
	if (signaling->config.sip.public_address[0] != '\0')
		transport.public_addr =
			pj_str(signaling->config.sip.public_address);
	if (signaling->config.sip.bound_address[0] != '\0')
		transport.bound_addr = pj_str(signaling->config.sip.bound_address);
	status = pjsua_transport_create(PJSIP_TRANSPORT_UDP, &transport,
					&signaling->transport_id);
	if (status != PJ_SUCCESS) {
		emit_library_error(signaling, "create SIP UDP transport", status);
		return SIPPYMODEM_SIP_SIGNALING_LIBRARY_ERROR;
	}
	status = pjsua_transport_get_info(signaling->transport_id, &info);
	if (status != PJ_SUCCESS) {
		emit_library_error(signaling, "inspect SIP UDP transport", status);
		return SIPPYMODEM_SIP_SIGNALING_LIBRARY_ERROR;
	}
	signaling->local_port = pj_sockaddr_get_port(&info.local_addr);
	if (signaling->local_port == 0) {
		emit_library_error(signaling, "resolve SIP UDP transport port",
				   PJ_EINVAL);
		return SIPPYMODEM_SIP_SIGNALING_LIBRARY_ERROR;
	}
	return SIPPYMODEM_SIP_SIGNALING_OK;
}

static enum sippymodem_sip_signaling_result add_account(
	struct sippymodem_sip_signaling *signaling)
{
	pjsua_acc_config account;
	enum sippymodem_sip_uri_result contact_result;
	pj_status_t status;

	if (!signaling->account_configured) {
		status = pjsua_acc_add_local(signaling->transport_id, PJ_TRUE,
					     &signaling->account_id);
	} else {
		pjsua_acc_config_default(&account);
		account.id = pj_str(signaling->identity_uri);
		if (signaling->config.sip.registration_enabled)
			account.reg_uri = pj_str(signaling->registrar_uri);
		account.transport_id = signaling->transport_id;
		account.register_on_acc_add =
			signaling->config.sip.registration_enabled;
		account.unreg_timeout = SIPPYMODEM_SIP_UNREGISTER_TIMEOUT_MS;
		account.cred_count = 1;
		account.cred_info[0].realm = pj_str(
			signaling->config.sip.realm[0] != '\0'
				? signaling->config.sip.realm
				: (char *)"*");
		account.cred_info[0].scheme = pj_str((char *)"digest");
		account.cred_info[0].username =
			pj_str((char *)signaling->account_user);
		account.cred_info[0].data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;
		account.cred_info[0].data =
			pj_str((char *)signaling->account_password);
		if (signaling->proxy_uri[0] != '\0') {
			account.proxy_cnt = 1;
			account.proxy[0] = pj_str(signaling->proxy_uri);
		}
		contact_result = sippymodem_sip_uri_format_contact(
			signaling->contact_uri, sizeof(signaling->contact_uri),
			signaling->account_user,
			signaling->config.sip.public_address,
			(uint16_t)signaling->local_port);
		if (contact_result == SIPPYMODEM_SIP_URI_OK)
			account.force_contact = pj_str(signaling->contact_uri);
		else if (contact_result != SIPPYMODEM_SIP_URI_NOT_CONFIGURED)
			return SIPPYMODEM_SIP_SIGNALING_INVALID_ARGUMENT;
		status = pjsua_acc_add(&account, PJ_TRUE, &signaling->account_id);
	}
	if (status != PJ_SUCCESS) {
		emit_library_error(signaling, "add SIP account", status);
		return SIPPYMODEM_SIP_SIGNALING_LIBRARY_ERROR;
	}
	if (signaling->account_configured)
		emit_trace(signaling, SIPPYMODEM_SIP_TRACE_ADAPTER, 3,
			   "SIP account configured\n",
			   sizeof("SIP account configured\n") - 1);
	return SIPPYMODEM_SIP_SIGNALING_OK;
}

static enum sippymodem_sip_signaling_result close_now(
	struct sippymodem_sip_signaling *signaling)
{
	pj_status_t status = PJ_SUCCESS;
	unsigned int destroy_flags = PJSUA_DESTROY_NO_NETWORK;

	if (signaling->closing)
		return SIPPYMODEM_SIP_SIGNALING_OK;
	signaling->closing = true;
	if (signaling->opened || signaling->library_created)
		emit_trace(signaling, SIPPYMODEM_SIP_TRACE_ADAPTER, 3,
			   "SIP signaling closing\n",
			   sizeof("SIP signaling closing\n") - 1);
	if (signaling->opened && signaling->account_configured &&
	    signaling->config.sip.registration_enabled)
		destroy_flags = signaling->registration_may_exist
				? 0
				: PJSUA_DESTROY_NO_RX_MSG;
	if (signaling->call_id != PJSUA_INVALID_ID &&
	    destroy_flags == PJSUA_DESTROY_NO_NETWORK)
		destroy_flags = PJSUA_DESTROY_NO_RX_MSG;
	if (signaling->library_created)
		status = pjsua_destroy2(destroy_flags);
	signaling->library_created = false;
	signaling->opened = false;
	signaling->closing = false;
	signaling->close_requested = false;
	signaling->transport_id = PJSUA_INVALID_ID;
	signaling->account_id = PJSUA_INVALID_ID;
	signaling->call_id = PJSUA_INVALID_ID;
	signaling->call_state = SIPPYMODEM_SIP_CALL_IDLE;
	signaling->call_starting = false;
	signaling->incoming_response_code = 0;
	signaling->local_port = 0;
	if (active_signaling == signaling)
		active_signaling = NULL;
	if (status != PJ_SUCCESS) {
		emit_library_error(signaling, "close signaling library", status);
		return SIPPYMODEM_SIP_SIGNALING_LIBRARY_ERROR;
	}
	return SIPPYMODEM_SIP_SIGNALING_OK;
}

struct sippymodem_sip_signaling *sippymodem_sip_signaling_create(
	const struct sippymodem_config *config,
	const struct sippymodem_sip_signaling_callbacks *callbacks)
{
	struct sippymodem_sip_signaling *signaling;

	if (config == NULL)
		return NULL;
	signaling = calloc(1, sizeof(*signaling));
	if (signaling == NULL)
		return NULL;
	signaling->config = *config;
	if (callbacks != NULL)
		signaling->callbacks = *callbacks;
	signaling->transport_id = PJSUA_INVALID_ID;
	signaling->account_id = PJSUA_INVALID_ID;
	signaling->call_id = PJSUA_INVALID_ID;
	signaling->call_state = SIPPYMODEM_SIP_CALL_IDLE;
	signaling->registration_state = config->sip.registration_enabled
						? SIPPYMODEM_SIP_REGISTRATION_UNREGISTERED
						: SIPPYMODEM_SIP_REGISTRATION_DISABLED;
	return signaling;
}

void sippymodem_sip_signaling_destroy(
	struct sippymodem_sip_signaling *signaling)
{
	if (signaling == NULL)
		return;
	if (signaling->callback_depth != 0) {
		signaling->close_requested = true;
		return;
	}
	(void)close_now(signaling);
	free(signaling);
}

enum sippymodem_sip_signaling_result sippymodem_sip_signaling_open(
	struct sippymodem_sip_signaling *signaling)
{
	enum sippymodem_sip_signaling_result result;
	pj_status_t status;

	if (signaling == NULL)
		return SIPPYMODEM_SIP_SIGNALING_INVALID_ARGUMENT;
	if (signaling->opened)
		return SIPPYMODEM_SIP_SIGNALING_OK;
	result = validate_config(&signaling->config);
	if (result != SIPPYMODEM_SIP_SIGNALING_OK)
		return result;
	result = resolve_account(signaling);
	if (result != SIPPYMODEM_SIP_SIGNALING_OK)
		return result;
	if (active_signaling != NULL) {
		emit_library_error(signaling, "open concurrent signaling instance",
				   PJ_EBUSY);
		return SIPPYMODEM_SIP_SIGNALING_BUSY;
	}

	active_signaling = signaling;
	signaling->close_requested = false;
	result = initialize_library(signaling);
	if (result != SIPPYMODEM_SIP_SIGNALING_OK)
		goto fail;
	if (signaling->close_requested)
		goto fail;
	result = create_transport(signaling);
	if (result != SIPPYMODEM_SIP_SIGNALING_OK)
		goto fail;
	if (signaling->close_requested)
		goto fail;
	status = pjsua_start();
	if (status != PJ_SUCCESS) {
		emit_library_error(signaling, "start signaling library", status);
		result = SIPPYMODEM_SIP_SIGNALING_LIBRARY_ERROR;
		goto fail;
	}
	if (signaling->close_requested)
		goto fail;
	result = add_account(signaling);
	if (result != SIPPYMODEM_SIP_SIGNALING_OK)
		goto fail;
	if (signaling->close_requested)
		goto fail;

	signaling->opened = true;
	signaling->last_library_status = PJ_SUCCESS;
	emit_trace(signaling, SIPPYMODEM_SIP_TRACE_ADAPTER, 3,
		   "SIP signaling ready\n",
		   sizeof("SIP signaling ready\n") - 1);
	if (signaling->close_requested)
		return close_now(signaling);
	if (signaling->callbacks.ready != NULL) {
		callback_enter(signaling);
		signaling->callbacks.ready(signaling->callbacks.context);
		callback_leave(signaling);
	}
	if (signaling->close_requested)
		return close_now(signaling);
	return SIPPYMODEM_SIP_SIGNALING_OK;

fail:
	(void)close_now(signaling);
	return result;
}

enum sippymodem_sip_signaling_result sippymodem_sip_signaling_close(
	struct sippymodem_sip_signaling *signaling)
{
	if (signaling == NULL)
		return SIPPYMODEM_SIP_SIGNALING_INVALID_ARGUMENT;
	if (signaling->callback_depth != 0) {
		signaling->close_requested = true;
		return SIPPYMODEM_SIP_SIGNALING_OK;
	}
	return close_now(signaling);
}

enum sippymodem_sip_signaling_result sippymodem_sip_signaling_step(
	struct sippymodem_sip_signaling *signaling, unsigned int timeout_ms)
{
	int events;

	if (signaling == NULL)
		return SIPPYMODEM_SIP_SIGNALING_INVALID_ARGUMENT;
	if (!signaling->opened)
		return SIPPYMODEM_SIP_SIGNALING_NOT_OPEN;
	events = pjsua_handle_events(timeout_ms);
	if (events < 0) {
		emit_library_error(signaling, "poll SIP events", (pj_status_t)-events);
		if (signaling->close_requested)
			(void)close_now(signaling);
		return SIPPYMODEM_SIP_SIGNALING_LIBRARY_ERROR;
	}
	if (signaling->close_requested)
		return close_now(signaling);
	return SIPPYMODEM_SIP_SIGNALING_OK;
}

enum sippymodem_sip_signaling_result sippymodem_sip_signaling_dial(
	struct sippymodem_sip_signaling *signaling, const char *target)
{
	enum sippymodem_sip_uri_result uri_result;
	pjsua_call_info info;
	pjsua_call_id call_id = PJSUA_INVALID_ID;
	pj_str_t target_uri;
	pj_status_t status;

	if (signaling == NULL)
		return SIPPYMODEM_SIP_SIGNALING_INVALID_ARGUMENT;
	if (!signaling->opened)
		return SIPPYMODEM_SIP_SIGNALING_NOT_OPEN;
	if (signaling->call_id != PJSUA_INVALID_ID || signaling->call_starting)
		return SIPPYMODEM_SIP_SIGNALING_CALL_ACTIVE;
	uri_result = sippymodem_sip_uri_format_target(
		signaling->target_uri, sizeof(signaling->target_uri), target,
		signaling->config.sip.target, signaling->account_domain);
	if (uri_result != SIPPYMODEM_SIP_URI_OK)
		return SIPPYMODEM_SIP_SIGNALING_INVALID_ARGUMENT;

	target_uri = pj_str(signaling->target_uri);
	signaling->call_direction = SIPPYMODEM_SIP_CALL_DIRECTION_OUTGOING;
	signaling->call_starting = true;
	status = pjsua_call_make_call(signaling->account_id, &target_uri, NULL,
				      NULL, NULL, &call_id);
	if (status != PJ_SUCCESS) {
		signaling->call_starting = false;
		signaling->call_id = PJSUA_INVALID_ID;
		if (signaling->call_state != SIPPYMODEM_SIP_CALL_IDLE)
			emit_call_event(signaling, SIPPYMODEM_SIP_CALL_IDLE,
					NULL);
		emit_library_error(signaling, "place SIP call", status);
		if (signaling->close_requested)
			(void)close_now(signaling);
		return SIPPYMODEM_SIP_SIGNALING_LIBRARY_ERROR;
	}
	if (signaling->call_starting) {
		signaling->call_starting = false;
		if (signaling->call_id == PJSUA_INVALID_ID)
			signaling->call_id = call_id;
		if (signaling->call_state == SIPPYMODEM_SIP_CALL_IDLE) {
			if (pjsua_call_get_info(call_id, &info) == PJ_SUCCESS)
				emit_call_event(signaling,
						SIPPYMODEM_SIP_CALL_DIALING,
						&info);
			else
				emit_call_event(signaling,
						SIPPYMODEM_SIP_CALL_DIALING,
						NULL);
		}
	}
	if (signaling->close_requested)
		return close_now(signaling);
	return SIPPYMODEM_SIP_SIGNALING_OK;
}

enum sippymodem_sip_signaling_result sippymodem_sip_signaling_answer(
	struct sippymodem_sip_signaling *signaling)
{
	pjsua_call_id call_id;
	pj_status_t status;
	unsigned int previous_response_code;

	if (signaling == NULL)
		return SIPPYMODEM_SIP_SIGNALING_INVALID_ARGUMENT;
	if (!signaling->opened)
		return SIPPYMODEM_SIP_SIGNALING_NOT_OPEN;
	call_id = signaling->call_id;
	if (call_id == PJSUA_INVALID_ID ||
	    signaling->call_direction != SIPPYMODEM_SIP_CALL_DIRECTION_INCOMING)
		return SIPPYMODEM_SIP_SIGNALING_NO_CALL;
	if (signaling->call_state == SIPPYMODEM_SIP_CALL_CONNECTED ||
	    signaling->incoming_response_code == 200)
		return SIPPYMODEM_SIP_SIGNALING_OK;
	if (signaling->incoming_response_code >= 300)
		return SIPPYMODEM_SIP_SIGNALING_NO_CALL;
	previous_response_code = signaling->incoming_response_code;
	signaling->incoming_response_code = 200;
	status = pjsua_call_answer(call_id, 200, NULL, NULL);
	if (status != PJ_SUCCESS) {
		signaling->incoming_response_code = previous_response_code;
		emit_library_error(signaling, "answer incoming SIP call", status);
		if (signaling->close_requested)
			(void)close_now(signaling);
		return SIPPYMODEM_SIP_SIGNALING_LIBRARY_ERROR;
	}
	if (signaling->close_requested)
		return close_now(signaling);
	return SIPPYMODEM_SIP_SIGNALING_OK;
}

enum sippymodem_sip_signaling_result sippymodem_sip_signaling_hangup(
	struct sippymodem_sip_signaling *signaling)
{
	pjsua_call_id call_id;
	pj_status_t status;

	if (signaling == NULL)
		return SIPPYMODEM_SIP_SIGNALING_INVALID_ARGUMENT;
	if (!signaling->opened)
		return SIPPYMODEM_SIP_SIGNALING_NOT_OPEN;
	call_id = signaling->call_id;
	if (call_id == PJSUA_INVALID_ID)
		return SIPPYMODEM_SIP_SIGNALING_NO_CALL;
	if (signaling->call_direction ==
	    SIPPYMODEM_SIP_CALL_DIRECTION_INCOMING &&
	    signaling->call_state != SIPPYMODEM_SIP_CALL_CONNECTED)
		signaling->incoming_response_code = 603;
	status = pjsua_call_hangup(call_id, 0, NULL, NULL);
	if (status != PJ_SUCCESS) {
		emit_library_error(signaling, "hang up SIP call", status);
		if (signaling->close_requested)
			(void)close_now(signaling);
		return SIPPYMODEM_SIP_SIGNALING_LIBRARY_ERROR;
	}
	if (signaling->close_requested)
		return close_now(signaling);
	return SIPPYMODEM_SIP_SIGNALING_OK;
}

bool sippymodem_sip_signaling_is_open(
	const struct sippymodem_sip_signaling *signaling)
{
	return signaling != NULL && signaling->opened;
}

unsigned int sippymodem_sip_signaling_local_port(
	const struct sippymodem_sip_signaling *signaling)
{
	return signaling != NULL && signaling->opened ? signaling->local_port : 0;
}

const char *sippymodem_sip_signaling_identity_uri(
	const struct sippymodem_sip_signaling *signaling)
{
	return signaling != NULL && signaling->account_configured
		       ? signaling->identity_uri
		       : NULL;
}

enum sippymodem_sip_registration_state
sippymodem_sip_signaling_registration_state(
	const struct sippymodem_sip_signaling *signaling)
{
	return signaling != NULL ? signaling->registration_state
				 : SIPPYMODEM_SIP_REGISTRATION_DISABLED;
}

enum sippymodem_sip_call_state sippymodem_sip_signaling_call_state(
	const struct sippymodem_sip_signaling *signaling)
{
	return signaling != NULL ? signaling->call_state
				 : SIPPYMODEM_SIP_CALL_IDLE;
}

int sippymodem_sip_signaling_last_library_status(
	const struct sippymodem_sip_signaling *signaling)
{
	return signaling != NULL ? signaling->last_library_status : 0;
}

const char *sippymodem_sip_signaling_result_name(
	enum sippymodem_sip_signaling_result result)
{
	switch (result) {
	case SIPPYMODEM_SIP_SIGNALING_OK:
		return "ok";
	case SIPPYMODEM_SIP_SIGNALING_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_SIP_SIGNALING_NOT_OPEN:
		return "signaling is not open";
	case SIPPYMODEM_SIP_SIGNALING_BUSY:
		return "another signaling instance is open";
	case SIPPYMODEM_SIP_SIGNALING_CALL_ACTIVE:
		return "a SIP call is already active";
	case SIPPYMODEM_SIP_SIGNALING_NO_CALL:
		return "no SIP call is active";
	case SIPPYMODEM_SIP_SIGNALING_LIBRARY_ERROR:
		return "signaling library error";
	}
	return "unknown signaling result";
}

const char *sippymodem_sip_registration_state_name(
	enum sippymodem_sip_registration_state state)
{
	switch (state) {
	case SIPPYMODEM_SIP_REGISTRATION_DISABLED:
		return "disabled";
	case SIPPYMODEM_SIP_REGISTRATION_REGISTERING:
		return "registering";
	case SIPPYMODEM_SIP_REGISTRATION_REGISTERED:
		return "registered";
	case SIPPYMODEM_SIP_REGISTRATION_UNREGISTERING:
		return "unregistering";
	case SIPPYMODEM_SIP_REGISTRATION_UNREGISTERED:
		return "unregistered";
	case SIPPYMODEM_SIP_REGISTRATION_FAILED:
		return "registration failed";
	case SIPPYMODEM_SIP_REGISTRATION_UNREGISTER_FAILED:
		return "unregistration failed";
	}
	return "unknown registration state";
}

const char *sippymodem_sip_call_state_name(
	enum sippymodem_sip_call_state state)
{
	switch (state) {
	case SIPPYMODEM_SIP_CALL_IDLE:
		return "idle";
	case SIPPYMODEM_SIP_CALL_DIALING:
		return "dialing";
	case SIPPYMODEM_SIP_CALL_INCOMING_PENDING:
		return "incoming";
	case SIPPYMODEM_SIP_CALL_EARLY:
		return "early";
	case SIPPYMODEM_SIP_CALL_CONNECTED:
		return "connected";
	}
	return "unknown call state";
}
