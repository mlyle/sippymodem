// SPDX-License-Identifier: BSD-3-Clause
#include "sippymodem.h"

#include "byte_queue.h"
#include "integer_math.h"
#include "datapump_link.h"
#include "line.h"
#include "media_workers.h"
#include "wake_signal.h"
#include "log_buffer.h"
#include "modem_state.h"
#include "sip_uri.h"

#include <errno.h>
#ifndef _WIN32
#include <poll.h>
#endif
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

enum { LINE_LIMIT = 32, EVENT_LIMIT = 128, QUEUE_LIMIT = 1048576, ENDPOINT_LIMIT = 8 };
struct sippymodem_line {
	struct sippymodem_server *server;
	struct modem_line *core;
	struct media_worker *worker;
	uint64_t media_deadline;
	bool finish_requested;
	int finish_pending;
	struct sippymodem_line_config config;
	struct sippymodem_status status;
	struct sippymodem_call_facts facts;
	struct byte_queue rx;
	struct log_buffer logs;
	struct sippymodem_log_record log_storage[SIPPYMODEM_LOG_CAPACITY];
	uint64_t log_time;
	char log_step[128];
	char log_phase[48];
	unsigned int id, slot;
	uint64_t sip_generation;
	uint64_t attempt_deadline;
	bool active;
	bool remote_closing;
	enum sippymodem_disconnect_cause closing_cause;
	bool terminal_due;
	bool incoming;
	bool started;
	bool stopping;
};

struct sippymodem_huntgroup {
	struct sippymodem_server *server;
	struct sippymodem_line *members[LINE_LIMIT];
};
struct sippymodem_endpoint {
	struct sippymodem_server *server;
	struct sippymodem_huntgroup *group;
	struct sip_endpoint *sip;
	unsigned int id;
	char target[APP_CONFIG_TEXT_MAX];
	bool started;
};

struct sippymodem_server {
	struct app_config config;
	struct sip_server *sip;
	struct sippymodem_endpoint *endpoints[ENDPOINT_LIMIT], *default_endpoint;
	struct sippymodem_huntgroup *groups[LINE_LIMIT], *default_group;
	struct sippymodem_line *lines[LINE_LIMIT];
	struct sippymodem_event events[EVENT_LIMIT];
	unsigned int max_lines, event_head, event_count, next_id;
	struct sippymodem_event terminal_events[LINE_LIMIT];
	bool terminal_pending[LINE_LIMIT];
	struct wake_signal wake;
#ifdef _WIN32
	HANDLE wait_timer; /* Lazy, owner-thread-only deadline timer. */
#endif
	uint64_t now, next_deadline;
	bool have_now, time_valid, started, processing, event_overflow;
};

static bool collect_media(struct sippymodem_line *line, bool wait);

static struct modem *line_modem(const struct sippymodem_line *line)
{
	return modem_line_modem(line->core);
}

static struct modem_host *line_host(const struct sippymodem_line *line)
{
	return modem_line_host(line->core);
}

static void snapshot(const struct sippymodem_line *line, struct sippymodem_status *status)
{
	const struct modem *modem = line_modem(line);
	*status = line->status;
	status->started = line->started;
	status->rtp_port = modem_host_rtp_local_port(line_host(line));
	status->off_hook = modem->hook != MODEM_HOOK_ON;
	status->carrier = (modem->modem_info & MODEM_SIGNAL_CD) != 0;
	bool connected = line->active && (modem->state == STATE_MODEM_ONLINE ||
					 modem->state == STATE_COMMAND_ONLINE);
	status->ready = connected && !line->remote_closing;
	status->dte_mode = modem->command ? SIPPYMODEM_COMMAND : SIPPYMODEM_DATA;
	status->phase = modem->started ? SIPPYMODEM_DISCONNECTING : SIPPYMODEM_IDLE;
	if (line->active) {
		if (line->remote_closing)
			status->phase = SIPPYMODEM_DISCONNECTING;
		else if (status->ready)
			status->phase = SIPPYMODEM_ONLINE;
		else if (modem->state == STATE_EC_DISC || modem->state == STATE_DP_DISC)
			status->phase = SIPPYMODEM_DISCONNECTING;
		else if (status->sip_state == SIPPYMODEM_SIP_CONNECTED)
			status->phase = SIPPYMODEM_TRAINING;
		else if (!line->incoming)
			status->phase = SIPPYMODEM_DIALING;
		else
			status->phase = SIPPYMODEM_RINGING;
	}
	struct dp_link_rates rates = datapump_get_link_rates(modem->dp);
	status->tx_bps = rates.transmit_bps;
	status->rx_bps = rates.receive_bps;
	status->tx_baud = rates.transmit_baud;
	status->rx_baud = rates.receive_baud;
	status->modulation = connected && modem->dp != NULL ? (unsigned int)modem->dp->id : 0;
	status->error_control = connected && modem->cfg.ec ? SIPPYMODEM_LAPM : SIPPYMODEM_RAW;
	status->compression = SIPPYMODEM_COMP_NONE;
	if (connected) {
		int compression = modem_ec_active_compression(modem);
		if (compression == MODEM_EC_COMPRESSION_V44)
			status->compression = SIPPYMODEM_COMP_V44;
		else if (compression == MODEM_EC_COMPRESSION_V42BIS)
			status->compression = SIPPYMODEM_COMP_V42BIS;
	}
	status->tx_pending = (size_t)modem->xmit.count;
	status->tx_free = modem->xmit.size - status->tx_pending;
	status->rx_ready = line->rx.count;
	status->rx_overflow = modem->dte_overflow;
}

static void log_line(struct sippymodem_line *line, enum sippymodem_log_level level,
	const char *format, ...) __attribute__((format(printf, 3, 4)));
static void log_line(struct sippymodem_line *line, enum sippymodem_log_level level,
	const char *format, ...)
{
	struct sippymodem_log_record record = {
		.time_ns = line->server->have_now ? line->server->now : 0,
		.generation = line->status.generation, .line_id = line->id, .level = level,
	};
	va_list args;
	va_start(args, format);
	int length = vsnprintf(record.text, sizeof(record.text), format, args);
	va_end(args);
	if (length < 0)
		return;
	for (size_t i = 0; record.text[i] != '\0'; ++i)
		if ((unsigned char)record.text[i] < 32 || record.text[i] == 127)
			record.text[i] = '?';
	record.truncated = (size_t)length >= sizeof(record.text);
	(void)log_buffer_append(&line->logs, &record);
}

static void log_event(struct sippymodem_line *line, enum sippymodem_event_type type,
	const struct sippymodem_status *status)
{
	static const char *const names[] = { "incoming", "sip-established", "connected", "state", "disconnected" };
	if (type == SIPPYMODEM_EVENT_DISCONNECTED) {
		enum sippymodem_log_level level = status->disconnect_cause >= SIPPYMODEM_DISCONNECT_MODEM ?
			SIPPYMODEM_LOG_WARNING : SIPPYMODEM_LOG_INFO;
		log_line(line, level, "disconnected origin=%s cause=%u reason=%s",
			status->disconnect_local ? "local" : "remote", status->disconnect_cause,
			status->disconnect_reason);
	} else {
		log_line(line, SIPPYMODEM_LOG_INFO,
			"%s phase=%u sip=%u ready=%u carrier=%u mode=%u modulation=%u tx=%u rx=%u tx-baud=%.6g rx-baud=%.6g ec=%u compression=%u",
			names[type], status->phase, status->sip_state, status->ready, status->carrier,
			status->dte_mode, status->modulation, status->tx_bps, status->rx_bps,
			status->tx_baud, status->rx_baud, status->error_control, status->compression);
	}
}

