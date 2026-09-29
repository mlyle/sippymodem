// SPDX-License-Identifier: BSD-3-Clause

#include <runtime.h>

#include <modem.h>
#include <datapumps.h>
#include <host.h>
#include <slmodem_adapter.h>
#include <slmodem_calls.h>
#include <slmodem_driver.h>

#include "test.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct modem_host {
	unsigned int marker;
	bool opened;
	struct sip_signaling_callbacks callbacks;
};

struct recording_files {
	struct media_recorder recorder;
	enum recording_files_result result;
};

struct runtime_capture {
	unsigned int level_reads;
	struct audio_levels levels;
	unsigned int modem_creates;
	unsigned int modem_deletes;
	unsigned int datapump_starts;
	unsigned int datapump_stops;
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
	unsigned int host_steps;
	unsigned int calls_drained;
	unsigned int termios_updates;
	unsigned int modem_events;
	unsigned int hangups;
	char teardown_order[8];
	size_t teardown_count;
	bool fail_modem;
	bool fail_datapumps;
	bool fail_adapter;
	bool fail_calls;
	bool fail_recording;
	bool fail_recorder;
	bool fail_host;
	bool fail_attach;
	bool host_received_recorder;
	bool emit_call;
	bool call_pending;
	bool drain_never_idle;
	struct sip_call_event call;
	enum modem_host_result host_step_result;
	enum slmodem_calls_result call_result;
	enum sip_session_result host_open_result;
	enum sip_session_result host_close_result;
	int remote_end_result;
	int forced_stop_result;
	uint64_t wait_ns;
	size_t datagrams;
	size_t frames;
	char terminal_input[64];
	size_t terminal_input_count;
	bool short_modem_write;
	struct sip_signaling_callbacks callbacks;
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

enum datapumps_result datapumps_start(
	struct datapumps *datapumps)
{
	capture.datapump_starts++;
	if (datapumps == NULL)
		return DATAPUMPS_INVALID_ARGUMENT;
	if (capture.fail_datapumps)
		return DATAPUMPS_REGISTRATION_CONFLICT;
	datapumps->call_progress_registered = true;
	datapumps->fsk300_registered = true;
	datapumps->v23_registered = true;
	return DATAPUMPS_OK;
}

enum datapumps_result datapumps_stop(
	struct datapumps *datapumps)
{
	capture.datapump_stops++;
	if (datapumps == NULL || !datapumps->call_progress_registered ||
	    !datapumps->fsk300_registered || !datapumps->v23_registered)
		return DATAPUMPS_NOT_STARTED;
	datapumps->call_progress_registered = false;
	datapumps->fsk300_registered = false;
	datapumps->v23_registered = false;
	return DATAPUMPS_OK;
}

bool datapumps_are_started(
	const struct datapumps *datapumps)
{
	return datapumps != NULL && datapumps->call_progress_registered &&
	       datapumps->fsk300_registered && datapumps->v23_registered;
}

bool slmodem_driver_init(
	struct slmodem_driver *driver)
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

struct modem_driver *slmodem_driver_interface(
	struct slmodem_driver *driver)
{
	return driver != NULL ? &driver->interface : NULL;
}

bool slmodem_driver_attach(
	struct slmodem_driver *driver,
	struct modem *modem,
	struct modem_host *host)
{
	capture.driver_attaches++;
	if (capture.fail_attach)
		return false;
	driver->modem = modem;
	driver->host = host;
	modem->dev_data = driver;
	return true;
}

bool slmodem_driver_detach(
	struct slmodem_driver *driver)
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
	modem->xmit.size = sizeof(capture.terminal_input);
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

const char *modem_call_phase(const struct modem *modem)
{
	(void)modem;
	return "training";
}

const char *sip_call_state_name(enum sip_call_state state)
{
	return state == SIP_CALL_CONNECTED ? "connected" : "idle";
}

struct audio_levels modem_host_take_audio_levels(struct modem_host *host)
{
	struct audio_levels levels = capture.levels;
	(void)host;
	capture.level_reads++;
	capture.levels = (struct audio_levels){ 0 };
	return levels;
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

void modem_update_termios(
	struct modem *modem, const struct termios *attributes)
{
	capture.termios_updates++;
	modem->termios = *attributes;
}

int modem_service_event(struct modem *modem)
{
	if (modem->event == 0)
		return 0;
	capture.modem_events++;
	modem->event = 0;
	return 1;
}

void modem_hangup(struct modem *modem)
{
	(void)modem;
	capture.hangups++;
}

int modem_write(struct modem *modem, const char *buffer, int count)
{
	(void)modem;
	if (count < 0 || (size_t)count > sizeof(capture.terminal_input))
		return -1;
	memcpy(capture.terminal_input, buffer, (size_t)count);
	capture.terminal_input_count = (size_t)count;
	return capture.short_modem_write && count > 0 ? count - 1 : count;
}

static enum modem_core_result discard_frame(
	void *context, int16_t *receive, int16_t *transmit, size_t count)
{
	(void)context;
	(void)receive;
	memset(transmit, 0, count * sizeof(transmit[0]));
	return MODEM_CORE_OK;
}

bool slmodem_adapter_init(
	struct slmodem_adapter *adapter, struct modem *modem)
{
	if (adapter == NULL || modem == NULL || capture.fail_adapter)
		return false;
	memset(adapter, 0, sizeof(*adapter));
	adapter->modem = modem;
	adapter->core.context = adapter;
	adapter->core.process = discard_frame;
	return true;
}

const struct modem_core *slmodem_adapter_core(
	const struct slmodem_adapter *adapter)
{
	return adapter != NULL ? &adapter->core : NULL;
}

static void queue_call(
	void *context, const struct sip_call_event *event)
{
	struct slmodem_calls *calls = context;
	if (event != NULL) {
		calls->state = event->state;
		capture.call = *event;
		capture.call_pending = true;
	}
}

bool slmodem_calls_init(
	struct slmodem_calls *calls, struct modem *modem)
{
	if (calls == NULL || modem == NULL || capture.fail_calls)
		return false;
	memset(calls, 0, sizeof(*calls));
	calls->modem = modem;
	calls->callbacks.context = calls;
	calls->callbacks.call_changed = queue_call;
	return true;
}

const struct sip_signaling_callbacks *
slmodem_calls_callbacks(
	const struct slmodem_calls *calls)
{
	return calls != NULL ? &calls->callbacks : NULL;
}

enum slmodem_calls_result slmodem_calls_drain(
	struct slmodem_calls *calls)
{
	(void)calls;
	capture.calls_drained++;
	if (capture.drain_never_idle)
		return SLMODEM_CALLS_OK;
	if (!capture.call_pending)
		return SLMODEM_CALLS_IDLE;
	capture.call_pending = false;
	if (capture.call.direction != SIP_CALL_DIRECTION_INCOMING &&
	    capture.call.direction != SIP_CALL_DIRECTION_OUTGOING)
		return SLMODEM_CALLS_INVALID_EVENT;
	return capture.call_result;
}

static void discard_recording(
	void *context, const struct recording_block *block)
{
	(void)context;
	(void)block;
}

struct recording_files *recording_files_create(
	const char *prefix)
{
	struct recording_files *files;

	(void)prefix;
	capture.recording_creates++;
	if (capture.fail_recording) {
		errno = EACCES;
		return NULL;
	}
	files = calloc(1, sizeof(*files));
	if (files != NULL) {
		files->recorder.write = discard_recording;
		files->result = RECORDING_FILES_OK;
	}
	return files;
}

void recording_files_destroy(
	struct recording_files *files)
{
	if (files == NULL)
		return;
	capture.recording_destroys++;
	note_teardown('R');
	free(files);
}

const struct media_recorder *recording_files_recorder(
	const struct recording_files *files)
{
	return files != NULL && !capture.fail_recorder ? &files->recorder : NULL;
}

enum recording_files_result recording_files_last_result(
	const struct recording_files *files)
{
	return files != NULL ? files->result :
			       RECORDING_FILES_INVALID_BLOCK;
}

struct modem_host *modem_host_create(
	const struct app_config *config,
	const struct media_recorder *recorder,
	const struct sip_signaling_callbacks *callbacks,
	const struct modem_core *core)
{
	struct modem_host *host;

	(void)config;
	capture.host_creates++;
	if (capture.fail_host || callbacks == NULL || core == NULL)
		return NULL;
	host = calloc(1, sizeof(*host));
	if (host != NULL) {
		host->marker = 1;
		host->callbacks = *callbacks;
		capture.callbacks = *callbacks;
		capture.host_received_recorder = recorder != NULL;
	}
	return host;
}

void modem_host_destroy(struct modem_host *host)
{
	if (host == NULL)
		return;
	capture.host_destroys++;
	note_teardown('H');
	free(host);
}

enum sip_session_result modem_host_open(
	struct modem_host *host)
{
	capture.host_opens++;
	if (capture.host_open_result == SIP_SESSION_OK)
		host->opened = true;
	return capture.host_open_result;
}

enum sip_session_result modem_host_close(
	struct modem_host *host)
{
	capture.host_closes++;
	host->opened = false;
	note_teardown('C');
	return capture.host_close_result;
}

bool modem_host_is_open(const struct modem_host *host)
{
	return host != NULL && host->opened;
}

enum modem_host_result modem_host_step(
	struct modem_host *host, uint64_t now_ns,
	size_t *datagrams_received, size_t *frames_processed)
{
	(void)now_ns;
	capture.host_steps++;
	if (datagrams_received != NULL)
		*datagrams_received = capture.datagrams;
	if (frames_processed != NULL)
		*frames_processed = capture.frames;
	if (capture.emit_call) {
		capture.emit_call = false;
		host->callbacks.call_changed(
			host->callbacks.context, &capture.call);
	}
	return capture.host_step_result;
}

uint64_t modem_host_wait_ns(
	const struct modem_host *host, uint64_t now_ns)
{
	(void)host;
	(void)now_ns;
	return capture.wait_ns;
}

int modem_host_media_poll_fd(
	const struct modem_host *host)
{
	(void)host;
	return -1;
}

static struct app_config runtime_config(bool recording)
{
	struct app_config config;

