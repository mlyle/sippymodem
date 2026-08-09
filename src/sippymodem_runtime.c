// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_runtime.h"

#include "modem.h"
#include "sippymodem_datapumps.h"
#include "sippymodem_host.h"
#include "sippymodem_slmodem_adapter.h"
#include "sippymodem_slmodem_calls.h"
#include "sippymodem_slmodem_driver.h"
#include "sippymodem_slmodem_terminal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

enum { SIPPYMODEM_RUNTIME_TERMINAL_READ_MAX = 4096 };

struct sippymodem_runtime {
	char modem_name[SIPPYMODEM_RUNTIME_NAME_MAX];
	struct sippymodem_datapumps datapumps;
	struct sippymodem_slmodem_driver driver;
	struct modem *modem;
	struct sippymodem_slmodem_adapter adapter;
	struct sippymodem_slmodem_calls calls;
	struct sippymodem_slmodem_terminal modem_terminal;
	struct sippymodem_terminal *terminal;
	struct sippymodem_recording_files *recording_files;
	struct sippymodem_host *host;
	enum sippymodem_host_result last_host_result;
	enum sippymodem_session_result last_session_result;
	enum sippymodem_slmodem_calls_result last_calls_result;
	enum sippymodem_slmodem_terminal_result last_modem_terminal_result;
	enum sippymodem_terminal_result last_terminal_result;
	bool terminal_client_connected;
};

static void destroy_incomplete_runtime(struct sippymodem_runtime *runtime)
{
	if (runtime == NULL)
		return;
	sippymodem_host_destroy(runtime->host);
	sippymodem_recording_files_destroy(runtime->recording_files);
	if (runtime->modem != NULL)
		modem_delete(runtime->modem);
	if (sippymodem_datapumps_are_started(&runtime->datapumps))
		(void)sippymodem_datapumps_stop(&runtime->datapumps);
	sippymodem_terminal_destroy(runtime->terminal);
	free(runtime);
}

struct sippymodem_runtime *sippymodem_runtime_create(
	const struct sippymodem_config *config, const char *modem_name)
{
	struct sippymodem_runtime *runtime;
	const struct sippymodem_recorder *recorder = NULL;
	struct modem_driver *modem_driver;
	size_t name_length;
	int saved_error;

	if (config == NULL || modem_name == NULL || modem_name[0] == '\0') {
		errno = EINVAL;
		return NULL;
	}
	name_length = strlen(modem_name);
	if (name_length >= SIPPYMODEM_RUNTIME_NAME_MAX) {
		errno = ENAMETOOLONG;
		return NULL;
	}
	errno = 0;
	runtime = calloc(1, sizeof(*runtime));
	if (runtime == NULL)
		return NULL;
	memcpy(runtime->modem_name, modem_name, name_length + 1);
	if (sippymodem_datapumps_start(&runtime->datapumps) !=
	    SIPPYMODEM_DATAPUMPS_OK) {
		errno = EBUSY;
		goto fail;
	}
	if (!sippymodem_slmodem_driver_init(&runtime->driver))
		goto fail;
	modem_driver = sippymodem_slmodem_driver_interface(&runtime->driver);
	if (modem_driver == NULL)
		goto fail;
	runtime->modem = modem_create(modem_driver, runtime->modem_name);
	if (runtime->modem == NULL ||
	    !sippymodem_slmodem_adapter_init(
		    &runtime->adapter, runtime->modem) ||
	    !sippymodem_slmodem_calls_init(&runtime->calls, runtime->modem) ||
	    !sippymodem_slmodem_terminal_init(
		    &runtime->modem_terminal, runtime->modem))
		goto fail;
	runtime->terminal = sippymodem_terminal_create();
	if (runtime->terminal == NULL)
		goto fail;
	if (config->recording.prefix[0] != '\0') {
		runtime->recording_files = sippymodem_recording_files_create(
			config->recording.prefix);
		if (runtime->recording_files == NULL)
			goto fail;
		recorder = sippymodem_recording_files_recorder(
			runtime->recording_files);
		if (recorder == NULL)
			goto fail;
	}
	runtime->host = sippymodem_host_create(
		config, recorder,
		sippymodem_slmodem_calls_callbacks(&runtime->calls),
		sippymodem_slmodem_adapter_core(&runtime->adapter));
	if (runtime->host == NULL ||
	    !sippymodem_slmodem_driver_attach(
		    &runtime->driver, runtime->modem, runtime->host))
		goto fail;
	return runtime;

fail:
	saved_error = errno != 0 ? errno : EINVAL;
	destroy_incomplete_runtime(runtime);
	errno = saved_error;
	return NULL;
}