static void log_progress(struct sippymodem_line *line)
{
	if (!line->active || (line->status.phase != SIPPYMODEM_TRAINING &&
	    line->status.phase != SIPPYMODEM_DIALING))
		return;
	const char *phase = modem_call_phase(line_modem(line));
	const char *step = modem_protocol_step(line_modem(line));
	bool changed = strcmp(line->log_phase, phase) != 0 || strcmp(line->log_step, step) != 0;
	uint64_t interval = changed ? UINT64_C(250000000) : UINT64_C(1000000000);
	if (line->log_phase[0] != '\0' && line->server->now - line->log_time < interval)
		return;
	log_line(line, SIPPYMODEM_LOG_DEBUG, "progress phase=%s step=%s tx=%u rx=%u",
		phase, step, line->status.tx_bps, line->status.rx_bps);
	snprintf(line->log_phase, sizeof(line->log_phase), "%s", phase);
	snprintf(line->log_step, sizeof(line->log_step), "%s", step);
	line->log_time = line->server->now;
}

static void emit(struct sippymodem_line *line, enum sippymodem_event_type type)
{
	struct sippymodem_server *server = line->server;
	struct sippymodem_status current;
	snapshot(line, &current);
	log_event(line, type, &current);
	struct sippymodem_event *event;
	if (server->event_count == EVENT_LIMIT) {
		server->event_overflow = true;
		if (type != SIPPYMODEM_EVENT_DISCONNECTED)
			return;
		event = &server->terminal_events[line->slot];
		if (server->terminal_pending[line->slot]) {
			event->type = type;
			event->line_id = line->id;
			event->status = current;
			return;
		}
		server->terminal_pending[line->slot] = true;
	} else {
		unsigned int tail = (server->event_head + server->event_count) % EVENT_LIMIT;
		event = &server->events[tail];
		server->event_count++;
	}
	event->type = type;
	event->line_id = line->id;
	event->status = current;
	if (!wake_signal_send(&server->wake))
		server->event_overflow = true;
}

static void synchronize(struct sippymodem_line *line)
{
	struct sippymodem_status current;
	snapshot(line, &current);
	bool changed = current.started != line->status.started || current.off_hook != line->status.off_hook ||
		current.phase != line->status.phase ||
		current.dte_mode != line->status.dte_mode || current.carrier != line->status.carrier ||
		current.ready != line->status.ready || current.modulation != line->status.modulation ||
		current.tx_bps != line->status.tx_bps || current.rx_bps != line->status.rx_bps ||
		current.error_control != line->status.error_control || current.compression != line->status.compression;
	bool connected = current.ready && !line->status.ready;
	if (changed)
		current.state_sequence++;
	line->status = current;
	if (connected) {
		line->attempt_deadline = 0;
		emit(line, SIPPYMODEM_EVENT_CONNECTED);
	} else if (changed) {
		emit(line, SIPPYMODEM_EVENT_STATE);
	}
}

static void disconnected(struct sippymodem_line *line, enum sippymodem_disconnect_cause cause)
{
	if (!line->active)
		return;
	line->active = false;
	line->remote_closing = false;
	line->attempt_deadline = 0;
	line->status.disconnect_sequence++;
	line->status.disconnect_generation = line->status.generation;
	line->status.disconnect_cause = cause;
	const char *reason = line_modem(line)->disconnect_reason;
	line->status.disconnect_local = reason != NULL ? line_modem(line)->disconnect_local :
		cause != SIPPYMODEM_DISCONNECT_REMOTE;
	if (reason == NULL)
		reason = cause == SIPPYMODEM_DISCONNECT_TIMEOUT ? "whole call attempt expired" :
			cause == SIPPYMODEM_DISCONNECT_EVENTS ? "event queue overflow" :
			cause == SIPPYMODEM_DISCONNECT_LOCAL ? "application hangup" :
			cause == SIPPYMODEM_DISCONNECT_SHUTDOWN ? "native line shutdown" :
			cause == SIPPYMODEM_DISCONNECT_RX_OVERFLOW ? "raw DTE receive overflow" :
			cause == SIPPYMODEM_DISCONNECT_MEDIA ? "native media processing failed" :
			cause == SIPPYMODEM_DISCONNECT_MODEM ? "modem call ended" : "remote SIP call ended";
	snprintf(line->status.disconnect_reason, sizeof(line->status.disconnect_reason), "%s", reason);
	line->status.state_sequence++;
	line->terminal_due = true;
}

static enum sippymodem_result end_call(struct sippymodem_line *line, enum sippymodem_disconnect_cause cause)
{
	disconnected(line, cause);
	/* Invalidate queued carrier callbacks before stopping the core/SDK call. */
	struct slmodem_calls *calls = modem_line_calls(line->core);
	calls->pending_count = 0;
	calls->overflowed = false;
	struct modem *modem = line_modem(line);
	enum sippymodem_result result = SIPPYMODEM_OK;
	modem_finish_call_report(modem, line->status.disconnect_local,
		line->status.disconnect_reason, RESULT_NOCARRIER);
	if (modem->started) {
		if (modem_stop_now(modem) != 0)
			result = SIPPYMODEM_ERROR;
		enum sip_signaling_result stopped = modem_line_last_signaling_result(line->core);
		if (stopped != SIP_SIGNALING_OK && stopped != SIP_SIGNALING_NO_CALL && stopped != SIP_SIGNALING_NOT_OPEN)
			result = SIPPYMODEM_ERROR;
	} else {
		enum sip_signaling_result hung_up = modem_host_hangup(line_host(line));
		if (hung_up != SIP_SIGNALING_OK && hung_up != SIP_SIGNALING_NO_CALL && hung_up != SIP_SIGNALING_NOT_OPEN)
			result = SIPPYMODEM_ERROR;
	}
	line->status.sip_state = SIPPYMODEM_SIP_IDLE;
	synchronize(line);
	if (line->terminal_due) {
		line->terminal_due = false;
		emit(line, SIPPYMODEM_EVENT_DISCONNECTED);
	}
	return result;
}

static bool admit(void *context)
{
	struct sippymodem_line *line = context;
	return line->started && !media_worker_busy(line->worker) && !line->stopping && !line->active && line->rx.count == 0 &&
		!line_modem(line)->started && !modem_host_media_cleanup_pending(line_host(line)) && line->status.generation != UINT64_MAX && !line->server->event_overflow;
}

static void begin_call(struct sippymodem_line *line)
{
	line->status.generation++;
	line->media_deadline = 0;
	line->finish_requested = false;
	line->finish_pending = 1;
	line->log_time = 0;
	line->log_phase[0] = line->log_step[0] = '\0';
	line->status.state_sequence++;
	line->active = true;
	line_modem(line)->disconnect_reason = NULL;
	line_modem(line)->disconnect_local = false;
	line_modem(line)->dte_overflow = false;
	line->facts = (struct sippymodem_call_facts){ .generation = line->status.generation };
	line->attempt_deadline = add_u64_saturating(
		line->server->now,
		(uint64_t)line->config.call_timeout_ms * UINT64_C(1000000));
}

