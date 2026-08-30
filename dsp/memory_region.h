// SPDX-License-Identifier: BSD-3-Clause

#ifndef MEMORY_REGION_H
#define MEMORY_REGION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct memory_region {
	const void *data;
	size_t size;
};

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

/* Conservatively reject a missing descriptor list when it claims entries. */
static inline bool memory_region_set_overlaps(
	const struct memory_region *regions, size_t count)
{
	if (regions == NULL)
		return count != 0;
	for (size_t left = 0; left < count; ++left) {
		for (size_t right = left + 1u; right < count; ++right) {
			if (memory_regions_overlap(
				    regions[left].data, regions[left].size,
				    regions[right].data, regions[right].size))
				return true;
		}
	}
	return false;
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
