// SPDX-License-Identifier: BSD-3-Clause
#ifndef LOG_TARGET_H
#define LOG_TARGET_H

#include <stdbool.h>

enum log_target { LOG_TARGET_STDERR, LOG_TARGET_SYSLOG };

/* Executable-owned output. Never linked into the native modem library. */
bool log_target_parse(const char *text, enum log_target *target);
void log_target_open(enum log_target target, const char *identity);
void log_target_close(void);
void log_target_write(int priority, const char *text);
void log_target_printf(int priority, const char *format, ...)
	__attribute__((format(printf, 2, 3)));

#endif
