// SPDX-License-Identifier: BSD-3-Clause
#include "log_target.h"
#include "test.h"
#include "syslog_capture.h"
#include <stdarg.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

static unsigned opens, closes, writes;
static int options, facility, priority;
static char identity[64], output[9000];
void __wrap_openlog(const char *name, int flags, int value)
{
	opens++; options = flags; facility = value;
	snprintf(identity, sizeof(identity), "%s", name);
}
void __wrap_closelog(void) { closes++; }
static void capture_syslog(int value, const char *format, va_list args)
{
	writes++; priority = value;
	vsnprintf(output, sizeof(output), format, args);
}

static bool test_log_target(void)
{
	enum log_target target = LOG_TARGET_SYSLOG;
	CHECK(log_target_parse("stderr", &target) && target == LOG_TARGET_STDERR &&
	      log_target_parse("SySlOg", &target) && target == LOG_TARGET_SYSLOG, "valid target rejected");
	CHECK(!log_target_parse("file", &target) && target == LOG_TARGET_SYSLOG &&
	      !log_target_parse(NULL, &target) && !log_target_parse("stderr", NULL), "invalid target accepted");
	FILE *capture = tmpfile();
	CHECK(capture != NULL, "capture failed");
	fflush(stderr);
	int original = dup(STDERR_FILENO);
	CHECK(original >= 0 && dup2(fileno(capture), STDERR_FILENO) >= 0, "redirect failed");
	log_target_write(LOG_INFO, "default");
	log_target_write(LOG_ERR, "newline\n");
	log_target_write(LOG_INFO, NULL);
	log_target_write(LOG_INFO, "");
	fflush(stderr);
	CHECK(dup2(original, STDERR_FILENO) >= 0, "restore failed");
	close(original);
	rewind(capture);
	char captured[64] = { 0 };
	size_t captured_size = fread(captured, 1, sizeof(captured) - 1, capture);
	fclose(capture);
	CHECK(captured_size == 16 && strcmp(captured, "default\nnewline\n") == 0 && writes == 0, "default output changed");
	log_target_open(LOG_TARGET_SYSLOG, "modem-test");
	CHECK(opens == 1 && options == LOG_PID && facility == LOG_DAEMON &&
	      strcmp(identity, "modem-test") == 0, "syslog identity is wrong");
	log_target_write(LOG_WARNING, "remote %n %s text");
	CHECK(writes == 1 && priority == LOG_WARNING && strcmp(output, "remote %n %s text") == 0,
	      "literal text treated as format or priority lost");
	log_target_printf(LOG_ERR, "failure: %s %u", "test", 7u);
	CHECK(writes == 2 && priority == LOG_ERR && strcmp(output, "failure: test 7") == 0, "formatting failed");
	char large[9000];
	memset(large, 'x', sizeof(large) - 1); large[sizeof(large) - 1] = 0;
	log_target_printf(LOG_DEBUG, "%s", large);
	CHECK(strlen(output) == 8192 && priority == LOG_DEBUG, "formatting was not bounded");
	log_target_open(LOG_TARGET_STDERR, "modem-test");
	log_target_close(); log_target_close();
	CHECK(closes == 1, "syslog closure is not idempotent");
	return true;
}
int main(void) { return test_log_target() ? 0 : 1; }
