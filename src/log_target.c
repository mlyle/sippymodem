// SPDX-License-Identifier: BSD-3-Clause
#include "log_target.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>

static enum log_target active_target;

bool log_target_parse(const char *text, enum log_target *target)
{
	if (text == NULL || target == NULL)
		return false;
	if (strcasecmp(text, "stderr") == 0)
		*target = LOG_TARGET_STDERR;
	else if (strcasecmp(text, "syslog") == 0)
		*target = LOG_TARGET_SYSLOG;
	else
		return false;
	return true;
}

void log_target_close(void)
{
	if (active_target == LOG_TARGET_SYSLOG)
		closelog();
	active_target = LOG_TARGET_STDERR;
}

void log_target_open(enum log_target target, const char *identity)
{
	log_target_close();
	active_target = target;
	if (target == LOG_TARGET_SYSLOG)
		openlog(identity, LOG_PID, LOG_DAEMON);
}

void log_target_write(int priority, const char *text)
{
	if (text == NULL || text[0] == '\0')
		return;
	if (active_target == LOG_TARGET_SYSLOG) {
		syslog(priority, "%s", text);
	} else {
		fputs(text, stderr);
		if (text[strlen(text) - 1] != '\n')
			fputc('\n', stderr);
	}
}

void log_target_printf(int priority, const char *format, ...)
{
	char text[8193];
	va_list args;
	va_start(args, format);
	int size = vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	if (size >= 0)
		log_target_write(priority, text);
}
