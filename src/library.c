// SPDX-License-Identifier: BSD-3-Clause
#include "sippymodem.h"

#include "byte_queue.h"
#include "datapump_link.h"
#include "line.h"
#include "modem_state.h"

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <unistd.h>

enum { LINE_LIMIT = 32, EVENT_LIMIT = 128, QUEUE_LIMIT = 1048576 };
struct sippymodem_line {
	struct sippymodem_server *server;
	struct modem_line *core;
	struct sippymodem_line_config config;
	struct sippymodem_status status;
	struct sippymodem_call_facts facts;
	struct byte_queue rx;
	unsigned int id, slot;
	uint64_t sip_generation;
	uint64_t attempt_deadline;
	bool active;
	bool terminal_due;
	bool incoming;
	bool started;
	bool stopping;
};

struct sippymodem_server {
	struct app_config config;
	struct sip_server *sip;
	struct sippymodem_line *lines[LINE_LIMIT];
	struct sippymodem_event events[EVENT_LIMIT];
	unsigned int max_lines, event_head, event_count, next_id;
	struct sippymodem_event terminal_events[LINE_LIMIT];
	bool terminal_pending[LINE_LIMIT];
	int event_fd;
	uint64_t now;
	bool have_now, time_valid, started, processing, event_overflow;
};

