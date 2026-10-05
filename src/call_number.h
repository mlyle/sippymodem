// SPDX-License-Identifier: BSD-3-Clause
#ifndef CALL_NUMBER_H
#define CALL_NUMBER_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Copy a bounded telephone user, removing visual separators. Require digits
 * and at most one leading '+'. Source and destination must not overlap. */
static inline bool call_number_copy(char *number, size_t capacity,
	const char *source, size_t length)
{
	if (number == NULL || capacity == 0)
		return false;
	memset(number, 0, capacity);
	if (source == NULL || length == 0 || length >= capacity)
		return false;
	size_t count = 0;
	bool digits = false;
	for (size_t index = 0; index < length; ++index) {
		char ch = source[index];
		if (ch >= '0' && ch <= '9') {
			digits = true;
			count++;
		} else if (ch == '+' && count == 0) {
			count++;
		} else if (ch != '-' && ch != '.' && ch != '(' && ch != ')') {
			return false;
		}
	}
	if (!digits)
		return false;
	count = 0;
	for (size_t index = 0; index < length; ++index)
		if ((source[index] >= '0' && source[index] <= '9') || source[index] == '+')
			number[count++] = source[index];
	return true;
}

#endif
