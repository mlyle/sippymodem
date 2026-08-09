// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_TEST_H
#define SIPPYMODEM_TEST_H

#include <stdbool.h>
#include <stdio.h>

#define ARRAY_LENGTH(array) (sizeof(array) / sizeof((array)[0]))

#define CHECK(condition, ...)                                                \
	do {                                                                   \
		if (!(condition)) {                                              \
			fprintf(stderr, "%s:%d: ", __FILE__, __LINE__);          \
			fprintf(stderr, __VA_ARGS__);                              \
			fputc('\n', stderr);                                      \
			return false;                                              \
		}                                                              \
	} while (0)

#endif