static void call_changed(void *context, const struct sip_call_event *event)
{
	struct sippymodem_line *line = context;
	if (line->stopping)
		return;
	if (event->state == SIP_CALL_INCOMING_PENDING) {
		if (line->active)
			return;
		begin_call(line);
		line->incoming = true;
		line->facts.endpoint_id = event->endpoint_id;
		line->sip_generation = event->generation;
		if (event->called_number != NULL && strlen(event->called_number) < sizeof(line->facts.called_number))
			memcpy(line->facts.called_number, event->called_number,
				strlen(event->called_number) + 1);
		if (event->caller_number != NULL && strnlen(event->caller_number, sizeof(line->facts.caller_number)) < sizeof(line->facts.caller_number))
			memcpy(line->facts.caller_number, event->caller_number, strlen(event->caller_number) + 1);
	} else if (event->state == SIP_CALL_DIALING) {
		line->sip_generation = event->generation;
	} else if (!line->active || event->generation != line->sip_generation) {
		return;
	}
	static const enum sippymodem_sip_state states[] = {
		[SIP_CALL_IDLE] = SIPPYMODEM_SIP_IDLE,
		[SIP_CALL_DIALING] = SIPPYMODEM_SIP_DIALING,
		[SIP_CALL_INCOMING_PENDING] = SIPPYMODEM_SIP_INCOMING,
		[SIP_CALL_EARLY] = SIPPYMODEM_SIP_EARLY,
		[SIP_CALL_CONNECTED] = SIPPYMODEM_SIP_CONNECTED,
	};
	if ((unsigned int)event->state >= sizeof(states) / sizeof(states[0]))
		return;
	line->status.sip_state = states[event->state];
	line->status.state_sequence++;
	if (event->state == SIP_CALL_IDLE && event->timed_out)
		modem_note_disconnect(line_modem(line), true, "SIP transaction timed out");
	enum sippymodem_disconnect_cause cause = event->timed_out ? SIPPYMODEM_DISCONNECT_TIMEOUT :
		line_modem(line)->disconnect_reason != NULL && line_modem(line)->disconnect_local ?
		SIPPYMODEM_DISCONNECT_MODEM : SIPPYMODEM_DISCONNECT_REMOTE;
	if (event->state == SIP_CALL_IDLE && line_modem(line)->started &&
	    (line_modem(line)->state == STATE_MODEM_ONLINE ||
	     line_modem(line)->state == STATE_COMMAND_ONLINE)) {
		/* SIP ended; retain carrier/RX until the accepted media tail is played. */
		line->remote_closing = true;
		line->closing_cause = cause;
		return;
	}
	struct sip_call_event owned_event = *event;
	owned_event.generation = line->status.generation;
	struct slmodem_calls *calls = modem_line_calls(line->core);
	calls->callbacks.call_changed(calls, &owned_event);
	if (event->state == SIP_CALL_INCOMING_PENDING)
		emit(line, SIPPYMODEM_EVENT_INCOMING);
	else if (event->state == SIP_CALL_CONNECTED)
		emit(line, SIPPYMODEM_EVENT_SIP_ESTABLISHED);
	else if (event->state == SIP_CALL_IDLE)
		disconnected(line, cause);
}

static bool drain_received_media(void *context)
{
	const struct sippymodem_line *line = context;
	return line->active && line->remote_closing && !line->stopping;
}

void sippymodem_server_config_init(struct sippymodem_server_config *config)
{
	if (config != NULL)
		*config = (struct sippymodem_server_config){ .max_lines = 8 };
}

void sippymodem_line_config_init(struct sippymodem_line_config *config)
{
	if (config != NULL)
		*config = (struct sippymodem_line_config){ .automode = true, .digital_role = 2,
			.v91_enabled = true, .compression_types = 3, .tx_capacity = 4096,
			.rx_capacity = 16384, .frame_ms = 20, .jitter_ms = 60,
			.call_timeout_ms = 60000, .remote_clock = true };
}

static bool copy_setting(char *destination, const char *source)
{
	if (source == NULL)
		return true;
	size_t length = strnlen(source, APP_CONFIG_TEXT_MAX);
	if (length >= APP_CONFIG_TEXT_MAX)
		return false;
	memcpy(destination, source, length + 1);
	return true;
}

static bool group_locked(const struct sippymodem_huntgroup *group)
{
	for (unsigned int index = 0; index < ENDPOINT_LIMIT; ++index) {
		struct sippymodem_endpoint *endpoint = group->server->endpoints[index];
		if (endpoint != NULL && endpoint->group == group && endpoint->started)
			return true;
	}
	return false;
}

static bool endpoint_accepts_line(void *context, void *line_context)
{
	struct sippymodem_endpoint *endpoint = context;
	struct sippymodem_line *line = line_context;
	if (!endpoint->started || line == NULL || line->server != endpoint->server)
		return false;
	for (unsigned int index = 0; index < LINE_LIMIT; ++index)
		if (endpoint->group->members[index] == line)
			return true;
	return false;
}

void sippymodem_endpoint_config_init(struct sippymodem_endpoint_config *config)
{
	if (config != NULL)
		*config = (struct sippymodem_endpoint_config){ 0 };
}

struct sippymodem_endpoint *sippymodem_default_endpoint(struct sippymodem_server *server)
{
	return server != NULL ? server->default_endpoint : NULL;
}

struct sippymodem_huntgroup *sippymodem_default_huntgroup(struct sippymodem_server *server)
{
	return server != NULL ? server->default_group : NULL;
}

struct sippymodem_huntgroup *sippymodem_huntgroup_create(struct sippymodem_server *server)
{
	if (server == NULL || server->processing) {
		errno = EINVAL;
		return NULL;
	}
	unsigned int slot = 0;
	while (slot < LINE_LIMIT && server->groups[slot] != NULL)
		++slot;
	if (slot == LINE_LIMIT) {
		errno = ENOSPC;
		return NULL;
	}
	struct sippymodem_huntgroup *group = calloc(1, sizeof(*group));
	if (group != NULL) {
		group->server = server;
		server->groups[slot] = group;
	}
	return group;
}

enum sippymodem_result sippymodem_huntgroup_add(struct sippymodem_huntgroup *group,
	struct sippymodem_line *line)
{
	if (group == NULL || line == NULL || group->server != line->server || group->server->processing)
		return SIPPYMODEM_INVALID;
	if (group == group->server->default_group || group_locked(group) || line->active)
		return SIPPYMODEM_BUSY;
	for (unsigned int index = 0; index < LINE_LIMIT; ++index)
		if (group->members[index] == line)
			return SIPPYMODEM_OK;
	for (unsigned int index = 0; index < LINE_LIMIT; ++index)
		if (group->members[index] == NULL) {
			group->members[index] = line;
			return SIPPYMODEM_OK;
		}
	return SIPPYMODEM_BUSY;
}

enum sippymodem_result sippymodem_huntgroup_remove(struct sippymodem_huntgroup *group,
	struct sippymodem_line *line)
{
	if (group == NULL || line == NULL || group->server != line->server || group->server->processing)
		return SIPPYMODEM_INVALID;
	if (group == group->server->default_group || group_locked(group) || line->active)
		return SIPPYMODEM_BUSY;
	for (unsigned int index = 0; index < LINE_LIMIT; ++index)
		if (group->members[index] == line)
			group->members[index] = NULL;
	return SIPPYMODEM_OK;
}

enum sippymodem_result sippymodem_huntgroup_destroy(struct sippymodem_huntgroup *group)
{
	if (group == NULL || group->server->processing)
		return SIPPYMODEM_INVALID;
	if (group == group->server->default_group)
		return SIPPYMODEM_BUSY;
	for (unsigned int index = 0; index < ENDPOINT_LIMIT; ++index)
		if (group->server->endpoints[index] != NULL && group->server->endpoints[index]->group == group)
			return SIPPYMODEM_BUSY;
	for (unsigned int index = 0; index < LINE_LIMIT; ++index)
		if (group->server->groups[index] == group)
			group->server->groups[index] = NULL;
	free(group);
	return SIPPYMODEM_OK;
}

