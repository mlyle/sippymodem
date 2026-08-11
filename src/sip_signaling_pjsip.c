// SPDX-License-Identifier: BSD-3-Clause

#include "sip_signaling.h"
#include "sip_uri.h"
#include "sdp.h"
#include "sdp_pjsip.h"

#include <pjsua-lib/pjsua.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
	SIP_ACCOUNT_URI_MAX = SIPPYMODEM_CONFIG_TEXT_MAX * 2 + 16,
	SIP_CALL_IDENTITY_MAX = 256,
	SIP_CALL_REASON_MAX = 128,
	SIP_MEDIA_ADDRESS_MAX = 64,
	SIP_REGISTRATION_REASON_MAX = 128,
	SIP_UNREGISTER_TIMEOUT_MS = 1000,
};

struct sip_signaling {
	struct sippymodem_config config;
	struct sip_signaling_callbacks callbacks;
	pjsua_transport_id transport_id;
	pjsua_acc_id account_id;
	pjsua_call_id call_id;
	int last_library_status;
	unsigned int local_port;
	unsigned int callback_depth;
	char login_buffer[SIPPYMODEM_CONFIG_TEXT_MAX];
	char identity_uri[SIP_ACCOUNT_URI_MAX];
	char registrar_uri[SIP_ACCOUNT_URI_MAX];
	char proxy_uri[SIP_ACCOUNT_URI_MAX];
	char contact_uri[SIP_ACCOUNT_URI_MAX];
	char target_uri[SIP_ACCOUNT_URI_MAX];
	char call_remote_identity[SIP_CALL_IDENTITY_MAX];
	char call_reason[SIP_CALL_REASON_MAX];
	char media_remote_address[SIP_MEDIA_ADDRESS_MAX];
	char registration_reason[SIP_REGISTRATION_REASON_MAX];
	const char *account_user;
	const char *account_password;
	const char *account_domain;
	enum sip_call_direction call_direction;
	enum sip_call_state call_state;
	enum sip_media_state media_state;
	enum sip_media_address_family media_remote_family;
	enum sip_registration_state registration_state;
	unsigned int incoming_response_code;
	unsigned int media_remote_port;
	bool call_starting;
	bool registration_may_exist;
	bool account_configured;
	bool library_created;
	bool opened;
	bool closing;
	bool close_requested;
};

static struct sip_signaling *active_signaling;

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
		emit_trace(active_signaling, SIP_TRACE_LIBRARY, level,
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
	struct sip_signaling *signaling = active_signaling;
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
	struct sip_signaling *signaling = active_signaling;
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
	struct sip_signaling *signaling = active_signaling;
	struct sdp_audio_description description;
	pj_status_t status;

	(void)call_id;
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
		signaling->config.sip.public_address);
	if (status != PJ_SUCCESS)
		emit_library_error(signaling, "apply modem audio SDP", status);
}