enum sippymodem_runtime_result sippymodem_runtime_open(
	struct sippymodem_runtime *runtime)
{
	struct termios attributes;

	if (runtime == NULL)
		return SIPPYMODEM_RUNTIME_INVALID_ARGUMENT;
	if (sippymodem_host_is_open(runtime->host))
		return sippymodem_terminal_is_open(runtime->terminal) ?
			       SIPPYMODEM_RUNTIME_OK :
			       SIPPYMODEM_RUNTIME_TERMINAL_ERROR;
	runtime->last_terminal_result =
		sippymodem_terminal_open(runtime->terminal);
	if (runtime->last_terminal_result != SIPPYMODEM_TERMINAL_OK &&
	    runtime->last_terminal_result != SIPPYMODEM_TERMINAL_ALREADY_OPEN)
		return SIPPYMODEM_RUNTIME_TERMINAL_ERROR;
	runtime->last_terminal_result = sippymodem_terminal_get_attributes(
		runtime->terminal, &attributes);
	if (runtime->last_terminal_result != SIPPYMODEM_TERMINAL_OK) {
		(void)sippymodem_terminal_close(runtime->terminal);
		return SIPPYMODEM_RUNTIME_TERMINAL_ERROR;
	}
	runtime->last_modem_terminal_result =
		sippymodem_slmodem_terminal_bind(
			&runtime->modem_terminal,
			sippymodem_terminal_master_fd(runtime->terminal),
			sippymodem_terminal_device_path(runtime->terminal));
	if (runtime->last_modem_terminal_result !=
	    SIPPYMODEM_SLMODEM_TERMINAL_OK) {
		(void)sippymodem_slmodem_terminal_unbind(
			&runtime->modem_terminal);
		(void)sippymodem_terminal_close(runtime->terminal);
		return SIPPYMODEM_RUNTIME_MODEM_TERMINAL_ERROR;
	}
	runtime->last_modem_terminal_result =
		sippymodem_slmodem_terminal_apply_attributes(
			&runtime->modem_terminal, &attributes);
	if (runtime->last_modem_terminal_result !=
		    SIPPYMODEM_SLMODEM_TERMINAL_OK &&
	    runtime->last_modem_terminal_result !=
		    SIPPYMODEM_SLMODEM_TERMINAL_IDLE) {
		(void)sippymodem_slmodem_terminal_unbind(
			&runtime->modem_terminal);
		(void)sippymodem_terminal_close(runtime->terminal);
		return SIPPYMODEM_RUNTIME_MODEM_TERMINAL_ERROR;
	}
	runtime->terminal_client_connected = false;
	runtime->last_session_result = sippymodem_host_open(runtime->host);
	if (runtime->last_session_result == SIPPYMODEM_SESSION_OK)
		return SIPPYMODEM_RUNTIME_OK;
	runtime->last_modem_terminal_result =
		sippymodem_slmodem_terminal_unbind(&runtime->modem_terminal);
	runtime->terminal_client_connected = false;
	runtime->last_terminal_result =
		sippymodem_terminal_close(runtime->terminal);
	return SIPPYMODEM_RUNTIME_SESSION_ERROR;
}

enum sippymodem_runtime_result sippymodem_runtime_close(
	struct sippymodem_runtime *runtime)
{
	int stop_result;

	if (runtime == NULL)
		return SIPPYMODEM_RUNTIME_INVALID_ARGUMENT;
	stop_result = modem_report_remote_call_end(runtime->modem);
	if (runtime->modem->started)
		stop_result = modem_stop_now(runtime->modem);
	if (sippymodem_host_is_open(runtime->host))
		runtime->last_session_result =
			sippymodem_host_close(runtime->host);
	else
		runtime->last_session_result = SIPPYMODEM_SESSION_NOT_OPEN;
	runtime->last_modem_terminal_result =
		sippymodem_slmodem_terminal_unbind(&runtime->modem_terminal);
	runtime->terminal_client_connected = false;
	runtime->last_terminal_result =
		sippymodem_terminal_close(runtime->terminal);
	if (stop_result != 0)
		return SIPPYMODEM_RUNTIME_MODEM_STOP_ERROR;
	if (runtime->last_session_result != SIPPYMODEM_SESSION_OK &&
	    runtime->last_session_result != SIPPYMODEM_SESSION_NOT_OPEN)
		return SIPPYMODEM_RUNTIME_SESSION_ERROR;
	if (runtime->last_modem_terminal_result !=
		    SIPPYMODEM_SLMODEM_TERMINAL_OK &&
	    runtime->last_modem_terminal_result !=
		    SIPPYMODEM_SLMODEM_TERMINAL_IDLE)
		return SIPPYMODEM_RUNTIME_MODEM_TERMINAL_ERROR;
	return runtime->last_terminal_result == SIPPYMODEM_TERMINAL_OK ?
		       SIPPYMODEM_RUNTIME_OK :
		       SIPPYMODEM_RUNTIME_TERMINAL_ERROR;
}

