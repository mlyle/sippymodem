// SPDX-License-Identifier: BSD-3-Clause

#include "runtime.h"

#include "modem.h"
#include "modem_frontend.h"
#include "datapump_link.h"
#include "host.h"
#include "line.h"
#include "byte_queue.h"
#include "slmodem_adapter.h"
#include "slmodem_calls.h"
#include "slmodem_driver.h"
#include "slmodem_terminal.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

enum { MODEM_RUNTIME_TERMINAL_READ_MAX = 4096 };

struct modem_runtime {
	struct modem_line *line;
	struct byte_queue rx_output, control_output;
	unsigned char rx_storage[16384], control_storage[4096];
	bool control_overflow;
	uint64_t attempt_deadline;
	/* Borrowed views of the line owner; the compatibility frontend owns PTY. */
	struct modem *modem;
	struct slmodem_calls *calls;
	struct slmodem_terminal modem_terminal;
	struct pty_terminal *terminal;
	const struct recording_files *recording_files;
	struct modem_host *host;
	enum modem_host_result last_host_result;
	enum sip_session_result last_session_result;
	enum slmodem_calls_result last_calls_result;
	enum slmodem_terminal_result last_modem_terminal_result;
	enum pty_terminal_result last_terminal_result;
	bool terminal_client_connected;
	bool trace_enabled;
	bool trace_started;
	uint64_t trace_time_ns;
	uint64_t progress_time_ns;
	const char *trace_step;
	const char *trace_phase;
	unsigned int trace_disconnect_sequence;
};

static void destroy_incomplete_runtime(struct modem_runtime *runtime)
{
	if (runtime == NULL)
		return;
	modem_line_destroy(runtime->line);
	pty_terminal_destroy(runtime->terminal);
	free(runtime);
}

static void trace_signaling(void *context, const struct sip_trace_record *record)
{
	static const char *const private_words[] = {
		"authorization", "authenticate", "authentication", "password", "credential",
	};
	char output[8193];
	size_t used = 0;
	(void)context;
	if (record == NULL || record->data == NULL || record->size == 0)
		return;
	/* PJSIP supplies whole log records, including whole SIP messages. Omit
	 * credential-bearing records rather than parsing possibly folded headers.
	 * Keep traces bounded and remote text unable to control the terminal. */
	if (record->size >= sizeof(output)) {
		fputs("[SIP trace: oversized record omitted]\n", stderr);
		return;
	}
	for (size_t word = 0; word < sizeof(private_words) / sizeof(*private_words);
	     ++word) {
		size_t length = strlen(private_words[word]);

		for (size_t offset = 0; offset + length <= record->size; ++offset) {
			size_t matched = 0;

			while (matched < length &&
			       tolower((unsigned char)record->data[offset + matched]) ==
				       private_words[word][matched])
				++matched;
			if (matched == length) {
				fputs("[SIP trace: authentication record omitted]\n", stderr);
				return;
			}
		}
	}
	for (size_t index = 0; index < record->size; ++index) {
		unsigned char byte = (unsigned char)record->data[index];

		if (byte == '\r')
			continue;
		output[used++] = (byte >= 32 && byte < 127) || byte == '\n' || byte == '\t' ?
			(char)byte : '?';
	}
	output[used] = '\0';
	fputs(output, stderr);
	if (used != 0 && output[used - 1] != '\n')
		fputc('\n', stderr);
}

/* Emit the retained first cause once, including when shutdown has no next tick. */
static void trace_disconnect(struct modem_runtime *runtime)
{
	const char *reason = runtime->modem->disconnect_reason;
	if (!runtime->trace_enabled || reason == NULL ||
	    runtime->modem->disconnect_sequence == runtime->trace_disconnect_sequence)
		return;
	char text[384];
	int length = snprintf(text, sizeof(text),
		"DISCONNECT origin=%s phase=%s step=%s reason=%s; %s\n",
		runtime->modem->disconnect_local ? "local" : "remote",
		runtime->modem->disconnect_phase,
		runtime->modem->disconnect_step, reason,
		runtime->modem->disconnect_local ?
			"we ended the call" : "the peer ended the call");
	if (length > 0 && (size_t)length < sizeof(text)) {
		struct sip_trace_record record = {
			.origin = SIP_TRACE_ADAPTER, .level = 3,
			.data = text, .size = (size_t)length,
		};
		trace_signaling(NULL, &record);
	}
	runtime->trace_disconnect_sequence = runtime->modem->disconnect_sequence;
}

