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
#include "loop.h"
#include "runtime.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum {
	SIPPYMODEM_EXIT_OK = 0,
	SIPPYMODEM_EXIT_FAILURE = 1,
	SIPPYMODEM_EXIT_USAGE = 2,
};

struct signal_handlers {
	struct sigaction interrupt;
	struct sigaction terminate;
	bool interrupt_installed;
	bool terminate_installed;
};

static volatile sig_atomic_t running;

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
		"Usage: %s [--help]\n"
		"Run sippymodem in the foreground; SIP_* environment variables "
		"provide configuration.\n",
		program != NULL ? program : "slmodemd");
}

static void report_loop_error(
	enum sippymodem_loop_result result,
	const struct sippymodem_loop_error *error)
{
	if (result == SIPPYMODEM_LOOP_RUNTIME_ERROR) {
		fprintf(stderr, "sippymodem: %s\n",
			sippymodem_runtime_result_name(error->runtime_result));
	} else if (error->system_error != 0) {
		fprintf(stderr, "sippymodem: %s: %s\n",
			sippymodem_loop_result_name(result),
			strerror(error->system_error));
	} else {
		fprintf(stderr, "sippymodem: %s\n",
			sippymodem_loop_result_name(result));
	}
}

int sippymodem_program_main(int argc, char *argv[])
{
	struct sippymodem_config config;
	struct sippymodem_config_error config_error;
	struct sippymodem_loop_error loop_error;
	struct signal_handlers handlers;
	struct sippymodem_runtime *runtime;
	enum sippymodem_runtime_result open_result;
	enum sippymodem_runtime_result close_result;
	enum sippymodem_loop_result loop_result;
	const char *program = argv != NULL && argc > 0 ? argv[0] : NULL;
	const char *device_path;
	int failure;
	int exit_code = SIPPYMODEM_EXIT_OK;

	if (argc == 2 && argv != NULL && argv[1] != NULL &&
	    strcmp(argv[1], "--help") == 0) {
		print_usage(stdout, program);
		return SIPPYMODEM_EXIT_OK;
	}
	if (argc != 1 || argv == NULL || argv[0] == NULL) {
		print_usage(stderr, program);
		return SIPPYMODEM_EXIT_USAGE;
	}

	sippymodem_config_init(&config);
	if (sippymodem_config_apply_environment(&config, &config_error) != 0) {
		fprintf(stderr, "sippymodem: %s: %s\n", config_error.setting,
			config_error.message);
		return SIPPYMODEM_EXIT_USAGE;
	}
	running = 1;
	if (!install_signal_handlers(&handlers)) {
		fprintf(stderr, "sippymodem: cannot install signal handlers: %s\n",
			strerror(errno));
		return SIPPYMODEM_EXIT_FAILURE;
	}

	errno = 0;
	runtime = sippymodem_runtime_create(&config, "sippymodem");
	if (runtime == NULL) {
		failure = errno != 0 ? errno : EINVAL;
		fprintf(stderr, "sippymodem: cannot create runtime: %s\n",
			strerror(failure));
		restore_signal_handlers(&handlers);
		return SIPPYMODEM_EXIT_FAILURE;
	}
	open_result = sippymodem_runtime_open(runtime);
	if (open_result != SIPPYMODEM_RUNTIME_OK) {
		fprintf(stderr, "sippymodem: cannot open runtime: %s\n",
			sippymodem_runtime_result_name(open_result));
		exit_code = SIPPYMODEM_EXIT_FAILURE;
		goto destroy;
	}
	device_path = sippymodem_runtime_device_path(runtime);
	if (device_path == NULL || fprintf(stdout, "%s\n", device_path) < 0 ||
	    fflush(stdout) != 0) {
		failure = errno != 0 ? errno : EIO;
		fprintf(stderr, "sippymodem: cannot publish modem device: %s\n",
			strerror(failure));
		exit_code = SIPPYMODEM_EXIT_FAILURE;
		goto close;
	}
	loop_result = sippymodem_loop_run(runtime, &running, &loop_error);
	if (loop_result != SIPPYMODEM_LOOP_OK) {
		report_loop_error(loop_result, &loop_error);
		exit_code = SIPPYMODEM_EXIT_FAILURE;
	}

close:
	close_result = sippymodem_runtime_close(runtime);
	if (close_result != SIPPYMODEM_RUNTIME_OK) {
		fprintf(stderr, "sippymodem: cannot close runtime: %s\n",
			sippymodem_runtime_result_name(close_result));
		exit_code = SIPPYMODEM_EXIT_FAILURE;
	}

destroy:
	sippymodem_runtime_destroy(runtime);
	restore_signal_handlers(&handlers);
	return exit_code;
}

#ifndef SIPPYMODEM_NO_PROCESS_ENTRY
int main(int argc, char *argv[])
{
	return sippymodem_program_main(argc, argv);
}
#endif
