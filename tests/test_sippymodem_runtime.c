// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_runtime.h>

#include <modem.h>
#include <sippymodem_host.h>
#include <sippymodem_slmodem_adapter.h>
#include <sippymodem_slmodem_calls.h>
#include <sippymodem_slmodem_driver.h>

#include "test.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct sippymodem_host {
	unsigned int marker;
	bool opened;
};

struct sippymodem_recording_files {
	struct sippymodem_recorder recorder;
};

struct runtime_capture {
	unsigned int modem_creates;
	unsigned int modem_deletes;
	unsigned int host_creates;
	unsigned int host_destroys;
	unsigned int recording_creates;
	unsigned int recording_destroys;
	unsigned int driver_attaches;
	unsigned int driver_detaches;
	unsigned int host_opens;
	unsigned int host_closes;
	unsigned int remote_ends;
	unsigned int forced_stops;
	char teardown_order[8];
	size_t teardown_count;
	bool fail_modem;
	bool fail_adapter;
	bool fail_calls;
	bool fail_recording;
	bool fail_recorder;
	bool fail_host;
	bool fail_attach;
	bool host_received_recorder;
	enum sippymodem_session_result host_open_result;
	enum sippymodem_session_result host_close_result;
	int remote_end_result;
	int forced_stop_result;
};

static struct runtime_capture capture;

static void note_teardown(char event)
{
	if (capture.teardown_count + 1 < sizeof(capture.teardown_order)) {
		capture.teardown_order[capture.teardown_count++] = event;
		capture.teardown_order[capture.teardown_count] = '\0';
	}
}

static int unused_driver_start(struct modem *modem)
{
	(void)modem;
	return 0;
}

static int unused_driver_stop(struct modem *modem)
{
	(void)modem;
	return 0;
}

static int unused_driver_control(
	struct modem *modem, unsigned int command, unsigned long argument)
{
	(void)modem;
	(void)command;
	(void)argument;
	return 0;
}

bool sippymodem_slmodem_driver_init(
	struct sippymodem_slmodem_driver *driver)
{
	if (driver == NULL)
		return false;
	memset(driver, 0, sizeof(*driver));
	driver->interface.name = "runtime test driver";
	driver->interface.start = unused_driver_start;
	driver->interface.stop = unused_driver_stop;
	driver->interface.ioctl = unused_driver_control;
	return true;
}

struct modem_driver *sippymodem_slmodem_driver_interface(
	struct sippymodem_slmodem_driver *driver)
{
	return driver != NULL ? &driver->interface : NULL;
}

bool sippymodem_slmodem_driver_attach(
	struct sippymodem_slmodem_driver *driver,
	struct modem *modem,
	struct sippymodem_host *host)
{
	capture.driver_attaches++;
	if (capture.fail_attach)
		return false;
	driver->modem = modem;
	driver->host = host;
	modem->dev_data = driver;
	return true;
}

bool sippymodem_slmodem_driver_detach(
	struct sippymodem_slmodem_driver *driver)
{
	if (driver == NULL || driver->modem == NULL)
		return false;
	capture.driver_detaches++;
	driver->modem->dev_data = NULL;
	driver->modem = NULL;
	driver->host = NULL;
	return true;
}

struct modem *modem_create(struct modem_driver *driver, const char *name)
{
	struct modem *modem;

	capture.modem_creates++;
	if (capture.fail_modem)
		return NULL;
	modem = calloc(1, sizeof(*modem));
	if (modem == NULL)
		return NULL;
	modem->driver = *driver;
	modem->name = name;
	modem->srate = MODEM_RATE;
	modem->format = MODEM_FORMAT;
	modem->frag = MODEM_FRAG;
	return modem;
}

void modem_delete(struct modem *modem)
{
	if (modem == NULL)
		return;
	capture.modem_deletes++;
	note_teardown('M');
	free(modem);
}

int modem_report_remote_call_end(struct modem *modem)
{
	capture.remote_ends++;
	if (capture.remote_end_result == 0)
		modem->started = 0;
	return capture.remote_end_result;
}

int modem_stop_now(struct modem *modem)
{
	capture.forced_stops++;
	modem->started = 0;
	return capture.forced_stop_result;
}

static enum sippymodem_modem_core_result discard_frame(
	void *context, int16_t *receive, int16_t *transmit, size_t count)
{
	(void)context;
	(void)receive;
	memset(transmit, 0, count * sizeof(transmit[0]));
	return SIPPYMODEM_MODEM_CORE_OK;
}

