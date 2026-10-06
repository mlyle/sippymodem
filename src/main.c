/*
 *
 *    Copyright (c) 2002, Smart Link Ltd.
 *    All rights reserved.
 *
 *    Redistribution and use in source and binary forms, with or without
 *    modification, are permitted provided that the following conditions
 *    are met:
 *
 *        1. Redistributions of source code must retain the above copyright
 *           notice, this list of conditions and the following disclaimer.
 *        2. Redistributions in binary form must reproduce the above
 *           copyright notice, this list of conditions and the following
 *           disclaimer in the documentation and/or other materials provided
 *           with the distribution.
 *        3. Neither the name of the Smart Link Ltd. nor the names of its
 *           contributors may be used to endorse or promote products derived
 *           from this software without specific prior written permission.
 *
 *    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *    "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *    A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *    OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "main.h"

#include "config.h"
#include "log_target.h"
#include "loop.h"
#include "runtime.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

enum {
	PROGRAM_EXIT_OK = 0,
	PROGRAM_EXIT_FAILURE = 1,
	PROGRAM_EXIT_USAGE = 2,
};

struct signal_handlers {
	struct sigaction interrupt;
	struct sigaction terminate;
	bool interrupt_installed;
	bool terminate_installed;
};

static volatile sig_atomic_t running;

static const char *program_label(const char *program)
{
	const char *separator;

	if (program == NULL || program[0] == '\0')
		return "modem";
	separator = strrchr(program, '/');
	return separator != NULL && separator[1] != '\0' ?
		       separator + 1 :
		       program;
}

static void request_stop(int signal_number)
{
	(void)signal_number;
	running = 0;
}

static bool install_signal_handlers(struct signal_handlers *handlers)
{
	struct sigaction action;
	int failure;

	memset(handlers, 0, sizeof(*handlers));
	memset(&action, 0, sizeof(action));
	action.sa_handler = request_stop;
	if (sigemptyset(&action.sa_mask) != 0 ||
	    sigaction(SIGINT, &action, &handlers->interrupt) != 0)
		return false;
	handlers->interrupt_installed = true;
	if (sigaction(SIGTERM, &action, &handlers->terminate) == 0) {
		handlers->terminate_installed = true;
		return true;
	}
	failure = errno;
	(void)sigaction(SIGINT, &handlers->interrupt, NULL);
	handlers->interrupt_installed = false;
	errno = failure;
	return false;
}

static void restore_signal_handlers(const struct signal_handlers *handlers)
{
	if (handlers->terminate_installed)
		(void)sigaction(SIGTERM, &handlers->terminate, NULL);
	if (handlers->interrupt_installed)
		(void)sigaction(SIGINT, &handlers->interrupt, NULL);
}

static void print_usage(FILE *stream, const char *program)
{
	fprintf(stream,
		"Usage: %s [--help] [--log-target stderr|syslog]\n"
		"Run the modem in the foreground; SIP_* environment variables "
		"provide configuration; MODEM_LOG_TARGET selects log output.\n",
		program != NULL ? program : "slmodemd");
}

static void report_loop_error(
	const char *label,
	enum modem_loop_result result,
	const struct modem_loop_error *error)
{
	if (result == MODEM_LOOP_RUNTIME_ERROR) {
		log_target_printf(LOG_ERR, "%s: %s\n", label,
			modem_runtime_result_name(error->runtime_result));
	} else if (error->system_error != 0) {
		log_target_printf(LOG_ERR, "%s: %s: %s\n", label,
			modem_loop_result_name(result),
			strerror(error->system_error));
	} else {
		log_target_printf(LOG_ERR, "%s: %s\n", label,
			modem_loop_result_name(result));
	}
}

int program_main(int argc, char *argv[])
{
	struct app_config config;
	struct config_error config_error;
	struct modem_loop_error loop_error;
	struct signal_handlers handlers;
	struct modem_runtime *runtime;
	enum modem_runtime_result open_result;
	enum modem_runtime_result close_result;
	enum modem_loop_result loop_result;
	const char *program = argv != NULL && argc > 0 ? argv[0] : NULL;
	const char *label = program_label(program);
	const char *device_path;
	int failure;
	int exit_code = PROGRAM_EXIT_OK;
	enum log_target cli_target = LOG_TARGET_STDERR;
	bool have_cli_target = false;

	if (argc == 2 && argv != NULL && argv[1] != NULL &&
	    strcmp(argv[1], "--help") == 0) {
		print_usage(stdout, program);
		return PROGRAM_EXIT_OK;
	}
	if (argv != NULL && argc == 3 && argv[1] != NULL &&
	    strcmp(argv[1], "--log-target") == 0)
		have_cli_target = log_target_parse(argv[2], &cli_target);
	else if (argv != NULL && argc == 2 && argv[1] != NULL &&
	         strncmp(argv[1], "--log-target=", 13) == 0)
		have_cli_target = log_target_parse(argv[1] + 13, &cli_target);
	if ((argc != 1 && !have_cli_target) || argv == NULL || argv[0] == NULL) {
		print_usage(stderr, program);
		return PROGRAM_EXIT_USAGE;
	}

	config_init(&config);
	if (config_apply_environment(&config, &config_error) != 0) {
		log_target_printf(LOG_ERR, "%s: %s: %s\n", label, config_error.setting,
			config_error.message);
		return PROGRAM_EXIT_USAGE;
	}
	if (have_cli_target)
		config.log_target = cli_target;
	log_target_open(config.log_target, label);
	running = 1;
	if (!install_signal_handlers(&handlers)) {
		log_target_printf(LOG_ERR, "%s: cannot install signal handlers: %s\n",
			label, strerror(errno));
		log_target_close();
		return PROGRAM_EXIT_FAILURE;
	}

	errno = 0;
	runtime = modem_runtime_create(&config, label);
	if (runtime == NULL) {
		failure = errno != 0 ? errno : EINVAL;
		log_target_printf(LOG_ERR, "%s: cannot create runtime: %s\n", label,
			strerror(failure));
		restore_signal_handlers(&handlers);
		log_target_close();
		return PROGRAM_EXIT_FAILURE;
	}
	open_result = modem_runtime_open(runtime);
	if (open_result != MODEM_RUNTIME_OK) {
		log_target_printf(LOG_ERR, "%s: cannot open runtime: %s\n", label,
			modem_runtime_result_name(open_result));
		exit_code = PROGRAM_EXIT_FAILURE;
		goto destroy;
	}
	device_path = modem_runtime_device_path(runtime);
	if (device_path == NULL || fprintf(stdout, "%s\n", device_path) < 0 ||
	    fflush(stdout) != 0) {
		failure = errno != 0 ? errno : EIO;
		log_target_printf(LOG_ERR, "%s: cannot publish modem device: %s\n", label,
			strerror(failure));
		exit_code = PROGRAM_EXIT_FAILURE;
		goto close;
	}
	loop_result = modem_loop_run(runtime, &running, &loop_error);
	if (loop_result != MODEM_LOOP_OK) {
		report_loop_error(label, loop_result, &loop_error);
		exit_code = PROGRAM_EXIT_FAILURE;
	}

close:
	close_result = modem_runtime_close(runtime);
	if (close_result != MODEM_RUNTIME_OK) {
		log_target_printf(LOG_ERR, "%s: cannot close runtime: %s\n", label,
			modem_runtime_result_name(close_result));
		exit_code = PROGRAM_EXIT_FAILURE;
	}

destroy:
	modem_runtime_destroy(runtime);
	restore_signal_handlers(&handlers);
	log_target_close();
	return exit_code;
}

#ifndef PROGRAM_NO_PROCESS_ENTRY
int main(int argc, char *argv[])
{
	return program_main(argc, argv);
}
#endif