static uint64_t deadline_after(uint64_t now, uint64_t delay)
{
	return delay > UINT64_MAX - now ? UINT64_MAX : now + delay;
}

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
	status->carrier = (modem->modem_info & TIOCM_CD) != 0;
	status->ready = line->active && (modem->state == STATE_MODEM_ONLINE || modem->state == STATE_COMMAND_ONLINE);
	status->dte_mode = modem->command ? SIPPYMODEM_COMMAND : SIPPYMODEM_DATA;
	status->phase = modem->started ? SIPPYMODEM_DISCONNECTING : SIPPYMODEM_IDLE;
	if (line->active) {
		if (status->ready)
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
	status->modulation = status->ready && modem->dp != NULL ? (unsigned int)modem->dp->id : 0;
	status->error_control = status->ready && modem->cfg.ec ? SIPPYMODEM_LAPM : SIPPYMODEM_RAW;
	status->compression = SIPPYMODEM_COMP_NONE;
	if (status->ready) {
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

static void emit(struct sippymodem_line *line, enum sippymodem_event_type type)
{
	struct sippymodem_server *server = line->server;
	struct sippymodem_event *event;
	if (server->event_count == EVENT_LIMIT) {
		server->event_overflow = true;
		if (type != SIPPYMODEM_EVENT_DISCONNECTED)
			return;
		event = &server->terminal_events[line->slot];
		if (server->terminal_pending[line->slot]) {
			event->type = type;
			event->line_id = line->id;
			snapshot(line, &event->status);
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
	snapshot(line, &event->status);
	uint64_t one = 1;
	ssize_t written = write(server->event_fd, &one, sizeof(one));
	if (written != sizeof(one))
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
	return line->started && !line->stopping && !line->active && line->rx.count == 0 &&
		!line_modem(line)->started && !modem_host_media_cleanup_pending(line_host(line)) && line->status.generation != UINT64_MAX && !line->server->event_overflow;
}

static void begin_call(struct sippymodem_line *line)
{
	line->status.generation++;
	line->status.state_sequence++;
	line->active = true;
	line_modem(line)->disconnect_reason = NULL;
	line_modem(line)->disconnect_local = false;
	line_modem(line)->dte_overflow = false;
	line->facts = (struct sippymodem_call_facts){ .generation = line->status.generation };
	line->attempt_deadline = deadline_after(line->server->now,
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
	struct sip_call_event owned_event = *event;
	owned_event.generation = line->status.generation;
	struct slmodem_calls *calls = modem_line_calls(line->core);
	calls->callbacks.call_changed(calls, &owned_event);
	if (event->state == SIP_CALL_INCOMING_PENDING)
		emit(line, SIPPYMODEM_EVENT_INCOMING);
	else if (event->state == SIP_CALL_CONNECTED)
		emit(line, SIPPYMODEM_EVENT_SIP_ESTABLISHED);
	else if (event->state == SIP_CALL_IDLE)
		disconnected(line, line_modem(line)->disconnect_reason != NULL && line_modem(line)->disconnect_local ?
			SIPPYMODEM_DISCONNECT_MODEM : SIPPYMODEM_DISCONNECT_REMOTE);
}

static int receive_output(void *context, const char *bytes, int count)
{
	struct sippymodem_line *line = context;
	return count <= 0 ? 0 : (int)byte_queue_write(&line->rx, bytes, (size_t)count);
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

struct sippymodem_server *sippymodem_server_create(const struct sippymodem_server_config *config)
{
	if (config == NULL || config->max_lines == 0 || config->max_lines > LINE_LIMIT ||
	    config->listen_port > 65535) {
		errno = EINVAL;
		return NULL;
	}
	struct sippymodem_server *server = calloc(1, sizeof(*server));
	if (server == NULL)
		return NULL;
	server->event_fd = -1;
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
	server->event_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE);
	if (server->sip == NULL || server->event_fd < 0)
		goto fail;
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
		.context = line, .admit = admit, .call_changed = call_changed,
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
	modem->sregs[SREG_EC] = config->ec_policy == SIPPYMODEM_EC_RAW ? 0 :
		config->ec_policy == SIPPYMODEM_EC_REQUIRED ? 2 : 1;
	modem->sregs[SREG_COMP] = config->compression_types != 0 ? 3 : 0;
	modem->sregs[SREG_COMP_TYPES] = (unsigned char)config->compression_types;
	if (config->min_bps != 0)
		modem->min_rate = config->min_bps;
	if (config->max_bps != 0)
		modem->max_rate = config->max_bps;
	modem->data_output_context = line;
	modem->data_output = receive_output;
	line->status.dte_mode = SIPPYMODEM_COMMAND;
	server->lines[id] = line;
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
	if (server->event_fd >= 0)
		close(server->event_fd);
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
	if (!line->started || !line->active)
		return SIPPYMODEM_CLOSED;
	struct modem *modem = line_modem(line);
	if (modem->command || (modem->state != STATE_MODEM_ONLINE && modem->state != STATE_COMMAND_ONLINE))
		return SIPPYMODEM_AGAIN;
	int count = modem_data_write(modem, bytes, length);
	if (count < 0)
		return SIPPYMODEM_ERROR;
	*accepted = (size_t)count;
	return count == 0 ? SIPPYMODEM_AGAIN : SIPPYMODEM_OK;
}

enum sippymodem_result sippymodem_receive(struct sippymodem_line *line,
	void *bytes, size_t capacity, size_t *received)
{
	if (received != NULL)
		*received = 0;
	if (line == NULL || received == NULL || (capacity != 0 && bytes == NULL))
		return SIPPYMODEM_INVALID;
	*received = byte_queue_read(&line->rx, bytes, capacity);
	if (*received != 0 || capacity == 0)
		return SIPPYMODEM_OK;
	return line->active ? SIPPYMODEM_AGAIN : SIPPYMODEM_CLOSED;
}

enum sippymodem_result sippymodem_get_status(const struct sippymodem_line *line,
	struct sippymodem_status *status)
{
	if (line == NULL || status == NULL)
		return SIPPYMODEM_INVALID;
	snapshot(line, status);
	return SIPPYMODEM_OK;
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

enum sippymodem_result sippymodem_set_dte_mode(struct sippymodem_line *line,
	uint64_t generation, enum sippymodem_dte_mode mode)
{
	enum sippymodem_result result = check_call(line, generation);
	if (result != SIPPYMODEM_OK)
		return result;
	if (mode != SIPPYMODEM_COMMAND && mode != SIPPYMODEM_DATA)
		return SIPPYMODEM_INVALID;
	struct modem *modem = line_modem(line);
	if (modem->xmit.count != 0)
		return SIPPYMODEM_BUSY;
	if (modem_set_dte_command(modem, mode == SIPPYMODEM_COMMAND) != 0)
		return SIPPYMODEM_AGAIN;
	synchronize(line);
	return SIPPYMODEM_OK;
}

enum sippymodem_result sippymodem_dial(struct sippymodem_line *line,
	const char *target, uint64_t *generation)
{
	if (line == NULL || generation == NULL || line->server->processing)
		return SIPPYMODEM_INVALID;
	if (!line->started)
		return SIPPYMODEM_CLOSED;
	if (line->active || line->rx.count != 0 || line_modem(line)->started || modem_host_media_cleanup_pending(line_host(line)) || !line->server->have_now || line->status.generation == UINT64_MAX || line->server->event_overflow)
		return SIPPYMODEM_BUSY;
	struct modem *modem = line_modem(line);
	const char *destination = target != NULL ? target : line->server->config.sip.target;
	size_t length = strnlen(destination, sizeof(modem->dial_string));
	if (length == 0 || length >= sizeof(modem->dial_string))
		return SIPPYMODEM_INVALID;
	memcpy(modem->dial_string, destination, length + 1);
	begin_call(line);
	line->incoming = false;
	*generation = line->status.generation;
	if (modem_dial(modem) != 0) {
		end_call(line, SIPPYMODEM_DISCONNECT_MODEM);
		return SIPPYMODEM_ERROR;
	}
	synchronize(line);
	return SIPPYMODEM_OK;
}

enum sippymodem_result sippymodem_answer(struct sippymodem_line *line, uint64_t generation)
{
	enum sippymodem_result result = check_call(line, generation);
	if (result != SIPPYMODEM_OK)
		return result;
	if (line->status.sip_state != SIPPYMODEM_SIP_INCOMING && line->status.sip_state != SIPPYMODEM_SIP_EARLY)
		return SIPPYMODEM_BUSY;
	return modem_answer(line_modem(line)) == 0 ? SIPPYMODEM_OK : SIPPYMODEM_ERROR;
}

enum sippymodem_result sippymodem_hangup(struct sippymodem_line *line, uint64_t generation)
{
	enum sippymodem_result result = check_call(line, generation);
	if (result != SIPPYMODEM_OK)
		return result;
	return end_call(line, SIPPYMODEM_DISCONNECT_LOCAL);
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
	enum sip_signaling_result sip_result = sip_server_step(server->sip, 0);
	uint64_t next = deadline_after(now_ns, MODEM_HOST_SIGNALING_POLL_NS);
	for (unsigned int id = 0; id < server->max_lines; ++id) {
		struct sippymodem_line *line = server->lines[id];
		if (line == NULL || !line->started)
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
		size_t datagrams = 0;
		enum modem_host_result result = modem_host_step(line_host(line), now_ns, &datagrams, NULL);
		if (datagrams >= RTP_ENDPOINT_MAX_DRAIN)
			next = now_ns;
		(void)modem_service_event(line_modem(line));
		if (line->config.ec_policy == SIPPYMODEM_EC_REQUIRED && line->active &&
		    (line_modem(line)->state == STATE_MODEM_ONLINE || line_modem(line)->state == STATE_COMMAND_ONLINE) &&
		    !line_modem(line)->cfg.ec)
			end_call(line, SIPPYMODEM_DISCONNECT_MODEM);
		if (line->active && line_modem(line)->dte_overflow)
			end_call(line, SIPPYMODEM_DISCONNECT_RX_OVERFLOW);
		else if (result != MODEM_HOST_OK && result != MODEM_HOST_IDLE && line->active)
			end_call(line, result == MODEM_HOST_MODEM_STOPPED ?
				SIPPYMODEM_DISCONNECT_MODEM : SIPPYMODEM_DISCONNECT_MEDIA);
		else if (line->active && line->status.sip_state == SIPPYMODEM_SIP_CONNECTED &&
			 !line_modem(line)->started)
			end_call(line, SIPPYMODEM_DISCONNECT_MODEM);
		synchronize(line);
		if (line->terminal_due && !line_modem(line)->started) {
			line->terminal_due = false;
			emit(line, SIPPYMODEM_EVENT_DISCONNECTED);
		}
		uint64_t deadline = deadline_after(now_ns, modem_host_wait_ns(line_host(line), now_ns));
		if (line->attempt_deadline != 0 && line->attempt_deadline < deadline)
			deadline = line->attempt_deadline;
		if (deadline < next)
			next = deadline;
	}
	server->processing = false;
	*next_deadline_ns = next;
	return server->event_overflow ? SIPPYMODEM_OVERFLOW :
		sip_result == SIP_SIGNALING_OK ? SIPPYMODEM_OK : SIPPYMODEM_ERROR;
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
	uint64_t one;
	ssize_t received = read(server->event_fd, &one, sizeof(one));
	if (received != sizeof(one))
		server->event_overflow = true;
	return SIPPYMODEM_OK;
}

int sippymodem_event_fd(const struct sippymodem_server *server)
{
	return server != NULL ? server->event_fd : -1;
}

unsigned int sippymodem_listen_port(const struct sippymodem_server *server)
{
	return server != NULL ? sip_server_local_port(server->sip) : 0;
}
