// SPDX-License-Identifier: BSD-3-Clause

#ifndef MEMORY_REGION_H
#define MEMORY_REGION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Zero-length regions never overlap.  Subtraction after ordering the start
 * addresses avoids constructing a potentially overflowing end address. */
static inline bool memory_regions_overlap(
	const void *left, size_t left_size,
	const void *right, size_t right_size)
{
	uintptr_t left_address = (uintptr_t)left;
	uintptr_t right_address = (uintptr_t)right;

	if (left_size == 0 || right_size == 0)
		return false;
	return left_address <= right_address ?
		       right_address - left_address < left_size :
		       left_address - right_address < right_size;
}

/* Validate a caller-owned byte region without relying on object alignment. */
static inline bool memory_region_is_zero(const void *region, size_t size)
{
	const uint8_t *bytes = region;

	if (bytes == NULL)
		return false;
	for (size_t index = 0; index < size; ++index) {
		if (bytes[index] != 0u)
			return false;
	}
	return true;
}

#endif
