// SPDX-License-Identifier: BSD-3-Clause

#include <main.h>

#include "test.h"

#include <config.h>
#include <loop.h>
#include <runtime.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

struct modem_runtime {
	int unused;
};

static struct {
	char calls[32];
	size_t call_count;
	int config_result;
	bool create_fails;
	enum modem_runtime_result open_result;
	enum modem_runtime_result close_result;
	enum modem_loop_result loop_result;
	enum modem_runtime_result loop_runtime_result;
	int signal_number;
	bool saw_running;
	bool saw_stop;
	bool wrong_runtime;
	char modem_name[32];
} fixture;

static struct modem_runtime runtime;

static void record_call(char call)
{
	if (fixture.call_count + 1 < sizeof(fixture.calls)) {
		fixture.calls[fixture.call_count++] = call;
		fixture.calls[fixture.call_count] = '\0';
	}
}

static void reset_fixture(void)
{
	memset(&fixture, 0, sizeof(fixture));
}

static int run_program(int argc, char *argv[])
{
	int saved_output = -1;
	int saved_errors = -1;
	int sink = -1;
	int result = -1;

	fflush(NULL);
	sink = open("/dev/null", O_WRONLY | O_CLOEXEC);
	saved_output = dup(STDOUT_FILENO);
	saved_errors = dup(STDERR_FILENO);
	if (sink < 0 || saved_output < 0 || saved_errors < 0)
		goto done;
	if (dup2(sink, STDOUT_FILENO) < 0 || dup2(sink, STDERR_FILENO) < 0)
		goto restore;
	result = sippymodem_program_main(argc, argv);
	fflush(NULL);

restore:
	if (saved_output >= 0)
		(void)dup2(saved_output, STDOUT_FILENO);
	if (saved_errors >= 0)
		(void)dup2(saved_errors, STDERR_FILENO);
done:
	if (sink >= 0)
		close(sink);
	if (saved_output >= 0)
		close(saved_output);
	if (saved_errors >= 0)
		close(saved_errors);
	return result;
}

void config_init(struct app_config *config)
{
	record_call('i');
	memset(config, 0, sizeof(*config));
}

int config_apply_environment(
	struct app_config *config, struct config_error *error)
{
	(void)config;
	record_call('a');
	if (fixture.config_result != 0) {
		strcpy(error->setting, "SIP_PORT");
		strcpy(error->message, "invalid test value");
	}
	return fixture.config_result;
}

struct modem_runtime *modem_runtime_create(
	const struct app_config *config, const char *modem_name)
{
	(void)config;
	record_call('c');
	strncpy(fixture.modem_name, modem_name, sizeof(fixture.modem_name) - 1);
	if (fixture.create_fails) {
		errno = ENOMEM;
		return NULL;
	}
	return &runtime;
}

void modem_runtime_destroy(struct modem_runtime *owned_runtime)
{
	if (owned_runtime != &runtime)
		fixture.wrong_runtime = true;
	record_call('d');
}

enum modem_runtime_result modem_runtime_open(
	struct modem_runtime *owned_runtime)
{
	if (owned_runtime != &runtime)
		fixture.wrong_runtime = true;
	record_call('o');
	return fixture.open_result;
}

enum modem_runtime_result modem_runtime_close(
	struct modem_runtime *owned_runtime)
{
	if (owned_runtime != &runtime)
		fixture.wrong_runtime = true;
	record_call('x');
	return fixture.close_result;
}

const char *modem_runtime_device_path(
	const struct modem_runtime *owned_runtime)
{
	if (owned_runtime != &runtime)
		fixture.wrong_runtime = true;
	record_call('p');
	return "/dev/pts/test";
}

enum modem_loop_result modem_loop_run(
	struct modem_runtime *owned_runtime,
	volatile sig_atomic_t *running,
	struct modem_loop_error *error)
{
	if (owned_runtime != &runtime)
		fixture.wrong_runtime = true;
	record_call('l');
	fixture.saw_running = *running != 0;
	error->runtime_result = fixture.loop_runtime_result;
	error->system_error = fixture.loop_result == MODEM_LOOP_WAIT_ERROR ?
				      EIO :
				      0;
	if (fixture.signal_number != 0)
		(void)raise(fixture.signal_number);
	else
		*running = 0;
	fixture.saw_stop = *running == 0;
	return fixture.loop_result;
}

const char *modem_runtime_result_name(
	enum modem_runtime_result result)
{
	(void)result;
	return "runtime test error";
}

const char *modem_loop_result_name(enum modem_loop_result result)
{
	(void)result;
	return "loop test error";
}

static bool test_runs_owned_lifecycle(void)
{
	char *arguments[] = { "slmodemd", NULL };

	reset_fixture();
	fixture.signal_number = SIGTERM;
	CHECK(run_program(1, arguments) == 0 &&
		      strcmp(fixture.calls, "iacoplxd") == 0 &&
		      strcmp(fixture.modem_name, "sippymodem") == 0 &&
		      fixture.saw_running && fixture.saw_stop &&
		      !fixture.wrong_runtime,
	      "entry point did not run the bounded runtime lifecycle");
	return true;
}

static bool test_validates_invocation_and_configuration(void)
{
	char *help[] = { "slmodemd", "--help", NULL };
	char *unknown[] = { "slmodemd", "--unknown", NULL };
	char *arguments[] = { "slmodemd", NULL };

	reset_fixture();
	CHECK(run_program(2, help) == 0 && fixture.call_count == 0,
	      "help unexpectedly created runtime state");
	CHECK(run_program(2, unknown) == 2 &&
		      fixture.call_count == 0,
	      "unknown arguments were accepted");
	fixture.config_result = -1;
	CHECK(run_program(1, arguments) == 2 &&
		      strcmp(fixture.calls, "ia") == 0,
	      "invalid configuration was not rejected before runtime creation");
	return true;
}

static bool test_unwinds_creation_and_open_failures(void)
{
	char *arguments[] = { "slmodemd", NULL };

	reset_fixture();
	fixture.create_fails = true;
	CHECK(run_program(1, arguments) == 1 &&
		      strcmp(fixture.calls, "iac") == 0,
	      "runtime creation failure was not isolated");
	reset_fixture();
	fixture.open_result = MODEM_RUNTIME_SESSION_ERROR;
	CHECK(run_program(1, arguments) == 1 &&
		      strcmp(fixture.calls, "iacod") == 0,
	      "runtime open failure was not destroyed without looping");
	return true;
}

static bool test_reports_loop_and_close_failures(void)
{
	char *arguments[] = { "slmodemd", NULL };

	reset_fixture();
	fixture.loop_result = MODEM_LOOP_RUNTIME_ERROR;
	fixture.loop_runtime_result = MODEM_RUNTIME_RECORDING_ERROR;
	CHECK(run_program(1, arguments) == 1 &&
		      strcmp(fixture.calls, "iacoplxd") == 0,
	      "runtime failure bypassed ordered shutdown");
	reset_fixture();
	fixture.close_result = MODEM_RUNTIME_SESSION_ERROR;
	CHECK(run_program(1, arguments) == 1 &&
		      strcmp(fixture.calls, "iacoplxd") == 0,
	      "shutdown failure was hidden");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "owned lifecycle", test_runs_owned_lifecycle },
		{ "invocation and configuration",
		  test_validates_invocation_and_configuration },
		{ "creation and open failures",
		  test_unwinds_creation_and_open_failures },
		{ "loop and close failures", test_reports_loop_and_close_failures },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (!tests[index].run()) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0 ? 0 : 1;
}
