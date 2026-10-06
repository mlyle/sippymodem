// SPDX-License-Identifier: BSD-3-Clause
#ifndef TEST_SYSLOG_CAPTURE_H
#define TEST_SYSLOG_CAPTURE_H

#include <stdarg.h>

/* Each fixture supplies its own copied-record sink. Wrap both libc entries so
 * fortified builds cannot accidentally send test records to the system log. */
static void capture_syslog(int priority, const char *format, va_list args);

void __wrap_syslog(int priority, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	capture_syslog(priority, format, args);
	va_end(args);
}

void __wrap___syslog_chk(int priority, int flag, const char *format, ...)
{
	(void)flag;
	va_list args;
	va_start(args, format);
	capture_syslog(priority, format, args);
	va_end(args);
}

#endif