struct sippymodem_endpoint *sippymodem_endpoint_create(struct sippymodem_server *server,
	struct sippymodem_huntgroup *group, const struct sippymodem_endpoint_config *config)
{
	if (server == NULL || group == NULL || group->server != server || config == NULL ||
	    server->processing || config->listen_port > 65535) {
		errno = EINVAL;
		return NULL;
	}
	unsigned int slot = 0;
	while (slot < ENDPOINT_LIMIT && server->endpoints[slot] != NULL)
		++slot;
	if (slot == ENDPOINT_LIMIT) {
		errno = ENOSPC;
		return NULL;
	}
	struct sippymodem_endpoint *endpoint = calloc(1, sizeof(*endpoint));
	if (endpoint == NULL)
		return NULL;
	endpoint->server = server;
	endpoint->group = group;
	struct app_config owned = server->config;
	struct sip_config *sip = &owned.sip;
	memset(sip, 0, sizeof(*sip));
	sip->listen_port = config->listen_port;
	sip->registration_enabled = config->register_account;
	if (!copy_setting(sip->bound_address, config->bound_address) ||
	    !copy_setting(sip->public_address, config->public_address) ||
	    !copy_setting(sip->identity_uri, config->identity_uri) ||
	    !copy_setting(sip->registrar_uri, config->registrar_uri) ||
	    !copy_setting(sip->proxy_uri, config->proxy_uri) || !copy_setting(sip->user, config->username) ||
	    !copy_setting(sip->password, config->password) || !copy_setting(sip->domain, config->domain) ||
	    !copy_setting(sip->target, config->default_target))
		goto invalid;
	char request_user[APP_CONFIG_TEXT_MAX] = { 0 }, request_host[APP_CONFIG_TEXT_MAX] = { 0 };
	if (config->request_user == NULL) {
		char identity[APP_CONFIG_TEXT_MAX * 2 + 16];
		const char *configured = sip->identity_uri[0] != '\0' ? sip->identity_uri : sip->user;
		if (sip_uri_format_target(identity, sizeof(identity), configured, NULL, sip->domain) != SIP_URI_OK)
			goto invalid;
		const char *user = strchr(identity, ':') + 1;
		const char *at = strchr(user, '@');
		if (at != NULL && at != user && (size_t)(at - user) < sizeof(request_user)) {
			memcpy(request_user, user, (size_t)(at - user));
		} else if (!copy_setting(request_user, config->username) || request_user[0] == '\0') {
			goto invalid;
		}
	} else if (!copy_setting(request_user, config->request_user)) {
		goto invalid;
	}
	if (!copy_setting(request_host, config->request_host != NULL ? config->request_host : "*"))
		goto invalid;
	errno = 0;
	endpoint->sip = sip_endpoint_create(server->sip, &owned, request_user, request_host,
		endpoint_accepts_line, endpoint);
	if (endpoint->sip == NULL) {
		int error = errno != 0 ? errno : EINVAL;
		free(endpoint);
		errno = error;
		return NULL;
	}
	endpoint->id = sip_endpoint_id(endpoint->sip);
	memcpy(endpoint->target, sip->target, sizeof(endpoint->target));
	server->endpoints[slot] = endpoint;
	return endpoint;
invalid:
	free(endpoint);
	errno = EINVAL;
	return NULL;
}

enum sippymodem_result sippymodem_endpoint_start(struct sippymodem_endpoint *endpoint)
{
	if (endpoint == NULL || endpoint->server->processing)
		return SIPPYMODEM_INVALID;
	if (!endpoint->server->started)
		return SIPPYMODEM_CLOSED;
	if (endpoint->started)
		return SIPPYMODEM_OK;
	enum sip_signaling_result result = sip_endpoint_open(endpoint->sip);
	if (result != SIP_SIGNALING_OK)
		return result == SIP_SIGNALING_BUSY ? SIPPYMODEM_BUSY :
			result == SIP_SIGNALING_INVALID_ARGUMENT ? SIPPYMODEM_INVALID : SIPPYMODEM_ERROR;
	endpoint->started = true;
	return SIPPYMODEM_OK;
}

enum sippymodem_result sippymodem_endpoint_stop(struct sippymodem_endpoint *endpoint)
{
	if (endpoint == NULL || endpoint->server->processing)
		return SIPPYMODEM_INVALID;
	endpoint->started = false;
	enum sippymodem_result result = SIPPYMODEM_OK;
	for (unsigned int index = 0; index < endpoint->server->max_lines; ++index) {
		struct sippymodem_line *line = endpoint->server->lines[index];
		if (line != NULL && line->facts.endpoint_id == endpoint->id) {
			(void)collect_media(line, true);
			if (!line->active)
				continue;
			enum sippymodem_result stopped = end_call(line, SIPPYMODEM_DISCONNECT_SHUTDOWN);
			if (stopped != SIPPYMODEM_OK)
				result = stopped;
		}
	}
	if (sip_endpoint_close(endpoint->sip) != SIP_SIGNALING_OK)
		result = SIPPYMODEM_ERROR;
	return result;
}

enum sippymodem_result sippymodem_endpoint_destroy(struct sippymodem_endpoint *endpoint)
{
	if (endpoint == NULL || endpoint->server->processing)
		return SIPPYMODEM_INVALID;
	if (endpoint == endpoint->server->default_endpoint)
		return SIPPYMODEM_BUSY;
	enum sippymodem_result result = sippymodem_endpoint_stop(endpoint);
	if (result != SIPPYMODEM_OK)
		return result;
	struct sippymodem_server *server = endpoint->server;
	for (unsigned int index = 0; index < ENDPOINT_LIMIT; ++index)
		if (server->endpoints[index] == endpoint)
			server->endpoints[index] = NULL;
	sip_endpoint_destroy(endpoint->sip);
	free(endpoint);
	return SIPPYMODEM_OK;
}

unsigned int sippymodem_endpoint_id(const struct sippymodem_endpoint *endpoint)
{
	return endpoint != NULL ? endpoint->id : 0;
}

unsigned int sippymodem_endpoint_listen_port(const struct sippymodem_endpoint *endpoint)
{
	return endpoint != NULL ? sip_endpoint_local_port(endpoint->sip) : 0;
}

enum sippymodem_result sippymodem_endpoint_get_status(const struct sippymodem_endpoint *endpoint,
	struct sippymodem_endpoint_status *status)
{
	if (endpoint == NULL || status == NULL)
		return SIPPYMODEM_INVALID;
	struct sip_endpoint_status current;
	sip_endpoint_get_status(endpoint->sip, &current);
	*status = (struct sippymodem_endpoint_status){
		.endpoint_id = current.id, .listen_port = current.local_port,
		.state_sequence = current.sequence, .started = endpoint->started,
		.registration = (enum sippymodem_registration_state)current.registration_state,
		.registration_code = current.registration_code, .library_status = current.library_status,
	};
	return SIPPYMODEM_OK;
}