static void pjsip_incoming_call(pjsua_acc_id account_id,
				pjsua_call_id call_id,
				pjsip_rx_data *request)
{
	struct sip_signaling *signaling = active_signaling;
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
	struct sip_signaling *signaling,
	enum sip_registration_state state,
	unsigned int status_code, pj_status_t library_status,
	unsigned int expires_seconds, const pj_str_t *reason)
{
	struct sip_registration_event event;
	if (signaling == NULL)
		return;
	copy_pj_text(signaling->registration_reason,
		     sizeof(signaling->registration_reason), reason);
	signaling->registration_state = state;
	if (state == SIP_REGISTRATION_REGISTERED)
		signaling->registration_may_exist = true;
	else if (state == SIP_REGISTRATION_UNREGISTERED)
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

	(void)account_id;
	if (active_signaling == NULL || info == NULL || info->cbparam == NULL)
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
	emit_registration_event(active_signaling, state, status_code,
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
	const struct sippymodem_config *config)
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
	struct sip_signaling *signaling)
{
	const struct sip_config *sip = &signaling->config.sip;
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
			return SIP_SIGNALING_INVALID_ARGUMENT;
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
			return SIP_SIGNALING_INVALID_ARGUMENT;
		signaling->account_domain =
			domain[0] != '\0' ? domain : NULL;
		return SIP_SIGNALING_OK;
	}
	if (strchr(user, '@') != NULL || strchr(user, ':') != NULL ||
	    strchr(domain, '@') != NULL)
		return SIP_SIGNALING_INVALID_ARGUMENT;

	if (!format_account_uri(signaling->identity_uri,
				sizeof(signaling->identity_uri), user, domain))
		return SIP_SIGNALING_INVALID_ARGUMENT;
	if (sip->identity_uri[0] != '\0' &&
	    !format_account_uri(signaling->identity_uri,
				sizeof(signaling->identity_uri),
				sip->identity_uri, domain))
		return SIP_SIGNALING_INVALID_ARGUMENT;
	if (sip->registrar_uri[0] != '\0') {
		if (!format_account_uri(signaling->registrar_uri,
					 sizeof(signaling->registrar_uri),
					 sip->registrar_uri, NULL))
			return SIP_SIGNALING_INVALID_ARGUMENT;
	} else {
		length = snprintf(signaling->registrar_uri,
				  sizeof(signaling->registrar_uri), "sip:%s", domain);
		if (length < 0 ||
		    (size_t)length >= sizeof(signaling->registrar_uri))
			return SIP_SIGNALING_INVALID_ARGUMENT;
	}
	if (sip->proxy_uri[0] != '\0' &&
	    !format_account_uri(signaling->proxy_uri,
				sizeof(signaling->proxy_uri), sip->proxy_uri, NULL))
		return SIP_SIGNALING_INVALID_ARGUMENT;

	signaling->account_user = user;
	signaling->account_password = password;
	signaling->account_domain = domain;
	signaling->account_configured = true;
	return SIP_SIGNALING_OK;
}

static void configure_media(const struct sippymodem_config *config,
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

static const char *pjsip_seed_codec(enum media_codec codec)
{
	switch (codec) {
	case MEDIA_CODEC_PCMA:
		return "PCMA/8000/1";
	case MEDIA_CODEC_CLEARMODE:
		/* The SDP callback replaces this library-internal seed mapping. */
		return "PCMU/8000/1";
	case MEDIA_CODEC_PCMU:
		return "PCMU/8000/1";
	}
	return NULL;
}

static pj_status_t configure_codecs(
	const struct sip_signaling *signaling)
{
	pjsua_codec_info codecs[PJMEDIA_CODEC_MGR_MAX_CODECS];
	const char *preferred = pjsip_seed_codec(signaling->config.media.codec);
	pjmedia_codec_param parameters;
	pj_str_t codec_id;
	unsigned int count = PJ_ARRAY_SIZE(codecs);
	bool preferred_found = false;
	pj_status_t status;

	if (preferred == NULL)
		return PJ_EINVAL;
	status = pjsua_enum_codecs(codecs, &count);
	if (status != PJ_SUCCESS)
		return status;
	for (unsigned int index = 0; index < count; ++index) {
		bool selected = pj_strcmp2(&codecs[index].codec_id, preferred) == 0;

		status = pjsua_codec_set_priority(
			&codecs[index].codec_id,
			selected ? PJMEDIA_CODEC_PRIO_NORMAL :
				   PJMEDIA_CODEC_PRIO_DISABLED);
		if (status != PJ_SUCCESS)
			return status;
		if (selected)
			preferred_found = true;
	}
	if (!preferred_found)
		return PJ_ENOTFOUND;
	codec_id = pj_str((char *)preferred);
	status = pjsua_codec_get_param(&codec_id, &parameters);
	if (status != PJ_SUCCESS)
		return status;
	parameters.setting.vad = 0;
	parameters.setting.cng = 0;
	parameters.setting.plc = 0;
	return pjsua_codec_set_param(&codec_id, &parameters);
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
	signaling->library_created = true;

	pjsua_config_default(&user_agent);
	user_agent.thread_cnt = 0;
	user_agent.max_calls = 1;
	user_agent.user_agent = pj_str((char *)"sippymodem");
	user_agent.cb.on_call_state = &pjsip_call_changed;
	user_agent.cb.on_call_sdp_created = &pjsip_sdp_created;
	user_agent.cb.on_stream_precreate = &pjsip_stream_precreate;
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
	struct sip_signaling *signaling)
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
		return SIP_SIGNALING_LIBRARY_ERROR;
	}
	status = pjsua_transport_get_info(signaling->transport_id, &info);
	if (status != PJ_SUCCESS) {
		emit_library_error(signaling, "inspect SIP UDP transport", status);
		return SIP_SIGNALING_LIBRARY_ERROR;
	}
	signaling->local_port = pj_sockaddr_get_port(&info.local_addr);
	if (signaling->local_port == 0) {
		emit_library_error(signaling, "resolve SIP UDP transport port",
				   PJ_EINVAL);
		return SIP_SIGNALING_LIBRARY_ERROR;
	}
	return SIP_SIGNALING_OK;
}