static enum sippymodem_runtime_result drain_call_transitions(
	struct sippymodem_runtime *runtime)
{
	for (unsigned int count = 0;
	     count < SIPPYMODEM_RUNTIME_MAX_CALL_TRANSITIONS; ++count) {
		runtime->last_calls_result =
			sippymodem_slmodem_calls_drain(&runtime->calls);
		if (runtime->last_calls_result ==
		    SIPPYMODEM_SLMODEM_CALLS_IDLE)
			return SIPPYMODEM_RUNTIME_OK;
		if (runtime->last_calls_result !=
		    SIPPYMODEM_SLMODEM_CALLS_OK)
			return SIPPYMODEM_RUNTIME_CALL_ERROR;
	}
	return SIPPYMODEM_RUNTIME_CALL_ERROR;
}

static enum sippymodem_runtime_result advance_terminal(
	struct sippymodem_runtime *runtime, size_t *bytes_received)
{
	char buffer[SIPPYMODEM_RUNTIME_TERMINAL_READ_MAX];
	struct termios attributes;
	size_t available;
	size_t count = 0;
	bool worked = false;

	if (!sippymodem_terminal_is_open(runtime->terminal))
		return SIPPYMODEM_RUNTIME_IDLE;
	runtime->last_terminal_result = sippymodem_terminal_get_attributes(
		runtime->terminal, &attributes);
	if (runtime->last_terminal_result != SIPPYMODEM_TERMINAL_OK)
		return SIPPYMODEM_RUNTIME_TERMINAL_ERROR;
	runtime->last_modem_terminal_result =
		sippymodem_slmodem_terminal_apply_attributes(
			&runtime->modem_terminal, &attributes);
	if (runtime->last_modem_terminal_result ==
	    SIPPYMODEM_SLMODEM_TERMINAL_OK)
		worked = true;
	else if (runtime->last_modem_terminal_result !=
		 SIPPYMODEM_SLMODEM_TERMINAL_IDLE)
		return SIPPYMODEM_RUNTIME_MODEM_TERMINAL_ERROR;
	runtime->last_modem_terminal_result =
		sippymodem_slmodem_terminal_service_event(
			&runtime->modem_terminal);
	if (runtime->last_modem_terminal_result ==
	    SIPPYMODEM_SLMODEM_TERMINAL_OK)
		worked = true;
	else if (runtime->last_modem_terminal_result !=
		 SIPPYMODEM_SLMODEM_TERMINAL_IDLE)
		return SIPPYMODEM_RUNTIME_MODEM_TERMINAL_ERROR;
	runtime->last_modem_terminal_result =
		sippymodem_slmodem_terminal_input_capacity(
			&runtime->modem_terminal, &available);
	if (runtime->last_modem_terminal_result !=
	    SIPPYMODEM_SLMODEM_TERMINAL_OK)
		return SIPPYMODEM_RUNTIME_MODEM_TERMINAL_ERROR;
	if (available > sizeof(buffer))
		available = sizeof(buffer);
	if (available == 0)
		return worked ? SIPPYMODEM_RUNTIME_OK : SIPPYMODEM_RUNTIME_IDLE;
	runtime->last_terminal_result = sippymodem_terminal_read(
		runtime->terminal, buffer, available, &count);
	if (runtime->last_terminal_result == SIPPYMODEM_TERMINAL_IDLE) {
		return worked ? SIPPYMODEM_RUNTIME_OK : SIPPYMODEM_RUNTIME_IDLE;
	}
	if (runtime->last_terminal_result ==
	    SIPPYMODEM_TERMINAL_CLIENT_DISCONNECTED) {
		if (runtime->terminal_client_connected) {
			runtime->terminal_client_connected = false;
			runtime->last_modem_terminal_result =
				sippymodem_slmodem_terminal_disconnect(
					&runtime->modem_terminal);
			if (runtime->last_modem_terminal_result !=
				    SIPPYMODEM_SLMODEM_TERMINAL_OK &&
			    runtime->last_modem_terminal_result !=
				    SIPPYMODEM_SLMODEM_TERMINAL_IDLE)
				return SIPPYMODEM_RUNTIME_MODEM_TERMINAL_ERROR;
			worked = true;
		}
		return worked ? SIPPYMODEM_RUNTIME_OK : SIPPYMODEM_RUNTIME_IDLE;
	}
	if (runtime->last_terminal_result != SIPPYMODEM_TERMINAL_OK)
		return SIPPYMODEM_RUNTIME_TERMINAL_ERROR;
	runtime->terminal_client_connected = true;
	if (bytes_received != NULL)
		*bytes_received = count;
	runtime->last_modem_terminal_result =
		sippymodem_slmodem_terminal_receive(
			&runtime->modem_terminal, buffer, count);
	if (runtime->last_modem_terminal_result !=
	    SIPPYMODEM_SLMODEM_TERMINAL_OK)
		return SIPPYMODEM_RUNTIME_MODEM_TERMINAL_ERROR;
	return SIPPYMODEM_RUNTIME_OK;
}

