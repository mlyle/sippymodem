// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_runtime.h"

#include "modem.h"
#include "sippymodem_host.h"
#include "sippymodem_slmodem_adapter.h"
#include "sippymodem_slmodem_calls.h"
#include "sippymodem_slmodem_driver.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct sippymodem_runtime {
	char modem_name[SIPPYMODEM_RUNTIME_NAME_MAX];
	struct sippymodem_slmodem_driver driver;
	struct modem *modem;
	struct sippymodem_slmodem_adapter adapter;
	struct sippymodem_slmodem_calls calls;
	struct sippymodem_recording_files *recording_files;
	struct sippymodem_host *host;
	enum sippymodem_session_result last_session_result;
};

static void destroy_incomplete_runtime(struct sippymodem_runtime *runtime)
{
	if (runtime == NULL)
		return;
	sippymodem_host_destroy(runtime->host);
	sippymodem_recording_files_destroy(runtime->recording_files);
	if (runtime->modem != NULL)
		modem_delete(runtime->modem);
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
	if (!sippymodem_slmodem_driver_init(&runtime->driver))
		goto fail;
	modem_driver = sippymodem_slmodem_driver_interface(&runtime->driver);
	if (modem_driver == NULL)
		goto fail;
	runtime->modem = modem_create(modem_driver, runtime->modem_name);
	if (runtime->modem == NULL ||
	    !sippymodem_slmodem_adapter_init(
		    &runtime->adapter, runtime->modem) ||
	    !sippymodem_slmodem_calls_init(&runtime->calls, runtime->modem))
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
	if (runtime == NULL)
		return SIPPYMODEM_RUNTIME_INVALID_ARGUMENT;
	if (sippymodem_host_is_open(runtime->host))
		return SIPPYMODEM_RUNTIME_OK;
	runtime->last_session_result = sippymodem_host_open(runtime->host);
	return runtime->last_session_result == SIPPYMODEM_SESSION_OK ?
		       SIPPYMODEM_RUNTIME_OK :
		       SIPPYMODEM_RUNTIME_SESSION_ERROR;
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
	if (stop_result != 0)
		return SIPPYMODEM_RUNTIME_MODEM_STOP_ERROR;
	return runtime->last_session_result == SIPPYMODEM_SESSION_OK ||
		       runtime->last_session_result == SIPPYMODEM_SESSION_NOT_OPEN ?
		       SIPPYMODEM_RUNTIME_OK :
		       SIPPYMODEM_RUNTIME_SESSION_ERROR;
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
	modem_delete(runtime->modem);
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
	return runtime != NULL && sippymodem_host_is_open(runtime->host);
}

enum sippymodem_session_result sippymodem_runtime_last_session_result(
	const struct sippymodem_runtime *runtime)
{
	return runtime != NULL ? runtime->last_session_result :
			       SIPPYMODEM_SESSION_INVALID_ARGUMENT;
}

const char *sippymodem_runtime_result_name(
	enum sippymodem_runtime_result result)
{
	switch (result) {
	case SIPPYMODEM_RUNTIME_OK:
		return "ok";
	case SIPPYMODEM_RUNTIME_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_RUNTIME_SESSION_ERROR:
		return "SIP/RTP session failed";
	case SIPPYMODEM_RUNTIME_MODEM_STOP_ERROR:
		return "modem stop failed";
	}
	return "unknown runtime result";
}