static enum sip_signaling_result add_account(
	struct sip_signaling *signaling)
{
	pjsua_acc_config account;
	enum sip_uri_result contact_result;
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
		account.unreg_timeout = SIP_UNREGISTER_TIMEOUT_MS;
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
		contact_result = sip_uri_format_contact(
			signaling->contact_uri, sizeof(signaling->contact_uri),
			signaling->account_user,
			signaling->config.sip.public_address,
			(uint16_t)signaling->local_port);
		if (contact_result == SIP_URI_OK)
			account.force_contact = pj_str(signaling->contact_uri);
		else if (contact_result != SIP_URI_NOT_CONFIGURED)
			return SIP_SIGNALING_INVALID_ARGUMENT;
		status = pjsua_acc_add(&account, PJ_TRUE, &signaling->account_id);
	}
	if (status != PJ_SUCCESS) {
		emit_library_error(signaling, "add SIP account", status);
		return SIP_SIGNALING_LIBRARY_ERROR;
	}
	if (signaling->account_configured)
		emit_trace(signaling, SIP_TRACE_ADAPTER, 3,
			   "SIP account configured\n",
			   sizeof("SIP account configured\n") - 1);
	return SIP_SIGNALING_OK;
}

static enum sip_signaling_result close_now(
	struct sip_signaling *signaling)
{
	pj_status_t status = PJ_SUCCESS;
	unsigned int destroy_flags = PJSUA_DESTROY_NO_NETWORK;

	if (signaling->closing)
		return SIP_SIGNALING_OK;
	signaling->closing = true;
	if (signaling->opened || signaling->library_created)
		emit_trace(signaling, SIP_TRACE_ADAPTER, 3,
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
	emit_media_inactive(signaling);
	if (signaling->library_created)
		status = pjsua_destroy2(destroy_flags);
	signaling->library_created = false;
	signaling->opened = false;
	signaling->closing = false;
	signaling->close_requested = false;
	signaling->transport_id = PJSUA_INVALID_ID;
	signaling->account_id = PJSUA_INVALID_ID;
	signaling->call_id = PJSUA_INVALID_ID;
	signaling->call_state = SIP_CALL_IDLE;
	signaling->media_state = SIP_MEDIA_INACTIVE;
	signaling->media_remote_family =
		SIP_MEDIA_ADDRESS_UNSPECIFIED;
	signaling->media_remote_address[0] = '\0';
	signaling->media_remote_port = 0;
	signaling->call_starting = false;
	signaling->incoming_response_code = 0;
	signaling->local_port = 0;
	if (active_signaling == signaling)
		active_signaling = NULL;
	if (status != PJ_SUCCESS) {
		emit_library_error(signaling, "close signaling library", status);
		return SIP_SIGNALING_LIBRARY_ERROR;
	}
	return SIP_SIGNALING_OK;
}

struct sip_signaling *sip_signaling_create(
	const struct sippymodem_config *config,
	const struct sip_signaling_callbacks *callbacks)
{
	struct sip_signaling *signaling;

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
	signaling->call_state = SIP_CALL_IDLE;
	signaling->registration_state = config->sip.registration_enabled
						? SIP_REGISTRATION_UNREGISTERED
						: SIP_REGISTRATION_DISABLED;
	return signaling;
}

void sip_signaling_destroy(
	struct sip_signaling *signaling)
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

enum sip_signaling_result sip_signaling_open(
	struct sip_signaling *signaling)
{
	enum sip_signaling_result result;
	pj_status_t status;