static int capture_payload(void *context, const char *bytes, int count)
{
	struct modem_runtime *runtime = context;
	return count <= 0 ? 0 : (int)byte_queue_write(&runtime->rx_output, bytes, (size_t)count);
}

static int capture_control(void *context, const char *bytes, int count)
{
	struct modem_runtime *runtime = context;
	int accepted = count <= 0 ? 0 : (int)byte_queue_write(&runtime->control_output, bytes, (size_t)count);
	if (accepted != count)
		runtime->control_overflow = true;
	return accepted;
}

static bool flush_output(struct modem_runtime *runtime, struct byte_queue *queue)
{
	if (queue->count == 0)
		return true;
	size_t count = queue->capacity - queue->head;
	if (count > queue->count)
		count = queue->count;
	ssize_t sent = write(pty_terminal_master_fd(runtime->terminal), queue->storage + queue->head, count);
	if (sent < 0)
		return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == EIO;
	queue->head = (queue->head + (size_t)sent) % queue->capacity;
	queue->count -= (size_t)sent;
	return true;
}

struct modem_runtime *modem_runtime_create(
	const struct app_config *config, const char *modem_name)
{
	if (config == NULL || modem_name == NULL || modem_name[0] == '\0') {
		errno = EINVAL;
		return NULL;
	}
	if (strlen(modem_name) >= MODEM_RUNTIME_NAME_MAX) {
		errno = ENAMETOOLONG;
		return NULL;
	}
	struct modem_runtime *runtime = calloc(1, sizeof(*runtime));
	if (runtime == NULL)
		return NULL;
	runtime->trace_enabled = config->sip.trace_enabled;
	runtime->line = modem_line_create(config, modem_name,
		config->sip.trace_enabled ? trace_signaling : NULL);
	if (runtime->line == NULL)
		goto fail;
	runtime->modem = modem_line_modem(runtime->line);
	runtime->host = modem_line_host(runtime->line);
	runtime->calls = modem_line_calls(runtime->line);
	runtime->recording_files = modem_line_recording(runtime->line);
	if (!slmodem_terminal_init(&runtime->modem_terminal, runtime->modem))
		goto fail;
	runtime->rx_output = (struct byte_queue){ .storage = runtime->rx_storage, .capacity = sizeof(runtime->rx_storage) };
	runtime->control_output = (struct byte_queue){ .storage = runtime->control_storage, .capacity = sizeof(runtime->control_storage) };
	runtime->modem->data_output = capture_payload;
	runtime->modem->data_output_context = runtime;
	runtime->modem->control_output = capture_control;
	runtime->modem->control_output_context = runtime;
	modem_frontend_init(runtime->modem);
	runtime->terminal = pty_terminal_create();
	if (runtime->terminal == NULL)
		goto fail;
	return runtime;

fail:;
	int saved_error = errno != 0 ? errno : EINVAL;
	destroy_incomplete_runtime(runtime);
	errno = saved_error;
	return NULL;
}

enum modem_runtime_result modem_runtime_open(
	struct modem_runtime *runtime)
{
	struct termios attributes;