	memset(&config, 0, sizeof(config));
	if (recording)
		snprintf(config.recording.prefix,
			 sizeof(config.recording.prefix), "capture");
	return config;
}

static bool test_owns_closed_runtime(void)
{
	struct app_config config = runtime_config(true);
	struct modem_runtime *runtime;
	struct modem *modem;
	char name[] = "runtime-test";

	memset(&capture, 0, sizeof(capture));
	runtime = modem_runtime_create(&config, name);
	modem = modem_runtime_modem(runtime);
	name[0] = 'x';
	CHECK(runtime != NULL && modem != NULL &&
		      strcmp(modem->name, "runtime-test") == 0 &&
		      modem->dev_data != NULL && capture.datapump_starts == 1 &&
		      capture.modem_creates == 1 &&
		      capture.host_creates == 1 &&
		      capture.recording_creates == 1 &&
		      capture.host_received_recorder &&
		      modem_runtime_recording_files(runtime) != NULL &&
		      modem->pty == -1 &&
		      modem_runtime_terminal_fd(runtime) == -1 &&
		      modem_runtime_device_path(runtime) == NULL,
	      "runtime did not own its complete closed graph");
	modem_runtime_destroy(runtime);
	CHECK(strcmp(capture.teardown_order, "HRM") == 0 &&
		      capture.datapump_stops == 1 &&
		      capture.driver_detaches == 1 && capture.host_destroys == 1 &&
		      capture.recording_destroys == 1 &&
		      capture.modem_deletes == 1 &&
		      modem_runtime_modem(NULL) == NULL &&
		      modem_runtime_recording_files(NULL) == NULL,
	      "runtime teardown order was unsafe or incomplete");
	return true;
}

static bool test_creation_rolls_back(void)
{
	struct app_config config = runtime_config(true);
	char long_name[MODEM_RUNTIME_NAME_MAX + 1];

	memset(&capture, 0, sizeof(capture));
	memset(long_name, 'x', sizeof(long_name) - 1);
	long_name[sizeof(long_name) - 1] = '\0';
	CHECK(modem_runtime_create(NULL, "test") == NULL &&
		      errno == EINVAL &&
		      modem_runtime_create(&config, long_name) == NULL &&
		      errno == ENAMETOOLONG,
	      "runtime accepted invalid construction arguments");

	memset(&capture, 0, sizeof(capture));
	capture.fail_datapumps = true;
	CHECK(modem_runtime_create(&config, "busy-datapumps") == NULL &&
		      errno == EBUSY && capture.datapump_starts == 1 &&
		      capture.datapump_stops == 0 && capture.modem_creates == 0,
	      "runtime hid a data-pump registration conflict");

	memset(&capture, 0, sizeof(capture));
	capture.fail_adapter = true;
	CHECK(modem_runtime_create(&config, "bad-adapter") == NULL &&
		      capture.modem_deletes == 1 && capture.datapump_stops == 1 &&
		      capture.host_creates == 0,
	      "runtime retained an incompatible modem adapter");
	memset(&capture, 0, sizeof(capture));
	capture.fail_recording = true;
	CHECK(modem_runtime_create(&config, "bad-recording") == NULL &&
		      errno == EACCES && capture.modem_deletes == 1 &&
		      capture.host_creates == 0,
	      "runtime retained owners after recording setup failed");
	memset(&capture, 0, sizeof(capture));
	capture.fail_recorder = true;
	CHECK(modem_runtime_create(&config, "bad-recorder") == NULL &&
		      capture.recording_destroys == 1 &&
		      capture.modem_deletes == 1 && capture.host_creates == 0,
	      "runtime accepted a recording owner without a recorder");
	memset(&capture, 0, sizeof(capture));
	capture.fail_host = true;
	CHECK(modem_runtime_create(&config, "bad-host") == NULL &&
		      capture.recording_destroys == 1 &&
		      capture.modem_deletes == 1,
	      "runtime retained owners after host creation failed");
	memset(&capture, 0, sizeof(capture));
	capture.fail_attach = true;
	CHECK(modem_runtime_create(&config, "bad-attach") == NULL &&
		      capture.host_destroys == 1 &&
		      capture.recording_destroys == 1 &&
		      capture.modem_deletes == 1,
	      "runtime retained owners after driver attachment failed");
	return true;
}

static bool test_opens_and_closes_in_dependency_order(void)
{
	struct app_config config = runtime_config(false);
	struct modem_runtime *runtime;
	struct modem *modem;
	const char *device_path;

	memset(&capture, 0, sizeof(capture));
	runtime = modem_runtime_create(&config, "lifecycle-test");
	CHECK(runtime != NULL &&
		      modem_runtime_open(runtime) == MODEM_RUNTIME_OK &&
		      modem_runtime_open(runtime) == MODEM_RUNTIME_OK &&
		      capture.host_opens == 1 && modem_runtime_is_open(runtime),
	      "runtime open was not idempotent");
	modem = modem_runtime_modem(runtime);
	device_path = modem_runtime_device_path(runtime);
	CHECK(device_path != NULL && modem_runtime_terminal_fd(runtime) >= 0 &&
		      modem->pty == modem_runtime_terminal_fd(runtime) &&
		      modem->pty_name == device_path &&
		      modem_runtime_terminal_poll_fd(runtime) == modem->pty &&
		      capture.termios_updates == 1 &&
		      cfgetispeed(&modem->termios) == B115200,
	      "runtime did not bind its terminal to the modem");
	modem->xmit.count = (int)modem->xmit.size;
	CHECK(modem_runtime_terminal_poll_fd(runtime) == -1,
	      "runtime polled terminal input without modem capacity");
	modem->xmit.count = 0;
	modem->started = 1;
	CHECK(modem_runtime_close(runtime) == MODEM_RUNTIME_OK &&
		      capture.remote_ends == 1 && !modem->started &&
		      capture.host_closes == 1 && !modem_runtime_is_open(runtime) &&
		      modem->pty == -1 && modem->pty_name == NULL &&
		      modem_runtime_terminal_fd(runtime) == -1 &&
		      modem_runtime_device_path(runtime) == NULL,
	      "runtime did not stop the modem before closing its host");
	modem_runtime_destroy(runtime);
	CHECK(strcmp(capture.teardown_order, "CHM") == 0 &&
		      capture.host_closes == 1 && capture.host_destroys == 1 &&
		      capture.modem_deletes == 1,
	      "closed runtime repeated teardown work");
	return true;
}

static bool test_reports_lifecycle_failures(void)
{
	struct app_config config = runtime_config(false);
	struct modem_runtime *runtime;
	struct modem *modem;

	memset(&capture, 0, sizeof(capture));
	capture.host_open_result = SIP_SESSION_SIGNALING_ERROR;
	runtime = modem_runtime_create(&config, "failed-open");
	CHECK(runtime != NULL &&
		      modem_runtime_open(runtime) ==
			      MODEM_RUNTIME_SESSION_ERROR &&
		      modem_runtime_last_session_result(runtime) ==
			      SIP_SESSION_SIGNALING_ERROR &&
		      modem_runtime_terminal_fd(runtime) == -1 &&
		      modem_runtime_modem(runtime)->pty == -1,
	      "runtime hid a session open failure");
	modem_runtime_destroy(runtime);

	memset(&capture, 0, sizeof(capture));
	capture.host_close_result = SIP_SESSION_SIGNALING_ERROR;
	runtime = modem_runtime_create(&config, "failed-host-close");
	CHECK(runtime != NULL &&
		      modem_runtime_open(runtime) == MODEM_RUNTIME_OK &&
		      modem_runtime_close(runtime) ==
			      MODEM_RUNTIME_SESSION_ERROR &&
		      modem_runtime_last_session_result(runtime) ==
			      SIP_SESSION_SIGNALING_ERROR,
	      "runtime hid a session close failure");
	modem_runtime_destroy(runtime);

	memset(&capture, 0, sizeof(capture));
	runtime = modem_runtime_create(&config, "failed-stop");
	CHECK(runtime != NULL &&
		      modem_runtime_open(runtime) == MODEM_RUNTIME_OK,
	      "failed-stop fixture did not open");
	capture.remote_end_result = -1;
	CHECK(modem_runtime_close(runtime) ==
		      MODEM_RUNTIME_MODEM_STOP_ERROR &&
		      capture.host_closes == 1,
	      "runtime hid a modem stop failure or skipped host close");
	modem_runtime_destroy(runtime);

	memset(&capture, 0, sizeof(capture));
	runtime = modem_runtime_create(&config, "forced-stop");
	CHECK(runtime != NULL &&
		      modem_runtime_open(runtime) == MODEM_RUNTIME_OK,
	      "forced-stop fixture did not open");
	modem = modem_runtime_modem(runtime);
	modem->started = 1;
	capture.remote_end_result = -1;
	CHECK(modem_runtime_close(runtime) == MODEM_RUNTIME_OK &&
		      capture.forced_stops == 1 && !modem->started,
	      "runtime did not recover a deferred modem stop");
	modem_runtime_destroy(runtime);

	memset(&capture, 0, sizeof(capture));
	runtime = modem_runtime_create(&config, "failed-forced-stop");
	CHECK(runtime != NULL &&
		      modem_runtime_open(runtime) == MODEM_RUNTIME_OK,
	      "failed forced-stop fixture did not open");
	modem = modem_runtime_modem(runtime);
	modem->started = 1;
	capture.remote_end_result = -1;
	capture.forced_stop_result = -1;
	CHECK(modem_runtime_close(runtime) ==
		      MODEM_RUNTIME_MODEM_STOP_ERROR &&
		      !modem->started,
	      "forced-stop error left released modem state runnable");
	modem_runtime_destroy(runtime);
	CHECK(modem_runtime_open(NULL) ==
		      MODEM_RUNTIME_INVALID_ARGUMENT &&
		      modem_runtime_close(NULL) ==
			      MODEM_RUNTIME_INVALID_ARGUMENT &&
		      !modem_runtime_is_open(NULL) &&
		      modem_runtime_terminal_fd(NULL) == -1 &&
		      modem_runtime_terminal_poll_fd(NULL) == -1 &&
		      modem_runtime_media_poll_fd(NULL) == -1 &&
		      modem_runtime_device_path(NULL) == NULL &&
		      modem_runtime_last_terminal_result(NULL) ==
			      PTY_TERMINAL_INVALID_ARGUMENT,
	      "runtime lifecycle accepted a missing owner");
	return true;
}

static bool test_steps_bounded_host_and_evidence_work(void)
{
	struct app_config config = runtime_config(true);
	struct modem_runtime *runtime;
	struct recording_files *recording;
	struct modem *modem;
	struct termios attributes;
	static const char command[] = "AT\r";
	size_t terminal_bytes = 99;
	size_t datagrams = 99;
	size_t frames = 99;
	int device_fd;

	memset(&capture, 0, sizeof(capture));
	capture.wait_ns = 7654321;
	runtime = modem_runtime_create(&config, "step-test");
	CHECK(runtime != NULL &&
		      modem_runtime_open(runtime) == MODEM_RUNTIME_OK,
	      "runtime step fixture did not open");
	modem = modem_runtime_modem(runtime);
	device_fd = open(modem_runtime_device_path(runtime),
			 O_RDWR | O_NOCTTY);
	CHECK(device_fd >= 0 && tcgetattr(device_fd, &attributes) == 0,
	      "runtime terminal client did not open");
	attributes.c_cflag |= HUPCL;
	CHECK(tcsetattr(device_fd, TCSANOW, &attributes) == 0 &&
		      write(device_fd, command, sizeof(command) - 1) ==
			      (ssize_t)sizeof(command) - 1,
	      "runtime terminal command fixture was not ready");
	recording = (struct recording_files *)
		modem_runtime_recording_files(runtime);
	capture.host_step_result = MODEM_HOST_OK;
	capture.datagrams = 3;
	capture.frames = 2;
	capture.emit_call = true;
	capture.call.direction = SIP_CALL_DIRECTION_OUTGOING;
	capture.call.state = SIP_CALL_CONNECTED;
	modem->event = 1;
	CHECK(modem_runtime_step(
		      runtime, 100, &terminal_bytes, &datagrams, &frames) ==
		      MODEM_RUNTIME_OK &&
		      terminal_bytes == sizeof(command) - 1 && datagrams == 3 &&
		      frames == 2 && capture.host_steps == 1 &&
		      capture.modem_events == 1 &&
		      capture.terminal_input_count == sizeof(command) - 1 &&
		      memcmp(capture.terminal_input, command,
			     sizeof(command) - 1) == 0 &&
		      capture.calls_drained == 2 &&
		      modem_runtime_last_host_result(runtime) ==
			      MODEM_HOST_OK &&
		      modem_runtime_last_calls_result(runtime) ==
			      SLMODEM_CALLS_IDLE &&
		      modem_runtime_wait_ns(runtime, 100) == capture.wait_ns,
	      "runtime did not advance and drain bounded host work");

	capture.host_step_result = MODEM_HOST_IDLE;
	CHECK(modem_runtime_step(runtime, 101, NULL, NULL, NULL) ==
		      MODEM_RUNTIME_IDLE,
	      "idle host work was not distinguished");
	capture.host_step_result = MODEM_HOST_MEDIA_BINDING_ERROR;
	CHECK(modem_runtime_step(runtime, 102, NULL, NULL, NULL) ==
		      MODEM_RUNTIME_MEDIA_BINDING_ERROR,
	      "runtime hid rejected negotiated media");
	capture.host_step_result = MODEM_HOST_AUDIO_ERROR;
	CHECK(modem_runtime_step(runtime, 103, NULL, NULL, NULL) ==
		      MODEM_RUNTIME_HOST_ERROR,
	      "runtime hid a host audio failure");
	capture.host_step_result = MODEM_HOST_MODEM_STOPPED;
	CHECK(modem_runtime_step(runtime, 104, NULL, NULL, NULL) ==
		      MODEM_RUNTIME_OK &&
		      modem_runtime_last_host_result(runtime) ==
			      MODEM_HOST_MODEM_STOPPED,
	      "runtime treated a handled modem stop as fatal");
	capture.host_step_result = MODEM_HOST_MEDIA_TIMEOUT;
	modem->started = 1;
	unsigned int remote_ends = capture.remote_ends;

	CHECK(modem_runtime_step(runtime, 104, NULL, NULL, NULL) == MODEM_RUNTIME_OK &&
	      capture.remote_ends == remote_ends + 1 && !modem->started,
	      "media timeout did not stop the modem while preserving the terminal");
	capture.remote_end_result = -1;
	CHECK(modem_runtime_step(runtime, 104, NULL, NULL, NULL) ==
		      MODEM_RUNTIME_MODEM_STOP_ERROR,
	      "media timeout hid a modem cleanup failure");
	capture.remote_end_result = 0;

	capture.host_step_result = MODEM_HOST_IDLE;
	capture.emit_call = true;
	capture.call.direction = (enum sip_call_direction)99;
	CHECK(modem_runtime_step(runtime, 105, NULL, NULL, NULL) ==
		      MODEM_RUNTIME_CALL_ERROR &&
		      modem_runtime_last_calls_result(runtime) ==
			      SLMODEM_CALLS_INVALID_EVENT,
	      "runtime hid an invalid call transition");
	recording->result = RECORDING_FILES_WRITE_ERROR;
	CHECK(modem_runtime_step(runtime, 106, NULL, NULL, NULL) ==
		      MODEM_RUNTIME_RECORDING_ERROR,
	      "runtime continued after recording evidence failed");
	recording->result = RECORDING_FILES_OK;
	capture.calls_drained = 0;
	capture.drain_never_idle = true;
	CHECK(modem_runtime_step(runtime, 107, NULL, NULL, NULL) ==
		      MODEM_RUNTIME_CALL_ERROR &&
		      capture.calls_drained ==
			      MODEM_RUNTIME_MAX_CALL_TRANSITIONS,
	      "runtime call draining was not bounded");
	capture.drain_never_idle = false;

	capture.short_modem_write = true;
	CHECK(write(device_fd, command, sizeof(command) - 1) ==
		      (ssize_t)sizeof(command) - 1 &&
		      modem_runtime_step(runtime, 108, NULL, NULL, NULL) ==
			      MODEM_RUNTIME_MODEM_TERMINAL_ERROR &&
		      modem_runtime_last_modem_terminal_result(runtime) ==
			      SLMODEM_TERMINAL_INPUT_ERROR,
	      "runtime hid partially accepted terminal input");
	capture.short_modem_write = false;
	close(device_fd);
	CHECK(modem_runtime_step(runtime, 109, NULL, NULL, NULL) ==
		      MODEM_RUNTIME_OK &&
		      capture.hangups == 1 &&
		      modem_runtime_terminal_poll_fd(runtime) == -1,
	      "runtime did not apply terminal HUPCL on disconnect");

	terminal_bytes = 99;
	datagrams = 99;
	frames = 99;
	CHECK(modem_runtime_step(
		      NULL, 0, &terminal_bytes, &datagrams, &frames) ==
		      MODEM_RUNTIME_INVALID_ARGUMENT &&
		      terminal_bytes == 0 && datagrams == 0 && frames == 0 &&
		      modem_runtime_wait_ns(NULL, 0) == UINT64_MAX &&
		      modem_runtime_last_host_result(NULL) ==
			      MODEM_HOST_INVALID_ARGUMENT &&
		      modem_runtime_last_calls_result(NULL) ==
			      SLMODEM_CALLS_INVALID_ARGUMENT &&
		      modem_runtime_last_modem_terminal_result(NULL) ==
			      SLMODEM_TERMINAL_INVALID_ARGUMENT &&
		      strcmp(modem_runtime_result_name(
			     MODEM_RUNTIME_RECORDING_ERROR),
			     "recording evidence failed") == 0,
	      "runtime step diagnostics were incomplete");
	modem_runtime_destroy(runtime);
	return true;
}

static bool test_opt_in_signaling_trace(void)
{
	static const char *const messages[] = {
		"SIP/2.0 200 OK\r\nContent-Length: 0\r\n",
		"INVITE sip:peer@example.net SIP/2.0\r\n"
		"pRoXy-AuThOrIzAtIoN: Digest\r\n\tresponse=secret\r\n",
		"Authorization: secret",
		"Authentication-Info: secret",
		"WWW-Authenticate: Digest nonce=secret",
		"password=secret", "credential=secret",
		"ready \033[31m\001",
	};
	struct app_config config = runtime_config(false);
	struct modem_runtime *runtime;
	struct sip_trace_record record = { .origin = SIP_TRACE_LIBRARY, .level = 4 };
	char oversized[8193] = { 0 };
	char output[1024] = { 0 };
	FILE *stream;
	size_t output_size;
	int saved_stderr;
	int restored;

	memset(&capture, 0, sizeof(capture));
	runtime = modem_runtime_create(&config, "trace-test");
	CHECK(runtime != NULL && capture.callbacks.trace == NULL,
	      "signaling trace is not opt-in");
	modem_runtime_destroy(runtime);
	config.sip.trace_enabled = true;
	runtime = modem_runtime_create(&config, "trace-test");
	CHECK(runtime != NULL && capture.callbacks.trace != NULL,
	      "trace callback was not installed");
	stream = tmpfile();
	CHECK(stream != NULL, "trace capture failed");
	fflush(stderr);
	saved_stderr = dup(STDERR_FILENO);
	CHECK(saved_stderr >= 0 && dup2(fileno(stream), STDERR_FILENO) >= 0,
	      "cannot redirect trace output");
	for (size_t index = 0; index < ARRAY_LENGTH(messages); ++index) {
		record.data = messages[index];
		record.size = strlen(record.data);
		capture.callbacks.trace(capture.callbacks.context, &record);
	}
	record.data = oversized;
	record.size = sizeof(oversized);
	capture.callbacks.trace(capture.callbacks.context, &record);
	fflush(stderr);
	restored = dup2(saved_stderr, STDERR_FILENO);
	close(saved_stderr);
	CHECK(restored >= 0, "cannot restore stderr");
	rewind(stream);
	output_size = fread(output, 1, sizeof(output) - 1, stream);
	fclose(stream);
	modem_runtime_destroy(runtime);
	CHECK(output_size != 0, "trace emitted no output");
	CHECK(strstr(output, "SIP/2.0 200 OK\nContent-Length: 0\n") != NULL &&
	      strstr(output, "authentication record omitted") != NULL &&
	      strstr(output, "oversized record omitted") != NULL &&
	      strstr(output, "ready ?[31m?\n") != NULL,
	      "trace omitted useful text or failed to sanitize controls");
	CHECK(strstr(output, "secret") == NULL &&
	      strchr(output, '\033') == NULL && strchr(output, '\r') == NULL,
	      "trace exposed credentials or terminal control bytes");
	return true;
}

static bool test_call_trace_is_rate_limited(void)
{
	struct app_config config = runtime_config(false);
	struct modem_runtime *runtime;
	char output[2048] = { 0 };
	FILE *stream = tmpfile();
	int saved_stderr;
	unsigned int reads;

	CHECK(stream != NULL, "call trace capture failed");
	memset(&capture, 0, sizeof(capture));
	runtime = modem_runtime_create(&config, "untraced");
	CHECK(runtime != NULL && modem_runtime_open(runtime) == MODEM_RUNTIME_OK,
	      "disabled trace fixture failed");
	(void)modem_runtime_step(runtime, 0, NULL, NULL, NULL);
	CHECK(capture.level_reads == 0, "disabled trace collected levels");
	modem_runtime_destroy(runtime);

	memset(&capture, 0, sizeof(capture));
	config.sip.trace_enabled = true;
	runtime = modem_runtime_create(&config, "call-trace");
	CHECK(runtime != NULL && modem_runtime_open(runtime) == MODEM_RUNTIME_OK,
	      "call trace fixture failed");
	fflush(stderr);
	saved_stderr = dup(STDERR_FILENO);
	CHECK(saved_stderr >= 0 && dup2(fileno(stream), STDERR_FILENO) >= 0,
	      "cannot redirect call trace");
	capture.levels = (struct audio_levels){ .receive_peak = 1000, .transmit_peak = 32768 };
	(void)modem_runtime_step(runtime, 0, NULL, NULL, NULL);
	capture.call = (struct sip_call_event){ .state = SIP_CALL_CONNECTED };
	capture.emit_call = true;
	(void)modem_runtime_step(runtime, 999999999, NULL, NULL, NULL);
	(void)modem_runtime_step(runtime, 1000000000, NULL, NULL, NULL);
	(void)modem_runtime_step(runtime, 1000000000, NULL, NULL, NULL);
	(void)modem_runtime_step(runtime, 0, NULL, NULL, NULL);
	reads = capture.level_reads;
	fflush(stderr);
	int restored = dup2(saved_stderr, STDERR_FILENO);
	close(saved_stderr);
	CHECK(restored >= 0, "cannot restore call trace output");
	rewind(stream);
	size_t output_size = fread(output, 1, sizeof(output) - 1, stream);
	fclose(stream);
	modem_runtime_destroy(runtime);
	CHECK(reads == 2, "call trace exceeded once per second or missed the boundary");
	CHECK(output_size != 0, "call trace emitted no output");
	CHECK(strstr(output, "CALL sip=idle phase=training datapump=none TX-peak=32768/32768 RX-peak=1000/32768") &&
	      strstr(output, "CALL sip=connected phase=training datapump=none TX-peak=0/32768 RX-peak=0/32768"),
	      "call trace lost state or retained peaks when no frames arrived");
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
		{ "bounded host step", test_steps_bounded_host_and_evidence_work },
		{ "opt-in signaling trace", test_opt_in_signaling_trace },
		{ "rate-limited call trace", test_call_trace_is_rate_limited },
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