struct sippymodem_server *sippymodem_server_create(const struct sippymodem_server_config *config)
{
	if (config == NULL || config->max_lines == 0 || config->max_lines > LINE_LIMIT ||
	    config->listen_port > 65535 || config->dsp_threads > LINE_LIMIT) {
		errno = EINVAL;
		return NULL;
	}
	struct sippymodem_server *server = calloc(1, sizeof(*server));
	if (server == NULL)
		return NULL;
	server->wake = (struct wake_signal)WAKE_SIGNAL_INITIALIZER;
	server->max_lines = config->max_lines;
	config_init(&server->config);
	struct sip_config *sip = &server->config.sip;
	sip->listen_port = config->listen_port;
	sip->registration_enabled = config->register_account;
	sip->log_level = 0;
	if (!copy_setting(sip->bound_address, config->bound_address) ||
	    !copy_setting(sip->public_address, config->public_address) ||
	    !copy_setting(sip->identity_uri, config->identity_uri) ||
	    !copy_setting(sip->registrar_uri, config->registrar_uri) ||
	    !copy_setting(sip->proxy_uri, config->proxy_uri) ||
	    !copy_setting(sip->user, config->username) ||
	    !copy_setting(sip->password, config->password) || !copy_setting(sip->domain, config->domain) ||
	    !copy_setting(sip->target, config->default_target)) {
		errno = EINVAL;
		goto fail;
	}
	server->sip = sip_server_create(&server->config, config->max_lines);
	if (server->sip == NULL || !wake_signal_open(&server->wake))
		goto fail;
	if (config->omit_default_endpoint) {
		if (sip_server_disable_default(server->sip) != SIP_SIGNALING_OK)
			goto fail;
	} else {
		server->default_group = sippymodem_huntgroup_create(server);
		if (server->default_group == NULL)
			goto fail;
		server->default_endpoint = calloc(1, sizeof(*server->default_endpoint));
		if (server->default_endpoint == NULL)
			goto fail;
		struct sippymodem_endpoint *endpoint = server->default_endpoint;
		endpoint->server = server;
		endpoint->group = server->default_group;
		endpoint->sip = sip_server_default_endpoint(server->sip);
		endpoint->id = sip_endpoint_id(endpoint->sip);
		memcpy(endpoint->target, sip->target, sizeof(endpoint->target));
		server->endpoints[0] = endpoint;
	}
	return server;
fail:
	sippymodem_server_destroy(server);
	return NULL;
}

enum sippymodem_result sippymodem_server_start(struct sippymodem_server *server)
{
	if (server == NULL || server->processing)
		return SIPPYMODEM_INVALID;
	if (server->started)
		return SIPPYMODEM_OK;
	enum sip_signaling_result result = sip_server_open(server->sip);
	if (result != SIP_SIGNALING_OK)
		return result == SIP_SIGNALING_BUSY ? SIPPYMODEM_BUSY : SIPPYMODEM_ERROR;
	server->started = true;
	if (server->default_endpoint != NULL)
		server->default_endpoint->started = true;
	server->have_now = false;
	return SIPPYMODEM_OK;
}

struct sippymodem_line *sippymodem_line_create(struct sippymodem_server *server,
	const struct sippymodem_line_config *config)
{
	if (server == NULL || config == NULL || server->processing ||
	    config->tx_capacity < 16 || config->tx_capacity > QUEUE_LIMIT ||
	    config->rx_capacity < 16 || config->rx_capacity > QUEUE_LIMIT ||
	    config->digital_role > 2 || config->compression_types > 3 ||
	    (unsigned int)config->ec_policy > SIPPYMODEM_EC_REQUIRED || config->rtp_port > 65535 ||
	    config->frame_ms == 0 || config->frame_ms > 120 || config->jitter_ms > 10000 ||
	    config->min_bps > MODEM_MAX_RATE || config->max_bps > MODEM_MAX_RATE ||
	    (config->min_bps != 0 && config->min_bps < MODEM_MIN_RATE) ||
	    (config->max_bps != 0 && config->max_bps < MODEM_MIN_RATE) ||
	    config->call_timeout_ms < 1000 || config->call_timeout_ms > 600000 ||
	    (config->max_bps != 0 && config->min_bps > config->max_bps)) {
		errno = EINVAL;
		return NULL;
	}
	unsigned int minimum = config->min_bps ? config->min_bps : MODEM_MIN_RATE;
	unsigned int maximum = config->max_bps ? config->max_bps : MODEM_MAX_RATE;
	if (config->modulation == DP_DS0 &&
	    !((minimum <= 56000 && maximum >= 56000) || (minimum <= 64000 && maximum >= 64000))) {
		errno = EINVAL;
		return NULL;
	}
	unsigned int id = 0;
	while (id < server->max_lines && server->lines[id] != NULL)
		id++;
	if (id == server->max_lines || server->next_id == UINT_MAX) {
		errno = ENOSPC;
		return NULL;
	}
	struct sippymodem_line *line = calloc(1, sizeof(*line));
	if (line == NULL)
		return NULL;
	line->server = server;
	line->slot = id;
	line->id = server->next_id++;
	line->status.line_id = line->id;
	line->logs = (struct log_buffer){ .storage = line->log_storage, .capacity = SIPPYMODEM_LOG_CAPACITY };
	line->config = *config;
	line->config.recording_prefix = NULL;
	line->rx.capacity = config->rx_capacity;
	line->rx.storage = malloc(config->rx_capacity);
	struct app_config owned = server->config;
	owned.media.codec = config->alaw ? MEDIA_CODEC_PCMA : MEDIA_CODEC_PCMU;
	owned.media.rtp_listen_port = config->rtp_port;
	owned.media.frame_duration_ms = config->frame_ms;
	owned.media.jitter_buffer_ms = config->jitter_ms;
	owned.media.remote_clock = config->remote_clock;
	if (!copy_setting(owned.recording.prefix, config->recording_prefix))
		goto fail;
	const struct sip_signaling_callbacks callbacks = {
		.context = line, .line_owner = line, .admit = admit, .call_changed = call_changed,
		.drain_received_media = drain_received_media,
	};
	line->core = modem_line_create_shared(server->sip, &owned, "native line", &callbacks);
	if (line->rx.storage == NULL || line->core == NULL)
		goto fail;
	struct modem *modem = line_modem(line);
	if (config->modulation != 0 && (!modem_dp_is_selectable(config->modulation) ||
	    modem_dp_get_registered_ops(config->modulation) == NULL))
		goto fail;
	void *tx = malloc(config->tx_capacity);
	if (tx == NULL)
		goto fail;
	free(modem->xmit.buf);
	modem->xmit.buf = tx;
	modem->xmit.size = (unsigned int)config->tx_capacity;
	modem->sregs[SREG_DP] = (unsigned char)(config->modulation != 0 ? config->modulation : DP_V92);
	modem->sregs[SREG_AUTOMODE] = config->automode;
	modem->sregs[SREG_V90_DIGITAL_INDICATION] = (unsigned char)config->digital_role;
	modem->sregs[SREG_V92_PCM_UPSTREAM] = config->pcm_upstream;
	modem->sregs[SREG_V91_ENABLED] = config->v91_enabled;
	modem->sregs[SREG_DS0_FRAMING] = config->ds0_lapm;
	modem->sregs[SREG_EC] = config->ec_policy == SIPPYMODEM_EC_RAW ? 0 :
		config->ec_policy == SIPPYMODEM_EC_REQUIRED ? 2 : 1;
	modem->sregs[SREG_COMP] = config->compression_types != 0 ? 3 : 0;
	modem->sregs[SREG_COMP_TYPES] = (unsigned char)config->compression_types;
	if (config->min_bps != 0)
		modem->min_rate = config->min_bps;
	if (config->max_bps != 0)
		modem->max_rate = config->max_bps;
	modem->data_output_context = &line->rx;
	modem->data_output = byte_queue_output;
	line->status.dte_mode = SIPPYMODEM_COMMAND;
	server->lines[id] = line;
	if (server->default_group != NULL)
		server->default_group->members[id] = line;
	return line;
fail:
	modem_line_destroy(line->core);
	free(line->rx.storage);
	free(line);
	return NULL;
}