	if (runtime == NULL)
		return MODEM_RUNTIME_INVALID_ARGUMENT;
	if (modem_host_is_open(runtime->host))
		return pty_terminal_is_open(runtime->terminal) ?
			       MODEM_RUNTIME_OK :
			       MODEM_RUNTIME_TERMINAL_ERROR;
	runtime->last_terminal_result =
		pty_terminal_open(runtime->terminal);
	if (runtime->last_terminal_result != PTY_TERMINAL_OK &&
	    runtime->last_terminal_result != PTY_TERMINAL_ALREADY_OPEN)
		return MODEM_RUNTIME_TERMINAL_ERROR;
	runtime->last_terminal_result = pty_terminal_get_attributes(
		runtime->terminal, &attributes);
	if (runtime->last_terminal_result != PTY_TERMINAL_OK) {
		(void)pty_terminal_close(runtime->terminal);
		return MODEM_RUNTIME_TERMINAL_ERROR;
	}
	runtime->last_modem_terminal_result =
		slmodem_terminal_bind(
			&runtime->modem_terminal,
			pty_terminal_master_fd(runtime->terminal),
			pty_terminal_device_path(runtime->terminal));
	if (runtime->last_modem_terminal_result !=
	    SLMODEM_TERMINAL_OK) {
		(void)slmodem_terminal_unbind(
			&runtime->modem_terminal);
		(void)pty_terminal_close(runtime->terminal);
		return MODEM_RUNTIME_MODEM_TERMINAL_ERROR;
	}
	runtime->last_modem_terminal_result =
		slmodem_terminal_apply_attributes(
			&runtime->modem_terminal, &attributes);
	if (runtime->last_modem_terminal_result !=
		    SLMODEM_TERMINAL_OK &&
	    runtime->last_modem_terminal_result !=
		    SLMODEM_TERMINAL_IDLE) {
		(void)slmodem_terminal_unbind(
			&runtime->modem_terminal);
		(void)pty_terminal_close(runtime->terminal);
		return MODEM_RUNTIME_MODEM_TERMINAL_ERROR;
	}
	runtime->terminal_client_connected = false;
	runtime->last_session_result = modem_host_open(runtime->host);
	if (runtime->last_session_result == SIP_SESSION_OK)
		return MODEM_RUNTIME_OK;
	runtime->last_modem_terminal_result =
		slmodem_terminal_unbind(&runtime->modem_terminal);
	runtime->terminal_client_connected = false;
	runtime->last_terminal_result =
		pty_terminal_close(runtime->terminal);
	return MODEM_RUNTIME_SESSION_ERROR;
}

enum modem_runtime_result modem_runtime_close(
	struct modem_runtime *runtime)
{
	int stop_result;

	if (runtime == NULL)
		return MODEM_RUNTIME_INVALID_ARGUMENT;
	if (runtime->modem->started)
		modem_note_disconnect(runtime->modem, true, "local runtime shutdown");
	stop_result = modem_report_remote_call_end(runtime->modem);
	if (runtime->modem->started)
		stop_result = modem_stop_now(runtime->modem);
	trace_disconnect(runtime);
	if (modem_host_is_open(runtime->host))
		runtime->last_session_result =
			modem_host_close(runtime->host);
	else
		runtime->last_session_result = SIP_SESSION_NOT_OPEN;
	runtime->last_modem_terminal_result =
		slmodem_terminal_unbind(&runtime->modem_terminal);
	runtime->terminal_client_connected = false;
	runtime->last_terminal_result =
		pty_terminal_close(runtime->terminal);
	if (stop_result != 0)
		return MODEM_RUNTIME_MODEM_STOP_ERROR;
	if (runtime->last_session_result != SIP_SESSION_OK &&
	    runtime->last_session_result != SIP_SESSION_NOT_OPEN)
		return MODEM_RUNTIME_SESSION_ERROR;
	if (runtime->last_modem_terminal_result !=
		    SLMODEM_TERMINAL_OK &&
	    runtime->last_modem_terminal_result !=
		    SLMODEM_TERMINAL_IDLE)
		return MODEM_RUNTIME_MODEM_TERMINAL_ERROR;
	return runtime->last_terminal_result == PTY_TERMINAL_OK ?
		       MODEM_RUNTIME_OK :
		       MODEM_RUNTIME_TERMINAL_ERROR;
}

static enum modem_runtime_result drain_call_transitions(
	struct modem_runtime *runtime)
{
	for (unsigned int count = 0;
	     count < MODEM_RUNTIME_MAX_CALL_TRANSITIONS; ++count) {
		runtime->last_calls_result =
			slmodem_calls_drain(runtime->calls);
		if (runtime->last_calls_result ==
		    SLMODEM_CALLS_IDLE)
			return MODEM_RUNTIME_OK;
		if (runtime->last_calls_result !=
		    SLMODEM_CALLS_OK)
			return MODEM_RUNTIME_CALL_ERROR;
	}
	return MODEM_RUNTIME_CALL_ERROR;
}