enum sippymodem_runtime_result sippymodem_runtime_step(
	struct sippymodem_runtime *runtime, uint64_t now_ns,
	size_t *terminal_bytes_received, size_t *datagrams_received,
	size_t *frames_processed)
{
	enum sippymodem_runtime_result calls_result;
	enum sippymodem_runtime_result terminal_result;

	if (terminal_bytes_received != NULL)
		*terminal_bytes_received = 0;
	if (datagrams_received != NULL)
		*datagrams_received = 0;
	if (frames_processed != NULL)
		*frames_processed = 0;
	if (runtime == NULL)
		return SIPPYMODEM_RUNTIME_INVALID_ARGUMENT;
	terminal_result = advance_terminal(runtime, terminal_bytes_received);
	runtime->last_host_result = sippymodem_host_step(
		runtime->host, now_ns, datagrams_received, frames_processed);
	calls_result = drain_call_transitions(runtime);
	if (runtime->recording_files != NULL &&
	    sippymodem_recording_files_last_result(runtime->recording_files) !=
		    SIPPYMODEM_RECORDING_FILES_OK)
		return SIPPYMODEM_RUNTIME_RECORDING_ERROR;
	if (calls_result != SIPPYMODEM_RUNTIME_OK)
		return calls_result;
	if (terminal_result != SIPPYMODEM_RUNTIME_OK &&
	    terminal_result != SIPPYMODEM_RUNTIME_IDLE)
		return terminal_result;
	if (runtime->last_host_result ==
	    SIPPYMODEM_HOST_MEDIA_BINDING_ERROR)
		return SIPPYMODEM_RUNTIME_MEDIA_BINDING_ERROR;
	if (runtime->last_host_result != SIPPYMODEM_HOST_OK &&
	    runtime->last_host_result != SIPPYMODEM_HOST_IDLE)
		return SIPPYMODEM_RUNTIME_HOST_ERROR;
	return runtime->last_host_result == SIPPYMODEM_HOST_OK ||
		       terminal_result == SIPPYMODEM_RUNTIME_OK ?
		       SIPPYMODEM_RUNTIME_OK :
		       SIPPYMODEM_RUNTIME_IDLE;
}

uint64_t sippymodem_runtime_wait_ns(
	const struct sippymodem_runtime *runtime, uint64_t now_ns)
{
	return runtime != NULL ?
		       sippymodem_host_wait_ns(runtime->host, now_ns) :
		       UINT64_MAX;
}

void sippymodem_runtime_destroy(struct sippymodem_runtime *runtime)
{
	if (runtime == NULL)
		return;
	(void)sippymodem_runtime_close(runtime);
	if (runtime->modem->started)
		(void)modem_stop_now(runtime->modem);
	(void)sippymodem_slmodem_driver_detach(&runtime->driver);
	sippymodem_host_destroy(runtime->host);
	sippymodem_recording_files_destroy(runtime->recording_files);
	(void)sippymodem_slmodem_terminal_unbind(&runtime->modem_terminal);
	modem_delete(runtime->modem);
	(void)sippymodem_datapumps_stop(&runtime->datapumps);
	sippymodem_terminal_destroy(runtime->terminal);
	free(runtime);
}