enum sippymodem_result sippymodem_line_start(struct sippymodem_line *line)
{
	if (line == NULL || line->server->processing)
		return SIPPYMODEM_INVALID;
	if (line->started)
		return SIPPYMODEM_OK;
	if (!line->server->started)
		return SIPPYMODEM_CLOSED;
	if (modem_host_open(line_host(line)) != SIP_SESSION_OK)
		return SIPPYMODEM_ERROR;
	line->worker = media_worker_create(line->core, &line->server->wake);
	if (line->worker == NULL) {
		(void)modem_host_close(line_host(line));
		return SIPPYMODEM_ERROR;
	}
	line->media_deadline = 0;
	line->started = true;
	synchronize(line);
	return SIPPYMODEM_OK;
}

enum sippymodem_result sippymodem_line_stop(struct sippymodem_line *line)
{
	if (line == NULL || line->server->processing)
		return SIPPYMODEM_INVALID;
	if (!line->started)
		return SIPPYMODEM_OK;
	(void)collect_media(line, true);
	media_worker_destroy(line->worker);
	line->worker = NULL;
	line->stopping = true;
	enum sippymodem_result result = end_call(line, SIPPYMODEM_DISCONNECT_SHUTDOWN);
	enum sip_session_result closed = modem_host_close(line_host(line));
	if (closed != SIP_SESSION_OK && closed != SIP_SESSION_NOT_OPEN)
		result = SIPPYMODEM_ERROR;
	line->started = false;
	line->stopping = false;
	synchronize(line);
	return result;
}

void sippymodem_line_destroy(struct sippymodem_line *line)
{
	if (line == NULL || line->server->processing)
		return;
	(void)sippymodem_line_stop(line);
	for (unsigned int index = 0; index < LINE_LIMIT; ++index) {
		struct sippymodem_huntgroup *group = line->server->groups[index];
		if (group != NULL)
			for (unsigned int member = 0; member < LINE_LIMIT; ++member)
				if (group->members[member] == line)
					group->members[member] = NULL;
	}
	line->server->lines[line->slot] = NULL;
	modem_line_destroy(line->core);
	free(line->rx.storage);
	free(line);
}

enum sippymodem_result sippymodem_server_stop(struct sippymodem_server *server)
{
	if (server == NULL || server->processing)
		return SIPPYMODEM_INVALID;
	enum sippymodem_result result = SIPPYMODEM_OK;
	for (unsigned int id = 0; id < server->max_lines; ++id)
		if (server->lines[id] != NULL) {
			enum sippymodem_result stopped = sippymodem_line_stop(server->lines[id]);
			if (result == SIPPYMODEM_OK && stopped != SIPPYMODEM_OK)
				result = stopped;
		}
	if (sip_server_close(server->sip) != SIP_SIGNALING_OK && result == SIPPYMODEM_OK)
		result = SIPPYMODEM_ERROR;
	for (unsigned int index = 0; index < ENDPOINT_LIMIT; ++index)
		if (server->endpoints[index] != NULL)
			server->endpoints[index]->started = false;
	server->started = false;
	return result;
}

void sippymodem_server_destroy(struct sippymodem_server *server)
{
	if (server == NULL || server->processing)
		return;
	(void)sippymodem_server_stop(server);
	for (unsigned int id = 0; id < server->max_lines; ++id)
		sippymodem_line_destroy(server->lines[id]);
	sip_server_destroy(server->sip);
	for (unsigned int index = 0; index < ENDPOINT_LIMIT; ++index)
		free(server->endpoints[index]);
	for (unsigned int index = 0; index < LINE_LIMIT; ++index)
		free(server->groups[index]);
#ifdef _WIN32
	if (server->wait_timer != NULL)
		CloseHandle(server->wait_timer);
#endif
	wake_signal_close(&server->wake);
	free(server);
}

enum sippymodem_result sippymodem_send(struct sippymodem_line *line,
	const void *bytes, size_t length, size_t *accepted)
{
	if (accepted != NULL)
		*accepted = 0;
	if (line == NULL || accepted == NULL || (length != 0 && bytes == NULL) || length > INT_MAX)
		return SIPPYMODEM_INVALID;
	if (length == 0)
		return SIPPYMODEM_OK;
	(void)collect_media(line, false);
	if (!line->started || !line->active || line->remote_closing)
		return SIPPYMODEM_CLOSED;
	if (media_worker_busy(line->worker) || line->finish_requested)
		return SIPPYMODEM_AGAIN;
	struct modem *modem = line_modem(line);
	if (modem->command || (modem->state != STATE_MODEM_ONLINE && modem->state != STATE_COMMAND_ONLINE))
		return SIPPYMODEM_AGAIN;
	int count = modem_data_write(modem, bytes, length);
	if (count < 0)
		return SIPPYMODEM_ERROR;
	*accepted = (size_t)count;
	snapshot(line, &line->status);
	return count == 0 ? SIPPYMODEM_AGAIN : SIPPYMODEM_OK;
}

enum sippymodem_result sippymodem_receive(struct sippymodem_line *line,
	void *bytes, size_t capacity, size_t *received)
{
	if (received != NULL)
		*received = 0;
	if (line == NULL || received == NULL || (capacity != 0 && bytes == NULL))
		return SIPPYMODEM_INVALID;
	if (capacity == 0)
		return SIPPYMODEM_OK;
	(void)collect_media(line, false);
	if (media_worker_busy(line->worker))
		return SIPPYMODEM_AGAIN;
	*received = byte_queue_read(&line->rx, bytes, capacity);
	snapshot(line, &line->status);
	if (*received != 0)
		return SIPPYMODEM_OK;
	return line->active ? SIPPYMODEM_AGAIN : SIPPYMODEM_CLOSED;
}

enum sippymodem_result sippymodem_get_status(const struct sippymodem_line *line,
	struct sippymodem_status *status)
{
	if (line == NULL || status == NULL)
		return SIPPYMODEM_INVALID;
	if (media_worker_busy(line->worker))
		*status = line->status;
	else
		snapshot(line, status);
	return SIPPYMODEM_OK;
}

enum sippymodem_result sippymodem_read_log(const struct sippymodem_line *line,
	uint64_t *cursor, struct sippymodem_log_record *records, size_t capacity,
	size_t *count, uint64_t *missed)
{
	return log_buffer_read(line != NULL ? &line->logs : NULL,
		cursor, records, capacity, count, missed);
}

enum sippymodem_result sippymodem_get_call_facts(const struct sippymodem_line *line,
	struct sippymodem_call_facts *facts)
{
	if (line == NULL || facts == NULL)
		return SIPPYMODEM_INVALID;
	*facts = line->facts;
	return SIPPYMODEM_OK;
}

static enum sippymodem_result check_call(struct sippymodem_line *line, uint64_t generation)
{
	if (line == NULL || line->server->processing)
		return SIPPYMODEM_INVALID;
	if (generation != line->status.generation)
		return SIPPYMODEM_STALE;
	return line->started && line->active ? SIPPYMODEM_OK : SIPPYMODEM_CLOSED;
}

enum sippymodem_result sippymodem_finish_tx(struct sippymodem_line *line,
	uint64_t generation)
{
	enum sippymodem_result result = check_call(line, generation);
	if (result != SIPPYMODEM_OK)
		return result;
	(void)collect_media(line, false);
	result = check_call(line, generation);
	if (result != SIPPYMODEM_OK)
		return result;
	if (line->remote_closing)
		return SIPPYMODEM_CLOSED;
	if (!line->status.ready)
		return line->status.phase == SIPPYMODEM_DISCONNECTING ? SIPPYMODEM_CLOSED : SIPPYMODEM_AGAIN;
	line->finish_requested = true;
	if (!media_worker_busy(line->worker))
		line->finish_pending = modem_finish_transmit(line_modem(line));
	return line->finish_pending < 0 ? SIPPYMODEM_ERROR :
		line->finish_pending == 0 ? SIPPYMODEM_OK : SIPPYMODEM_AGAIN;
}