static enum modem_runtime_result advance_terminal(
	struct modem_runtime *runtime, size_t *bytes_received)
{
	char buffer[MODEM_RUNTIME_TERMINAL_READ_MAX];
	struct termios attributes;
	size_t available;
	size_t count = 0;
	bool worked = false;

	if (!pty_terminal_is_open(runtime->terminal))
		return MODEM_RUNTIME_IDLE;
	runtime->last_terminal_result = pty_terminal_get_attributes(
		runtime->terminal, &attributes);
	if (runtime->last_terminal_result != PTY_TERMINAL_OK)
		return MODEM_RUNTIME_TERMINAL_ERROR;
	runtime->last_modem_terminal_result =
		slmodem_terminal_apply_attributes(
			&runtime->modem_terminal, &attributes);
	if (runtime->last_modem_terminal_result ==
	    SLMODEM_TERMINAL_OK)
		worked = true;
	else if (runtime->last_modem_terminal_result !=
		 SLMODEM_TERMINAL_IDLE)
		return MODEM_RUNTIME_MODEM_TERMINAL_ERROR;
	runtime->last_modem_terminal_result =
		slmodem_terminal_service_event(
			&runtime->modem_terminal);
	if (runtime->last_modem_terminal_result ==
	    SLMODEM_TERMINAL_OK)
		worked = true;
	else if (runtime->last_modem_terminal_result !=
		 SLMODEM_TERMINAL_IDLE)
		return MODEM_RUNTIME_MODEM_TERMINAL_ERROR;
	runtime->last_modem_terminal_result =
		slmodem_terminal_input_capacity(
			&runtime->modem_terminal, &available);
	if (runtime->last_modem_terminal_result !=
	    SLMODEM_TERMINAL_OK)
		return MODEM_RUNTIME_MODEM_TERMINAL_ERROR;
	if (available > sizeof(buffer))
		available = sizeof(buffer);
	if (available == 0)
		return worked ? MODEM_RUNTIME_OK : MODEM_RUNTIME_IDLE;
	runtime->last_terminal_result = pty_terminal_read(
		runtime->terminal, buffer, available, &count);
	if (runtime->last_terminal_result == PTY_TERMINAL_IDLE) {
		return worked ? MODEM_RUNTIME_OK : MODEM_RUNTIME_IDLE;
	}
	if (runtime->last_terminal_result ==
	    PTY_TERMINAL_CLIENT_DISCONNECTED) {
		if (runtime->terminal_client_connected) {
			runtime->terminal_client_connected = false;
			runtime->last_modem_terminal_result =
				slmodem_terminal_disconnect(
					&runtime->modem_terminal);
			if (runtime->last_modem_terminal_result !=
				    SLMODEM_TERMINAL_OK &&
			    runtime->last_modem_terminal_result !=
				    SLMODEM_TERMINAL_IDLE)
				return MODEM_RUNTIME_MODEM_TERMINAL_ERROR;
			worked = true;
		}
		return worked ? MODEM_RUNTIME_OK : MODEM_RUNTIME_IDLE;
	}
	if (runtime->last_terminal_result != PTY_TERMINAL_OK)
		return MODEM_RUNTIME_TERMINAL_ERROR;
	runtime->terminal_client_connected = true;
	if (bytes_received != NULL)
		*bytes_received = count;
	runtime->last_modem_terminal_result =
		slmodem_terminal_receive(
			&runtime->modem_terminal, buffer, count);
	if (runtime->last_modem_terminal_result !=
	    SLMODEM_TERMINAL_OK)
		return MODEM_RUNTIME_MODEM_TERMINAL_ERROR;
	return MODEM_RUNTIME_OK;
}

