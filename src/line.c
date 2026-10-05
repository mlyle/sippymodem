// SPDX-License-Identifier: BSD-3-Clause
#include "line.h"

#include "modem.h"
#include "slmodem_adapter.h"
#include "slmodem_driver.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

enum { MODEM_LINE_NAME_MAX = 64 };

struct modem_line {
	char name[MODEM_LINE_NAME_MAX];
	struct modem *modem;
	struct modem_host *host;
	struct recording_files *recording;
	struct slmodem_driver driver;
	struct slmodem_adapter adapter;
	struct slmodem_calls calls;
};

struct modem_line *modem_line_create(const struct app_config *config,
	const char *name, void (*trace)(void *, const struct sip_trace_record *))
{
	if (config == NULL || name == NULL || name[0] == '\0') {
		errno = EINVAL;
		return NULL;
	}
	size_t length = strlen(name);
	if (length >= MODEM_LINE_NAME_MAX) {
		errno = ENAMETOOLONG;
		return NULL;
	}
	struct modem_line *line = calloc(1, sizeof(*line));
	if (line == NULL)
		return NULL;
	memcpy(line->name, name, length + 1);
	const struct media_recorder *recorder = NULL;
	if (!slmodem_driver_init(&line->driver))
		goto fail;
	struct modem_driver *driver = slmodem_driver_interface(&line->driver);
	if (driver == NULL)
		goto fail;
	line->modem = modem_create(driver, line->name);
	if (line->modem == NULL || !slmodem_adapter_init(&line->adapter, line->modem) ||
	    !slmodem_calls_init(&line->calls, line->modem))
		goto fail;
	line->modem->pcm_codec_grid = config->media.pcm_codec_grid;
	if (config->recording.prefix[0] != '\0') {
		line->recording = recording_files_create(config->recording.prefix);
		if (line->recording == NULL ||
		    (recorder = recording_files_recorder(line->recording)) == NULL)
			goto fail;
	}
	struct sip_signaling_callbacks callbacks = *slmodem_calls_callbacks(&line->calls);
	callbacks.trace = trace;
	line->host = modem_host_create(config, recorder, &callbacks,
		slmodem_adapter_core(&line->adapter));
	if (line->host == NULL ||
	    !slmodem_driver_attach(&line->driver, line->modem, line->host))
		goto fail;
	return line;

fail:;
	int saved_error = errno != 0 ? errno : EINVAL;
	modem_line_destroy(line);
	errno = saved_error;
	return NULL;
}

void modem_line_destroy(struct modem_line *line)
{
	if (line == NULL)
		return;
	if (line->modem != NULL && line->modem->started)
		(void)modem_stop_now(line->modem);
	if (line->modem != NULL && line->modem->dev_data == &line->driver)
		(void)slmodem_driver_detach(&line->driver);
	modem_host_destroy(line->host);
	recording_files_destroy(line->recording);
	if (line->modem != NULL)
		modem_delete(line->modem);
	free(line);
}

struct modem *modem_line_modem(struct modem_line *line)
{
	return line != NULL ? line->modem : NULL;
}

struct modem_host *modem_line_host(struct modem_line *line)
{
	return line != NULL ? line->host : NULL;
}

struct slmodem_calls *modem_line_calls(struct modem_line *line)
{
	return line != NULL ? &line->calls : NULL;
}

const struct recording_files *modem_line_recording(const struct modem_line *line)
{
	return line != NULL ? line->recording : NULL;
}
