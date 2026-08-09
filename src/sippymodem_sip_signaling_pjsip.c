// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_sip_signaling.h"
#include "sippymodem_sdp.h"

#include <pjsua-lib/pjsua.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct sippymodem_sip_signaling {
	struct sippymodem_config config;
	struct sippymodem_sip_signaling_callbacks callbacks;
	pjsua_transport_id transport_id;
	pjsua_acc_id account_id;
	int last_library_status;
	unsigned int local_port;
	unsigned int callback_depth;
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

static bool text_is_terminated(const char *text, size_t capacity)
{
	return memchr(text, '\0', capacity) != NULL;
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
	    !text_is_terminated(config->sip.public_address,
				sizeof(config->sip.public_address)) ||
	    !text_is_terminated(config->sip.bound_address,
				sizeof(config->sip.bound_address)))
		return SIPPYMODEM_SIP_SIGNALING_INVALID_ARGUMENT;
	if (config->sip.registration_enabled)
		return SIPPYMODEM_SIP_SIGNALING_UNSUPPORTED_CONFIGURATION;
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

static enum sippymodem_sip_signaling_result close_now(
	struct sippymodem_sip_signaling *signaling)
{
	pj_status_t status = PJ_SUCCESS;

	if (signaling->closing)
		return SIPPYMODEM_SIP_SIGNALING_OK;
	signaling->closing = true;
	if (signaling->opened || signaling->library_created)
		emit_trace(signaling, SIPPYMODEM_SIP_TRACE_ADAPTER, 3,
			   "SIP signaling closing\n",
			   sizeof("SIP signaling closing\n") - 1);
	/* This stage has no registered account or call to notify on shutdown. */
	if (signaling->library_created)
		status = pjsua_destroy2(PJSUA_DESTROY_NO_NETWORK);
	signaling->library_created = false;
	signaling->opened = false;
	signaling->closing = false;
	signaling->close_requested = false;
	signaling->transport_id = PJSUA_INVALID_ID;
	signaling->account_id = PJSUA_INVALID_ID;
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
	status = pjsua_acc_add_local(signaling->transport_id, PJ_TRUE,
				     &signaling->account_id);
	if (status != PJ_SUCCESS) {
		emit_library_error(signaling, "add local SIP account", status);
		result = SIPPYMODEM_SIP_SIGNALING_LIBRARY_ERROR;
		goto fail;
	}
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
	case SIPPYMODEM_SIP_SIGNALING_UNSUPPORTED_CONFIGURATION:
		return "registered accounts are not available";
	case SIPPYMODEM_SIP_SIGNALING_LIBRARY_ERROR:
		return "signaling library error";
	}
	return "unknown signaling result";
}