static void format_link_rate(char *text, size_t size, unsigned bps, double baud)
{
	char bits[16] = "?";
	char symbols[24] = "?";

	if (bps != 0)
		snprintf(bits, sizeof(bits), "%u", bps);
	if (baud > 0)
		snprintf(symbols, sizeof(symbols), "%.6g", baud);
	snprintf(text, size, "%sbps/%sbaud", bits, symbols);
}

static void trace_call(struct modem_runtime *runtime, uint64_t now_ns)
{
	struct audio_levels levels;
	if (!runtime->trace_enabled || !modem_host_is_open(runtime->host))
		return;
	const struct dp *dp = runtime->modem->dp;
	const char *phase = modem_call_phase(runtime->modem);
	const char *step = modem_protocol_step(runtime->modem);
	char text[384];
	struct sip_trace_record record = {
		.origin = SIP_TRACE_ADAPTER,
		.level = 3,
		.data = text,
	};
	int length;

	bool heartbeat = !runtime->trace_started ||
		(now_ns > runtime->trace_time_ns &&
		 now_ns - runtime->trace_time_ns >= UINT64_C(1000000000));
	bool changed = runtime->trace_step == NULL ||
		strcmp(step, runtime->trace_step) != 0 ||
		strcmp(phase, runtime->trace_phase) != 0;
	bool progress = runtime->trace_started && changed &&
		now_ns > runtime->progress_time_ns &&
		now_ns - runtime->progress_time_ns >= UINT64_C(500000000);

	if (heartbeat || progress) {
		if (heartbeat) {
			struct dp_link_rates rates = datapump_get_link_rates(dp);
			char transmit[64], receive[64];

			format_link_rate(transmit, sizeof(transmit), rates.transmit_bps, rates.transmit_baud);
			format_link_rate(receive, sizeof(receive), rates.receive_bps, rates.receive_baud);
			runtime->trace_started = true;
			runtime->trace_time_ns = now_ns;
			levels = modem_host_take_audio_levels(runtime->host);
			length = snprintf(text, sizeof(text),
				"CALL sip=%s phase=%s datapump=%s step=%s TX=%s RX=%s TX-peak=%u/32768 RX-peak=%u/32768\n",
				sip_call_state_name(runtime->calls->state), phase,
				dp != NULL && dp->op != NULL && dp->op->name != NULL ? dp->op->name : "none",
				step, transmit, receive, levels.transmit_peak, levels.receive_peak);
		} else {
			length = snprintf(text, sizeof(text),
				"TRAIN phase=%s step=%s\n", phase, step);
		}
		if (length > 0 && (size_t)length < sizeof(text)) {
			record.size = (size_t)length;
			trace_signaling(NULL, &record);
		}
		if (changed) {
			runtime->progress_time_ns = now_ns;
			runtime->trace_phase = phase;
			runtime->trace_step = step;
		}
	}
	trace_disconnect(runtime);
}

