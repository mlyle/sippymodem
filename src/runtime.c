// SPDX-License-Identifier: BSD-3-Clause

#include "runtime.h"

#include "modem.h"
#include "host.h"
#include "slmodem_adapter.h"
#include "slmodem_calls.h"
#include "slmodem_driver.h"
#include "slmodem_terminal.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MODEM_RUNTIME_TERMINAL_READ_MAX = 4096 };

struct modem_runtime {
	char modem_name[MODEM_RUNTIME_NAME_MAX];
	struct slmodem_driver driver;
	struct modem *modem;
	struct slmodem_adapter adapter;
	struct slmodem_calls calls;
	struct slmodem_terminal modem_terminal;
	struct pty_terminal *terminal;
	struct recording_files *recording_files;
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
	modem_host_destroy(runtime->host);
	recording_files_destroy(runtime->recording_files);
	if (runtime->modem != NULL)
		modem_delete(runtime->modem);
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
		"DISCONNECT origin=%s phase=%s step=%s reason=%s\n",
		runtime->modem->disconnect_local ? "local" : "remote",
		runtime->modem->disconnect_phase,
		runtime->modem->disconnect_step, reason);
	if (length > 0 && (size_t)length < sizeof(text)) {
		struct sip_trace_record record = {
			.origin = SIP_TRACE_ADAPTER, .level = 3,
			.data = text, .size = (size_t)length,
		};
		trace_signaling(NULL, &record);
	}
	runtime->trace_disconnect_sequence = runtime->modem->disconnect_sequence;
}

struct modem_runtime *modem_runtime_create(
	const struct app_config *config, const char *modem_name)
{
	struct modem_runtime *runtime;
	const struct media_recorder *recorder = NULL;
	struct modem_driver *modem_driver;
	struct sip_signaling_callbacks callbacks;
	size_t name_length;
	int saved_error;

	if (config == NULL || modem_name == NULL || modem_name[0] == '\0') {
		errno = EINVAL;
		return NULL;
	}
	name_length = strlen(modem_name);
	if (name_length >= MODEM_RUNTIME_NAME_MAX) {
		errno = ENAMETOOLONG;
		return NULL;
	}
	errno = 0;
	runtime = calloc(1, sizeof(*runtime));
	if (runtime == NULL)
		return NULL;
	memcpy(runtime->modem_name, modem_name, name_length + 1);
	runtime->trace_enabled = config->sip.trace_enabled;
	if (!slmodem_driver_init(&runtime->driver))
		goto fail;
	modem_driver = slmodem_driver_interface(&runtime->driver);
	if (modem_driver == NULL)
		goto fail;
	runtime->modem = modem_create(modem_driver, runtime->modem_name);
	if (runtime->modem == NULL ||
	    !slmodem_adapter_init(
		    &runtime->adapter, runtime->modem) ||
	    !slmodem_calls_init(&runtime->calls, runtime->modem) ||
	    !slmodem_terminal_init(
		    &runtime->modem_terminal, runtime->modem))
		goto fail;
	runtime->modem->pcm_codec_grid = config->media.pcm_codec_grid;
	runtime->terminal = pty_terminal_create();
	if (runtime->terminal == NULL)
		goto fail;
	if (config->recording.prefix[0] != '\0') {
		runtime->recording_files = recording_files_create(
			config->recording.prefix);
		if (runtime->recording_files == NULL)
			goto fail;
		recorder = recording_files_recorder(
			runtime->recording_files);
		if (recorder == NULL)
			goto fail;
	}
	callbacks = *slmodem_calls_callbacks(&runtime->calls);
	if (config->sip.trace_enabled)
		callbacks.trace = trace_signaling;
	runtime->host = modem_host_create(
		config, recorder, &callbacks,
		slmodem_adapter_core(&runtime->adapter));
	if (runtime->host == NULL ||
	    !slmodem_driver_attach(
		    &runtime->driver, runtime->modem, runtime->host))
		goto fail;
	return runtime;

fail:
	saved_error = errno != 0 ? errno : EINVAL;
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
			slmodem_calls_drain(&runtime->calls);
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
			runtime->trace_started = true;
			runtime->trace_time_ns = now_ns;
			levels = modem_host_take_audio_levels(runtime->host);
			length = snprintf(text, sizeof(text),
				"CALL sip=%s phase=%s datapump=%s step=%s TX-peak=%u/32768 RX-peak=%u/32768\n",
				sip_call_state_name(runtime->calls.state), phase,
				dp != NULL && dp->op != NULL && dp->op->name != NULL ? dp->op->name : "none",
				step, levels.transmit_peak, levels.receive_peak);
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
	return runtime != NULL ?
		       modem_host_wait_ns(runtime->host, now_ns) :
		       UINT64_MAX;
}

void modem_runtime_destroy(struct modem_runtime *runtime)
{
	if (runtime == NULL)
		return;
	(void)modem_runtime_close(runtime);
	if (runtime->modem->started)
		(void)modem_stop_now(runtime->modem);
	(void)slmodem_driver_detach(&runtime->driver);
	modem_host_destroy(runtime->host);
	recording_files_destroy(runtime->recording_files);
	(void)slmodem_terminal_unbind(&runtime->modem_terminal);
	modem_delete(runtime->modem);
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