	if (signaling == NULL)
		return SIP_SIGNALING_INVALID_ARGUMENT;
	if (signaling->opened)
		return SIP_SIGNALING_OK;
	result = validate_config(&signaling->config);
	if (result != SIP_SIGNALING_OK)
		return result;
	result = resolve_account(signaling);
	if (result != SIP_SIGNALING_OK)
		return result;
	if (active_signaling != NULL) {
		emit_library_error(signaling, "open concurrent signaling instance",
				   PJ_EBUSY);
		return SIP_SIGNALING_BUSY;
	}

	active_signaling = signaling;
	signaling->close_requested = false;
	result = initialize_library(signaling);
	if (result != SIP_SIGNALING_OK)
		goto fail;
	if (signaling->close_requested)
		goto fail;
	result = create_transport(signaling);
	if (result != SIP_SIGNALING_OK)
		goto fail;
	if (signaling->close_requested)
		goto fail;
	status = pjsua_start();
	if (status != PJ_SUCCESS) {
		emit_library_error(signaling, "start signaling library", status);
		result = SIP_SIGNALING_LIBRARY_ERROR;
		goto fail;
	}
	if (signaling->close_requested)
		goto fail;
	result = add_account(signaling);
	if (result != SIP_SIGNALING_OK)
		goto fail;
	if (signaling->close_requested)
		goto fail;

	signaling->opened = true;
	signaling->last_library_status = PJ_SUCCESS;
	emit_trace(signaling, SIP_TRACE_ADAPTER, 3,
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
	return SIP_SIGNALING_OK;

fail:
	(void)close_now(signaling);
	return result;
}

enum sip_signaling_result sip_signaling_close(
	struct sip_signaling *signaling)
{
	if (signaling == NULL)
		return SIP_SIGNALING_INVALID_ARGUMENT;
	if (signaling->callback_depth != 0) {
		signaling->close_requested = true;
		return SIP_SIGNALING_OK;
	}
	return close_now(signaling);
}

enum sip_signaling_result sip_signaling_step(
	struct sip_signaling *signaling, unsigned int timeout_ms)
{
	int events;

	if (signaling == NULL)
		return SIP_SIGNALING_INVALID_ARGUMENT;
	if (!signaling->opened)
		return SIP_SIGNALING_NOT_OPEN;
	events = pjsua_handle_events(timeout_ms);
	if (events < 0) {
		emit_library_error(signaling, "poll SIP events", (pj_status_t)-events);
		if (signaling->close_requested)
			(void)close_now(signaling);
		return SIP_SIGNALING_LIBRARY_ERROR;
	}
	if (signaling->close_requested)
		return close_now(signaling);
	return SIP_SIGNALING_OK;
}

enum sip_signaling_result sip_signaling_dial(
	struct sip_signaling *signaling, const char *target)
{
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
	uri_result = sip_uri_format_target(
		signaling->target_uri, sizeof(signaling->target_uri), target,
		signaling->config.sip.target, signaling->account_domain);
	if (uri_result != SIP_URI_OK)
		return SIP_SIGNALING_INVALID_ARGUMENT;

	target_uri = pj_str(signaling->target_uri);
	signaling->call_direction = SIP_CALL_DIRECTION_OUTGOING;
	signaling->call_starting = true;
	status = pjsua_call_make_call(signaling->account_id, &target_uri, NULL,
				      NULL, NULL, &call_id);
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
	status = pjsua_call_hangup(call_id, 0, NULL, NULL);
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
	return signaling != NULL && signaling->opened ? signaling->local_port : 0;
}

const char *sip_signaling_identity_uri(
	const struct sip_signaling *signaling)
{
	return signaling != NULL && signaling->account_configured
		       ? signaling->identity_uri
		       : NULL;
}

enum sip_registration_state
sip_signaling_registration_state(
	const struct sip_signaling *signaling)
{
	return signaling != NULL ? signaling->registration_state
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