enum sippymodem_result sippymodem_set_dte_mode(struct sippymodem_line *line,
	uint64_t generation, enum sippymodem_dte_mode mode)
{
	enum sippymodem_result result = check_call(line, generation);
	if (result != SIPPYMODEM_OK)
		return result;
	if (mode != SIPPYMODEM_COMMAND && mode != SIPPYMODEM_DATA)
		return SIPPYMODEM_INVALID;
	(void)collect_media(line, true);
	result = check_call(line, generation);
	if (result != SIPPYMODEM_OK)
		return result;
	struct modem *modem = line_modem(line);
	if (modem->xmit.count != 0)
		return SIPPYMODEM_BUSY;
	if (modem_set_dte_command(modem, mode == SIPPYMODEM_COMMAND) != 0)
		return SIPPYMODEM_AGAIN;
	synchronize(line);
	return SIPPYMODEM_OK;
}

enum sippymodem_result sippymodem_dial_from(struct sippymodem_line *line,
	struct sippymodem_endpoint *endpoint, const char *target, uint64_t *generation)
{
	if (line == NULL || endpoint == NULL || line->server != endpoint->server || generation == NULL || line->server->processing)
		return SIPPYMODEM_INVALID;
	if (!line->started || !endpoint->started)
		return SIPPYMODEM_CLOSED;
	if (!admit(line) || !line->server->have_now)
		return SIPPYMODEM_BUSY;
	struct modem *modem = line_modem(line);
	const char *destination = target != NULL ? target : endpoint->target;
	size_t length = strnlen(destination, sizeof(modem->dial_string));
	if (length == 0 || length >= sizeof(modem->dial_string))
		return SIPPYMODEM_INVALID;
	if (modem_host_set_endpoint(line_host(line), endpoint->sip) != SIP_SIGNALING_OK)
		return SIPPYMODEM_BUSY;
	memcpy(modem->dial_string, destination, length + 1);
	begin_call(line);
	line->facts.endpoint_id = endpoint->id;
	line->incoming = false;
	*generation = line->status.generation;
	if (modem_dial(modem) != 0) {
		end_call(line, SIPPYMODEM_DISCONNECT_MODEM);
		return SIPPYMODEM_ERROR;
	}
	synchronize(line);
	return SIPPYMODEM_OK;
}

enum sippymodem_result sippymodem_dial(struct sippymodem_line *line,
	const char *target, uint64_t *generation)
{
	return line == NULL ? SIPPYMODEM_INVALID :
		line->server->default_endpoint == NULL ? SIPPYMODEM_CLOSED :
		sippymodem_dial_from(line, line->server->default_endpoint, target, generation);
}

enum sippymodem_result sippymodem_answer(struct sippymodem_line *line, uint64_t generation)
{
	enum sippymodem_result result = check_call(line, generation);
	if (result != SIPPYMODEM_OK)
		return result;
	(void)collect_media(line, true);
	result = check_call(line, generation);
	if (result != SIPPYMODEM_OK)
		return result;
	line->media_deadline = 0;
	if (line_modem(line)->started ||
	    (line->status.sip_state != SIPPYMODEM_SIP_INCOMING && line->status.sip_state != SIPPYMODEM_SIP_EARLY))
		return SIPPYMODEM_BUSY;
	return modem_answer(line_modem(line)) == 0 ? SIPPYMODEM_OK : SIPPYMODEM_ERROR;
}

enum sippymodem_result sippymodem_hangup(struct sippymodem_line *line, uint64_t generation)
{
	enum sippymodem_result result = check_call(line, generation);
	if (result != SIPPYMODEM_OK)
		return result;
	(void)collect_media(line, true);
	result = check_call(line, generation);
	return result != SIPPYMODEM_OK ? result : end_call(line, SIPPYMODEM_DISCONNECT_LOCAL);
}

/* Collect only this line. Normal process and byte I/O never wait; explicit
 * controls/lifecycle can finish the target's current bounded job. */
static bool collect_media(struct sippymodem_line *line, bool wait)
{
	struct media_job job;
	if (!media_worker_collect(line->worker, wait, &job))
		return !media_worker_busy(line->worker);
	if (!job.notified || !wake_signal_take(&line->server->wake))
		line->server->event_overflow = true;
	if (modem_host_finish_media(line_host(line)) != SIP_SESSION_OK)
		job.result = MODEM_HOST_SESSION_ERROR;
	(void)modem_line_flush_signaling(line->core);
	if (line->remote_closing && !modem_host_media_is_configured(line_host(line)))
		end_call(line, line->closing_cause);
	struct modem *modem = line_modem(line);
	if (line->config.ec_policy == SIPPYMODEM_EC_REQUIRED &&
	    !(modem->dp != NULL && modem->dp->id == DP_DS0 && !line->config.ds0_lapm) &&
	    line->active && (modem->state == STATE_MODEM_ONLINE || modem->state == STATE_COMMAND_ONLINE) &&
	    !modem->cfg.ec)
		end_call(line, SIPPYMODEM_DISCONNECT_MODEM);
	if (line->active && modem->dte_overflow)
		end_call(line, SIPPYMODEM_DISCONNECT_RX_OVERFLOW);
	else if (job.result != MODEM_HOST_OK && job.result != MODEM_HOST_IDLE && line->active)
		end_call(line, job.result == MODEM_HOST_MODEM_STOPPED ?
			SIPPYMODEM_DISCONNECT_MODEM : SIPPYMODEM_DISCONNECT_MEDIA);
	else if (line->active && line->status.sip_state == SIPPYMODEM_SIP_CONNECTED && !modem->started)
		end_call(line, SIPPYMODEM_DISCONNECT_MODEM);
	synchronize(line);
	log_progress(line);
	if (line->finish_requested && line->status.ready)
		line->finish_pending = modem_finish_transmit(modem);
	if (line->terminal_due && !modem->started) {
		line->terminal_due = false;
		emit(line, SIPPYMODEM_EVENT_DISCONNECTED);
	}
	line->media_deadline = job.datagrams >= RTP_ENDPOINT_MAX_DRAIN ? line->server->now :
		add_u64_saturating(line->server->now,
			modem_host_wait_ns(line_host(line), line->server->now));
	return true;
}

static bool media_read_ready(const struct sippymodem_line *line)
{
#ifdef _WIN32
	HANDLE event = modem_host_media_read_handle(line_host(line));
	/* recv() can leave a read notification after the last datagram. Service
	 * it even on an empty socket; the receive path acknowledges it. */
	if (event != NULL && WaitForSingleObject(event, 0) == WAIT_OBJECT_0)
		return true;
#endif
	return net_socket_readable(modem_host_media_poll_fd(line_host(line)));
}