bool sippymodem_slmodem_adapter_init(
	struct sippymodem_slmodem_adapter *adapter, struct modem *modem)
{
	if (adapter == NULL || modem == NULL || capture.fail_adapter)
		return false;
	memset(adapter, 0, sizeof(*adapter));
	adapter->modem = modem;
	adapter->core.context = adapter;
	adapter->core.process = discard_frame;
	return true;
}

const struct sippymodem_modem_core *sippymodem_slmodem_adapter_core(
	const struct sippymodem_slmodem_adapter *adapter)
{
	return adapter != NULL ? &adapter->core : NULL;
}

bool sippymodem_slmodem_calls_init(
	struct sippymodem_slmodem_calls *calls, struct modem *modem)
{
	if (calls == NULL || modem == NULL || capture.fail_calls)
		return false;
	memset(calls, 0, sizeof(*calls));
	calls->modem = modem;
	calls->callbacks.context = calls;
	return true;
}

const struct sippymodem_sip_signaling_callbacks *
sippymodem_slmodem_calls_callbacks(
	const struct sippymodem_slmodem_calls *calls)
{
	return calls != NULL ? &calls->callbacks : NULL;
}

static void discard_recording(
	void *context, const struct sippymodem_recording_block *block)
{
	(void)context;
	(void)block;
}

struct sippymodem_recording_files *sippymodem_recording_files_create(
	const char *prefix)
{
	struct sippymodem_recording_files *files;

	(void)prefix;
	capture.recording_creates++;
	if (capture.fail_recording) {
		errno = EACCES;
		return NULL;
	}
	files = calloc(1, sizeof(*files));
	if (files != NULL)
		files->recorder.write = discard_recording;
	return files;
}

void sippymodem_recording_files_destroy(
	struct sippymodem_recording_files *files)
{
	if (files == NULL)
		return;
	capture.recording_destroys++;
	note_teardown('R');
	free(files);
}

const struct sippymodem_recorder *sippymodem_recording_files_recorder(
	const struct sippymodem_recording_files *files)
{
	return files != NULL && !capture.fail_recorder ? &files->recorder : NULL;
}

struct sippymodem_host *sippymodem_host_create(
	const struct sippymodem_config *config,
	const struct sippymodem_recorder *recorder,
	const struct sippymodem_sip_signaling_callbacks *callbacks,
	const struct sippymodem_modem_core *core)
{
	struct sippymodem_host *host;

	(void)config;
	capture.host_creates++;
	if (capture.fail_host || callbacks == NULL || core == NULL)
		return NULL;
	host = calloc(1, sizeof(*host));
	if (host != NULL) {
		host->marker = 1;
		capture.host_received_recorder = recorder != NULL;
	}
	return host;
}

void sippymodem_host_destroy(struct sippymodem_host *host)
{
	if (host == NULL)
		return;
	capture.host_destroys++;
	note_teardown('H');
	free(host);
}

enum sippymodem_session_result sippymodem_host_open(
	struct sippymodem_host *host)
{
	capture.host_opens++;
	if (capture.host_open_result == SIPPYMODEM_SESSION_OK)
		host->opened = true;
	return capture.host_open_result;
}

enum sippymodem_session_result sippymodem_host_close(
	struct sippymodem_host *host)
{
	capture.host_closes++;
	host->opened = false;
	note_teardown('C');
	return capture.host_close_result;
}

bool sippymodem_host_is_open(const struct sippymodem_host *host)
{
	return host != NULL && host->opened;
}

static struct sippymodem_config runtime_config(bool recording)
{
	struct sippymodem_config config;

	memset(&config, 0, sizeof(config));
	if (recording)
		snprintf(config.recording.prefix,
			 sizeof(config.recording.prefix), "capture");
	return config;
}