enum modem_runtime_result modem_runtime_step(
	struct modem_runtime *runtime, uint64_t now_ns,
	size_t *terminal_bytes_received, size_t *datagrams_received,
	size_t *frames_processed)
{
	enum modem_runtime_result calls_result;
	enum modem_runtime_result terminal_result;

	if (terminal_bytes_received != NULL)
		*terminal_bytes_received = 0;
	if (datagrams_received != NULL)
		*datagrams_received = 0;
	if (frames_processed != NULL)
		*frames_processed = 0;
	if (runtime == NULL)
		return MODEM_RUNTIME_INVALID_ARGUMENT;
	terminal_result = advance_terminal(runtime, terminal_bytes_received);
	runtime->last_host_result = modem_host_step(
		runtime->host, now_ns, datagrams_received, frames_processed);
	/* Retain our decision before the resulting SIP idle event is drained. */
	if (runtime->last_host_result != MODEM_HOST_OK &&
	    runtime->last_host_result != MODEM_HOST_IDLE)
		modem_note_disconnect(runtime->modem, true,
			modem_host_result_name(runtime->last_host_result));
	calls_result = drain_call_transitions(runtime);
	if (!runtime->modem->started || (runtime->modem->modem_info & TIOCM_CD))
		runtime->attempt_deadline = 0;
	else if (runtime->attempt_deadline == 0) {
		uint64_t delay = (uint64_t)runtime->modem->sregs[SREG_WAIT_CARRIER_AFTER_DIAL] * UINT64_C(1000000000);
		runtime->attempt_deadline = delay > UINT64_MAX - now_ns ? UINT64_MAX : now_ns + delay;
	} else if (now_ns >= runtime->attempt_deadline) {
		modem_note_disconnect(runtime->modem, true, "whole call attempt expired");
		modem_update_status(runtime->modem, STATUS_NOCARRIER);
		(void)modem_stop_now(runtime->modem);
		runtime->attempt_deadline = 0;
	}
	/* Raw async cannot apply receive flow control. Fail in this safe owner
	 * boundary, after the RX callback and datapump processing have returned. */
	if (runtime->modem->started && (runtime->modem->dte_overflow || runtime->control_overflow)) {
		modem_note_disconnect(runtime->modem, true, runtime->control_overflow ?
			"terminal control output overflow" : "raw DTE receive overflow");
		(void)modem_stop_now(runtime->modem);
	}
	if (runtime->control_overflow)
		terminal_result = MODEM_RUNTIME_TERMINAL_ERROR;
	if (!flush_output(runtime, &runtime->control_output) ||
	    ((!runtime->modem->command || !(runtime->modem->modem_info & TIOCM_CD)) && runtime->control_output.count == 0 &&
	     !flush_output(runtime, &runtime->rx_output)))
		terminal_result = MODEM_RUNTIME_TERMINAL_ERROR;
	trace_call(runtime, now_ns);
	if (runtime->recording_files != NULL &&
	    recording_files_last_result(runtime->recording_files) !=
		    RECORDING_FILES_OK)
		return MODEM_RUNTIME_RECORDING_ERROR;
	if (calls_result != MODEM_RUNTIME_OK)
		return calls_result;
	if (terminal_result != MODEM_RUNTIME_OK &&
	    terminal_result != MODEM_RUNTIME_IDLE)
		return terminal_result;
	if (runtime->last_host_result ==
	    MODEM_HOST_MEDIA_BINDING_ERROR)
		return MODEM_RUNTIME_MEDIA_BINDING_ERROR;
	/* The host has already suspended audio and requested SIP hangup. Keep the
	 * terminal alive so the modem result can be reported and reused. */
	if (runtime->last_host_result == MODEM_HOST_MODEM_STOPPED)
		return MODEM_RUNTIME_OK;
	if (runtime->last_host_result == MODEM_HOST_MEDIA_TIMEOUT)
		return modem_report_remote_call_end(runtime->modem) == 0 ?
			MODEM_RUNTIME_OK : MODEM_RUNTIME_MODEM_STOP_ERROR;
	if (runtime->last_host_result != MODEM_HOST_OK &&
	    runtime->last_host_result != MODEM_HOST_IDLE)
		return MODEM_RUNTIME_HOST_ERROR;
	return runtime->last_host_result == MODEM_HOST_OK ||
		       terminal_result == MODEM_RUNTIME_OK ?
		       MODEM_RUNTIME_OK :
		       MODEM_RUNTIME_IDLE;
}

uint64_t modem_runtime_wait_ns(
	const struct modem_runtime *runtime, uint64_t now_ns)
{
	if (runtime == NULL)
		return UINT64_MAX;
	uint64_t wait = modem_host_wait_ns(runtime->host, now_ns);
	if (runtime->attempt_deadline != 0) {
		uint64_t remaining = runtime->attempt_deadline <= now_ns ? 0 : runtime->attempt_deadline - now_ns;
		if (remaining < wait)
			wait = remaining;
	}
	return wait;
}

void modem_runtime_destroy(struct modem_runtime *runtime)
{
	if (runtime == NULL)
		return;
	(void)modem_runtime_close(runtime);
	if (runtime->modem->started)
		(void)modem_stop_now(runtime->modem);
	(void)slmodem_terminal_unbind(&runtime->modem_terminal);
	modem_line_destroy(runtime->line);
	pty_terminal_destroy(runtime->terminal);
	free(runtime);
}