enum sippymodem_result sippymodem_process(struct sippymodem_server *server,
	uint64_t now_ns, uint64_t *next_deadline_ns)
{
	if (next_deadline_ns != NULL)
		*next_deadline_ns = UINT64_MAX;
	if (server == NULL || next_deadline_ns == NULL || server->processing ||
	    (server->time_valid && now_ns < server->now))
		return SIPPYMODEM_INVALID;
	server->now = now_ns;
	server->have_now = true;
	server->time_valid = true;
	if (!server->started)
		return SIPPYMODEM_CLOSED;
	server->processing = true;
	for (unsigned int id = 0; id < server->max_lines; ++id)
		if (server->lines[id] != NULL)
			(void)collect_media(server->lines[id], false);
	enum sip_signaling_result sip_result = sip_server_step(server->sip, 0);
	uint64_t next = add_u64_saturating(now_ns, MODEM_HOST_SIGNALING_POLL_NS);
	for (unsigned int id = 0; id < server->max_lines; ++id) {
		struct sippymodem_line *line = server->lines[id];
		if (line == NULL || !line->started || media_worker_busy(line->worker))
			continue;
		struct slmodem_calls *calls = modem_line_calls(line->core);
		if (server->event_overflow || calls->overflowed)
			end_call(line, SIPPYMODEM_DISCONNECT_EVENTS);
		for (unsigned int transition = 0; transition < SLMODEM_CALL_EVENT_CAPACITY; ++transition) {
			enum slmodem_calls_result result = slmodem_calls_drain(calls);
			if (result == SLMODEM_CALLS_IDLE)
				break;
			if (result != SLMODEM_CALLS_OK) {
				end_call(line, SIPPYMODEM_DISCONNECT_MODEM);
				break;
			}
		}
		if (line->active && line->attempt_deadline != 0 && now_ns >= line->attempt_deadline)
			end_call(line, SIPPYMODEM_DISCONNECT_TIMEOUT);
		if (now_ns >= line->media_deadline || media_read_ready(line)) {
			(void)modem_host_prepare_media(line_host(line));
			synchronize(line);
			if (line_modem(line)->started) {
				if (!media_worker_submit(line->worker, now_ns)) {
					(void)modem_host_finish_media(line_host(line));
					server->processing = false;
					return SIPPYMODEM_ERROR;
				}
			} else {
				(void)modem_host_finish_media(line_host(line));
				line->media_deadline = add_u64_saturating(
					now_ns, MODEM_HOST_SIGNALING_POLL_NS);
			}
		}
		if (!media_worker_busy(line->worker) && line->media_deadline < next)
			next = line->media_deadline;
		if (line->attempt_deadline != 0 && line->attempt_deadline < next)
			next = line->attempt_deadline;
	}
	server->processing = false;
	server->next_deadline = next;
	*next_deadline_ns = next;
	return server->event_overflow ? SIPPYMODEM_OVERFLOW :
		sip_result == SIP_SIGNALING_OK ? SIPPYMODEM_OK : SIPPYMODEM_ERROR;
}

enum sippymodem_result sippymodem_wait(struct sippymodem_server *server,
	uint64_t now_ns, unsigned int timeout_ms)
{
	if (server == NULL || server->processing ||
	    (server->time_valid && now_ns < server->now))
		return SIPPYMODEM_INVALID;
	if (!server->started)
		return SIPPYMODEM_CLOSED;
	if (!server->have_now)
		return SIPPYMODEM_INVALID;
	if (now_ns >= server->next_deadline)
		return SIPPYMODEM_OK;
	uint64_t until_due = server->next_deadline - now_ns;
	uint64_t budget = (uint64_t)timeout_ms * UINT64_C(1000000);
	bool deadline_due = until_due <= budget;
	if (deadline_due)
		budget = until_due;
	/* process() always schedules SIP within 20 ms, so this cast is bounded.
	 * Round up by less than 1 ms to avoid spinning on a sub-millisecond tail. */
	unsigned int wait_ms = (unsigned int)((budget + 999999) / 1000000);
#ifdef _WIN32
	_Static_assert(LINE_LIMIT + 2 <= MAXIMUM_WAIT_OBJECTS, "native wait capacity");
	HANDLE handles[LINE_LIMIT + 2] = { wake_signal_handle(&server->wake) };
	unsigned int count = 1;
#else
	struct pollfd handles[LINE_LIMIT + 1] = {
		{ .fd = wake_signal_fd(&server->wake), .events = POLLIN }
	};
	unsigned int count = 1;
#endif
	for (unsigned int id = 0; id < server->max_lines; ++id) {
		struct sippymodem_line *line = server->lines[id];
		if (line == NULL || !line->started || media_worker_busy(line->worker))
			continue;
#ifdef _WIN32
		HANDLE handle = modem_host_media_read_handle(line_host(line));
		if (handle != NULL)
			handles[count++] = handle;
#else
		net_socket socket = modem_host_media_poll_fd(line_host(line));
		if (socket != NET_INVALID_SOCKET)
			handles[count++] = (struct pollfd){ .fd = socket, .events = POLLIN };
#endif
	}
#ifdef _WIN32
	unsigned int input_count = count;
	if (budget != 0) {
		if (server->wait_timer == NULL) {
			server->wait_timer = CreateWaitableTimerExW(NULL, NULL,
				CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
			if (server->wait_timer == NULL && GetLastError() == ERROR_INVALID_PARAMETER)
				server->wait_timer = CreateWaitableTimerExW(NULL, NULL, 0,
					TIMER_MODIFY_STATE | SYNCHRONIZE);
			if (server->wait_timer == NULL)
				return SIPPYMODEM_ERROR;
		}
		LARGE_INTEGER due = { .QuadPart = -(LONGLONG)((budget + 99) / 100) };
		if (!SetWaitableTimer(server->wait_timer, &due, 0, NULL, NULL, FALSE))
			return SIPPYMODEM_ERROR;
		handles[count++] = server->wait_timer;
	}
	DWORD result = WaitForMultipleObjects(count, handles, FALSE, wait_ms);
	DWORD error = result == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
	if (budget != 0 && !CancelWaitableTimer(server->wait_timer))
		return SIPPYMODEM_ERROR;
	if (result < WAIT_OBJECT_0 + input_count)
		return SIPPYMODEM_OK;
	if (result != WAIT_OBJECT_0 + input_count && result != WAIT_TIMEOUT) {
		SetLastError(error);
		return SIPPYMODEM_ERROR;
	}
#else
	int result = poll(handles, count, (int)wait_ms);
	if (result > 0)
		return SIPPYMODEM_OK;
	if (result < 0)
		return errno == EINTR ? SIPPYMODEM_AGAIN : SIPPYMODEM_ERROR;
#endif
	return deadline_due ? SIPPYMODEM_OK : SIPPYMODEM_AGAIN;
}

enum sippymodem_result sippymodem_poll_event(struct sippymodem_server *server,
	struct sippymodem_event *event)
{
	if (server == NULL || event == NULL)
		return SIPPYMODEM_INVALID;
	if (server->event_count != 0) {
		*event = server->events[server->event_head];
		server->event_head = (server->event_head + 1) % EVENT_LIMIT;
		server->event_count--;
	} else {
		unsigned int slot = 0;
		while (slot < server->max_lines && !server->terminal_pending[slot])
			slot++;
		if (slot == server->max_lines)
			return server->event_overflow ? SIPPYMODEM_OVERFLOW : SIPPYMODEM_AGAIN;
		*event = server->terminal_events[slot];
		server->terminal_pending[slot] = false;
	}
	if (!wake_signal_take(&server->wake))
		server->event_overflow = true;
	return SIPPYMODEM_OK;
}

int sippymodem_event_fd(const struct sippymodem_server *server)
{
	return server != NULL ? wake_signal_fd(&server->wake) : -1;
}

void *sippymodem_event_handle(const struct sippymodem_server *server)
{
	return server != NULL ? wake_signal_handle(&server->wake) : NULL;
}

unsigned int sippymodem_listen_port(const struct sippymodem_server *server)
{
	return server != NULL ? sip_server_local_port(server->sip) : 0;
}