static bool test_owns_closed_runtime(void)
{
	struct sippymodem_config config = runtime_config(true);
	struct sippymodem_runtime *runtime;
	struct modem *modem;
	char name[] = "sippymodem-test";

	memset(&capture, 0, sizeof(capture));
	runtime = sippymodem_runtime_create(&config, name);
	modem = sippymodem_runtime_modem(runtime);
	name[0] = 'x';
	CHECK(runtime != NULL && modem != NULL &&
		      strcmp(modem->name, "sippymodem-test") == 0 &&
		      modem->dev_data != NULL && capture.modem_creates == 1 &&
		      capture.host_creates == 1 &&
		      capture.recording_creates == 1 &&
		      capture.host_received_recorder &&
		      sippymodem_runtime_recording_files(runtime) != NULL,
	      "runtime did not own its complete closed graph");
	sippymodem_runtime_destroy(runtime);
	CHECK(strcmp(capture.teardown_order, "HRM") == 0 &&
		      capture.driver_detaches == 1 && capture.host_destroys == 1 &&
		      capture.recording_destroys == 1 &&
		      capture.modem_deletes == 1 &&
		      sippymodem_runtime_modem(NULL) == NULL &&
		      sippymodem_runtime_recording_files(NULL) == NULL,
	      "runtime teardown order was unsafe or incomplete");
	return true;
}

static bool test_creation_rolls_back(void)
{
	struct sippymodem_config config = runtime_config(true);
	char long_name[SIPPYMODEM_RUNTIME_NAME_MAX + 1];

	memset(&capture, 0, sizeof(capture));
	memset(long_name, 'x', sizeof(long_name) - 1);
	long_name[sizeof(long_name) - 1] = '\0';
	CHECK(sippymodem_runtime_create(NULL, "test") == NULL &&
		      errno == EINVAL &&
		      sippymodem_runtime_create(&config, long_name) == NULL &&
		      errno == ENAMETOOLONG,
	      "runtime accepted invalid construction arguments");

	memset(&capture, 0, sizeof(capture));
	capture.fail_adapter = true;
	CHECK(sippymodem_runtime_create(&config, "bad-adapter") == NULL &&
		      capture.modem_deletes == 1 && capture.host_creates == 0,
	      "runtime retained an incompatible modem adapter");
	memset(&capture, 0, sizeof(capture));
	capture.fail_recording = true;
	CHECK(sippymodem_runtime_create(&config, "bad-recording") == NULL &&
		      errno == EACCES && capture.modem_deletes == 1 &&
		      capture.host_creates == 0,
	      "runtime retained owners after recording setup failed");
	memset(&capture, 0, sizeof(capture));
	capture.fail_recorder = true;
	CHECK(sippymodem_runtime_create(&config, "bad-recorder") == NULL &&
		      capture.recording_destroys == 1 &&
		      capture.modem_deletes == 1 && capture.host_creates == 0,
	      "runtime accepted a recording owner without a recorder");
	memset(&capture, 0, sizeof(capture));
	capture.fail_host = true;
	CHECK(sippymodem_runtime_create(&config, "bad-host") == NULL &&
		      capture.recording_destroys == 1 &&
		      capture.modem_deletes == 1,
	      "runtime retained owners after host creation failed");
	memset(&capture, 0, sizeof(capture));
	capture.fail_attach = true;
	CHECK(sippymodem_runtime_create(&config, "bad-attach") == NULL &&
		      capture.host_destroys == 1 &&
		      capture.recording_destroys == 1 &&
		      capture.modem_deletes == 1,
	      "runtime retained owners after driver attachment failed");
	return true;
}

static bool test_opens_and_closes_in_dependency_order(void)
{
	struct sippymodem_config config = runtime_config(false);
	struct sippymodem_runtime *runtime;
	struct modem *modem;

	memset(&capture, 0, sizeof(capture));
	runtime = sippymodem_runtime_create(&config, "lifecycle-test");
	CHECK(runtime != NULL &&
		      sippymodem_runtime_open(runtime) == SIPPYMODEM_RUNTIME_OK &&
		      sippymodem_runtime_open(runtime) == SIPPYMODEM_RUNTIME_OK &&
		      capture.host_opens == 1 && sippymodem_runtime_is_open(runtime),
	      "runtime open was not idempotent");
	modem = sippymodem_runtime_modem(runtime);
	modem->started = 1;
	CHECK(sippymodem_runtime_close(runtime) == SIPPYMODEM_RUNTIME_OK &&
		      capture.remote_ends == 1 && !modem->started &&
		      capture.host_closes == 1 && !sippymodem_runtime_is_open(runtime),
	      "runtime did not stop the modem before closing its host");
	sippymodem_runtime_destroy(runtime);
	CHECK(strcmp(capture.teardown_order, "CHM") == 0 &&
		      capture.host_closes == 1 && capture.host_destroys == 1 &&
		      capture.modem_deletes == 1,
	      "closed runtime repeated teardown work");
	return true;
}