struct modem *modem_runtime_modem(struct modem_runtime *runtime)
{
	return runtime != NULL ? runtime->modem : NULL;
}

const struct recording_files *modem_runtime_recording_files(
	const struct modem_runtime *runtime)
{
	return runtime != NULL ? runtime->recording_files : NULL;
}

bool modem_runtime_is_open(const struct modem_runtime *runtime)
{
	return runtime != NULL && modem_host_is_open(runtime->host) &&
	       pty_terminal_is_open(runtime->terminal);
}

int modem_runtime_terminal_fd(
	const struct modem_runtime *runtime)
{
	return runtime != NULL ?
		       pty_terminal_master_fd(runtime->terminal) :
		       -1;
}

int modem_runtime_terminal_poll_fd(
	const struct modem_runtime *runtime)
{
	size_t capacity;

	if (runtime == NULL ||
	    runtime->last_terminal_result ==
		    PTY_TERMINAL_CLIENT_DISCONNECTED ||
	    slmodem_terminal_input_capacity(
		    &runtime->modem_terminal, &capacity) !=
		    SLMODEM_TERMINAL_OK ||
	    capacity == 0)
		return -1;
	return pty_terminal_master_fd(runtime->terminal);
}

int modem_runtime_media_poll_fd(
	const struct modem_runtime *runtime)
{
	return runtime != NULL ?
		       modem_host_media_poll_fd(runtime->host) :
		       -1;
}

const char *modem_runtime_device_path(
	const struct modem_runtime *runtime)
{
	return runtime != NULL ?
		       pty_terminal_device_path(runtime->terminal) :
		       NULL;
}

enum modem_host_result modem_runtime_last_host_result(
	const struct modem_runtime *runtime)
{
	return runtime != NULL ? runtime->last_host_result :
			       MODEM_HOST_INVALID_ARGUMENT;
}

enum sip_session_result modem_runtime_last_session_result(
	const struct modem_runtime *runtime)
{
	return runtime != NULL ? runtime->last_session_result :
			       SIP_SESSION_INVALID_ARGUMENT;
}

enum pty_terminal_result modem_runtime_last_terminal_result(
	const struct modem_runtime *runtime)
{
	return runtime != NULL ? runtime->last_terminal_result :
			       PTY_TERMINAL_INVALID_ARGUMENT;
}

enum slmodem_calls_result modem_runtime_last_calls_result(
	const struct modem_runtime *runtime)
{
	return runtime != NULL ? runtime->last_calls_result :
			       SLMODEM_CALLS_INVALID_ARGUMENT;
}

enum slmodem_terminal_result
modem_runtime_last_modem_terminal_result(
	const struct modem_runtime *runtime)
{
	return runtime != NULL ? runtime->last_modem_terminal_result :
			       SLMODEM_TERMINAL_INVALID_ARGUMENT;
}

const char *modem_runtime_result_name(
	enum modem_runtime_result result)
{
	switch (result) {
	case MODEM_RUNTIME_OK:
		return "ok";
	case MODEM_RUNTIME_IDLE:
		return "idle";
	case MODEM_RUNTIME_MEDIA_BINDING_ERROR:
		return "negotiated media could not be bound";
	case MODEM_RUNTIME_INVALID_ARGUMENT:
		return "invalid argument";
	case MODEM_RUNTIME_TERMINAL_ERROR:
		return "modem terminal failed";
	case MODEM_RUNTIME_SESSION_ERROR:
		return "SIP/RTP session failed";
	case MODEM_RUNTIME_MODEM_STOP_ERROR:
		return "modem stop failed";
	case MODEM_RUNTIME_HOST_ERROR:
		return "SIP modem host failed";
	case MODEM_RUNTIME_CALL_ERROR:
		return "modem call transition failed";
	case MODEM_RUNTIME_RECORDING_ERROR:
		return "recording evidence failed";
	case MODEM_RUNTIME_MODEM_TERMINAL_ERROR:
		return "modem terminal adapter failed";
	}
	return "unknown runtime result";
}