struct modem *sippymodem_runtime_modem(struct sippymodem_runtime *runtime)
{
	return runtime != NULL ? runtime->modem : NULL;
}

const struct sippymodem_recording_files *sippymodem_runtime_recording_files(
	const struct sippymodem_runtime *runtime)
{
	return runtime != NULL ? runtime->recording_files : NULL;
}

bool sippymodem_runtime_is_open(const struct sippymodem_runtime *runtime)
{
	return runtime != NULL && sippymodem_host_is_open(runtime->host) &&
	       sippymodem_terminal_is_open(runtime->terminal);
}

int sippymodem_runtime_terminal_fd(
	const struct sippymodem_runtime *runtime)
{
	return runtime != NULL ?
		       sippymodem_terminal_master_fd(runtime->terminal) :
		       -1;
}

int sippymodem_runtime_terminal_poll_fd(
	const struct sippymodem_runtime *runtime)
{
	size_t capacity;

	if (runtime == NULL ||
	    runtime->last_terminal_result ==
		    SIPPYMODEM_TERMINAL_CLIENT_DISCONNECTED ||
	    sippymodem_slmodem_terminal_input_capacity(
		    &runtime->modem_terminal, &capacity) !=
		    SIPPYMODEM_SLMODEM_TERMINAL_OK ||
	    capacity == 0)
		return -1;
	return sippymodem_terminal_master_fd(runtime->terminal);
}

const char *sippymodem_runtime_device_path(
	const struct sippymodem_runtime *runtime)
{
	return runtime != NULL ?
		       sippymodem_terminal_device_path(runtime->terminal) :
		       NULL;
}

enum sippymodem_host_result sippymodem_runtime_last_host_result(
	const struct sippymodem_runtime *runtime)
{
	return runtime != NULL ? runtime->last_host_result :
			       SIPPYMODEM_HOST_INVALID_ARGUMENT;
}

enum sippymodem_session_result sippymodem_runtime_last_session_result(
	const struct sippymodem_runtime *runtime)
{
	return runtime != NULL ? runtime->last_session_result :
			       SIPPYMODEM_SESSION_INVALID_ARGUMENT;
}

enum sippymodem_terminal_result sippymodem_runtime_last_terminal_result(
	const struct sippymodem_runtime *runtime)
{
	return runtime != NULL ? runtime->last_terminal_result :
			       SIPPYMODEM_TERMINAL_INVALID_ARGUMENT;
}

enum sippymodem_slmodem_calls_result sippymodem_runtime_last_calls_result(
	const struct sippymodem_runtime *runtime)
{
	return runtime != NULL ? runtime->last_calls_result :
			       SIPPYMODEM_SLMODEM_CALLS_INVALID_ARGUMENT;
}

enum sippymodem_slmodem_terminal_result
sippymodem_runtime_last_modem_terminal_result(
	const struct sippymodem_runtime *runtime)
{
	return runtime != NULL ? runtime->last_modem_terminal_result :
			       SIPPYMODEM_SLMODEM_TERMINAL_INVALID_ARGUMENT;
}

const char *sippymodem_runtime_result_name(
	enum sippymodem_runtime_result result)
{
	switch (result) {
	case SIPPYMODEM_RUNTIME_OK:
		return "ok";
	case SIPPYMODEM_RUNTIME_IDLE:
		return "idle";
	case SIPPYMODEM_RUNTIME_MEDIA_BINDING_ERROR:
		return "negotiated media could not be bound";
	case SIPPYMODEM_RUNTIME_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_RUNTIME_TERMINAL_ERROR:
		return "modem terminal failed";
	case SIPPYMODEM_RUNTIME_SESSION_ERROR:
		return "SIP/RTP session failed";
	case SIPPYMODEM_RUNTIME_MODEM_STOP_ERROR:
		return "modem stop failed";
	case SIPPYMODEM_RUNTIME_HOST_ERROR:
		return "SIP modem host failed";
	case SIPPYMODEM_RUNTIME_CALL_ERROR:
		return "modem call transition failed";
	case SIPPYMODEM_RUNTIME_RECORDING_ERROR:
		return "recording evidence failed";
	case SIPPYMODEM_RUNTIME_MODEM_TERMINAL_ERROR:
		return "modem terminal adapter failed";
	}
	return "unknown runtime result";
}