static bool test_reports_lifecycle_failures(void)
{
	struct sippymodem_config config = runtime_config(false);
	struct sippymodem_runtime *runtime;
	struct modem *modem;

	memset(&capture, 0, sizeof(capture));
	capture.host_open_result = SIPPYMODEM_SESSION_SIGNALING_ERROR;
	runtime = sippymodem_runtime_create(&config, "failed-open");
	CHECK(runtime != NULL &&
		      sippymodem_runtime_open(runtime) ==
			      SIPPYMODEM_RUNTIME_SESSION_ERROR &&
		      sippymodem_runtime_last_session_result(runtime) ==
			      SIPPYMODEM_SESSION_SIGNALING_ERROR,
	      "runtime hid a session open failure");
	sippymodem_runtime_destroy(runtime);

	memset(&capture, 0, sizeof(capture));
	capture.host_close_result = SIPPYMODEM_SESSION_SIGNALING_ERROR;
	runtime = sippymodem_runtime_create(&config, "failed-host-close");
	CHECK(runtime != NULL &&
		      sippymodem_runtime_open(runtime) == SIPPYMODEM_RUNTIME_OK &&
		      sippymodem_runtime_close(runtime) ==
			      SIPPYMODEM_RUNTIME_SESSION_ERROR &&
		      sippymodem_runtime_last_session_result(runtime) ==
			      SIPPYMODEM_SESSION_SIGNALING_ERROR,
	      "runtime hid a session close failure");
	sippymodem_runtime_destroy(runtime);

	memset(&capture, 0, sizeof(capture));
	runtime = sippymodem_runtime_create(&config, "failed-stop");
	CHECK(runtime != NULL &&
		      sippymodem_runtime_open(runtime) == SIPPYMODEM_RUNTIME_OK,
	      "failed-stop fixture did not open");
	capture.remote_end_result = -1;
	CHECK(sippymodem_runtime_close(runtime) ==
		      SIPPYMODEM_RUNTIME_MODEM_STOP_ERROR &&
		      capture.host_closes == 1,
	      "runtime hid a modem stop failure or skipped host close");
	sippymodem_runtime_destroy(runtime);

	memset(&capture, 0, sizeof(capture));
	runtime = sippymodem_runtime_create(&config, "forced-stop");
	CHECK(runtime != NULL &&
		      sippymodem_runtime_open(runtime) == SIPPYMODEM_RUNTIME_OK,
	      "forced-stop fixture did not open");
	modem = sippymodem_runtime_modem(runtime);
	modem->started = 1;
	capture.remote_end_result = -1;
	CHECK(sippymodem_runtime_close(runtime) == SIPPYMODEM_RUNTIME_OK &&
		      capture.forced_stops == 1 && !modem->started,
	      "runtime did not recover a deferred modem stop");
	sippymodem_runtime_destroy(runtime);

	memset(&capture, 0, sizeof(capture));
	runtime = sippymodem_runtime_create(&config, "failed-forced-stop");
	CHECK(runtime != NULL &&
		      sippymodem_runtime_open(runtime) == SIPPYMODEM_RUNTIME_OK,
	      "failed forced-stop fixture did not open");
	modem = sippymodem_runtime_modem(runtime);
	modem->started = 1;
	capture.remote_end_result = -1;
	capture.forced_stop_result = -1;
	CHECK(sippymodem_runtime_close(runtime) ==
		      SIPPYMODEM_RUNTIME_MODEM_STOP_ERROR &&
		      !modem->started,
	      "forced-stop error left released modem state runnable");
	sippymodem_runtime_destroy(runtime);
	CHECK(sippymodem_runtime_open(NULL) ==
		      SIPPYMODEM_RUNTIME_INVALID_ARGUMENT &&
		      sippymodem_runtime_close(NULL) ==
			      SIPPYMODEM_RUNTIME_INVALID_ARGUMENT &&
		      !sippymodem_runtime_is_open(NULL),
	      "runtime lifecycle accepted a missing owner");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "closed ownership", test_owns_closed_runtime },
		{ "creation rollback", test_creation_rolls_back },
		{ "open and close", test_opens_and_closes_in_dependency_order },
		{ "lifecycle failures", test_reports_lifecycle_failures },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		printf("[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			printf("[       OK ] %s\n", tests[index].name);
		else {
			printf("[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0 ? 0 : 1;
}
