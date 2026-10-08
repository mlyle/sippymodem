// SPDX-License-Identifier: BSD-3-Clause

#include <memory_region.h>

#include "test.h"

#include <stdlib.h>

static bool test_boundaries_and_containment(void)
{
	uint8_t storage[32];

	CHECK(memory_regions_overlap(storage, 1, storage, 1) &&
	      memory_regions_overlap(storage, 8, storage + 2, 2) &&
	      memory_regions_overlap(storage + 2, 2, storage, 8) &&
	      memory_regions_overlap(storage, 4, storage + 3, 4) &&
	      memory_regions_overlap(storage + 3, 4, storage, 4) &&
	      !memory_regions_overlap(storage, 4, storage + 4, 4) &&
	      !memory_regions_overlap(storage + 12, 3, storage + 2, 4),
	      "memory-region boundary or symmetry changed");
	return true;
}

static bool test_empty_regions(void)
{
	uint8_t storage[8];

	CHECK(!memory_regions_overlap(storage, 0, storage, 0) &&
	      !memory_regions_overlap(storage + 2, 0, storage, sizeof(storage)) &&
	      !memory_regions_overlap(storage, sizeof(storage), storage + 2, 0),
	      "empty memory region was treated as overlapping");
	return true;
}

static bool test_region_sets(void)
{
	uint8_t storage[32];

	CHECK(!memory_region_set_overlaps(NULL, 0) &&
	      memory_region_set_overlaps(NULL, 1) &&
	      memory_region_set_overlaps(NULL, 3),
	      "missing memory-region list validation changed");
	for (size_t count = 2; count <= 4; ++count) {
		struct memory_region *regions = malloc(count * sizeof(*regions));

		CHECK(regions != NULL, "memory-region fixture allocation failed");
		for (size_t i = 0; i < count; ++i)
			regions[i] = (struct memory_region){ storage + 8 * i, 4 };
		CHECK(!memory_region_set_overlaps(regions, 0) &&
		      !memory_region_set_overlaps(regions, 1) &&
		      !memory_region_set_overlaps(regions, count),
		      "disjoint memory-region set changed at count %zu", count);
		for (size_t left = 0; left + 1 < count; ++left) {
			for (size_t right = left + 1; right < count; ++right) {
				regions[right].data = storage + 8 * left + 3;
				CHECK(memory_region_set_overlaps(regions, count),
				      "set overlap missed at %zu/%zu/%zu", count, left, right);
				regions[right].size = 0;
				CHECK(!memory_region_set_overlaps(regions, count),
				      "empty set member overlapped at %zu/%zu/%zu", count, left, right);
				regions[right].size = 4;
				regions[right].data = storage + 8 * left + 4;
				CHECK(!memory_region_set_overlaps(regions, count),
				      "adjacent set members overlapped at %zu/%zu/%zu", count, left, right);
				regions[right].data = storage + 8 * right;
			}
		}
		free(regions);
	}
	return true;
}

static bool test_zero_regions(void)
{
	uint8_t storage[8] = { 0 };

	CHECK(memory_region_is_zero(storage, sizeof(storage)) &&
	      memory_region_is_zero(storage, 0) &&
	      !memory_region_is_zero(NULL, 0),
	      "zero memory region validation changed");
	storage[5] = 1;
	CHECK(!memory_region_is_zero(storage, sizeof(storage)) &&
	      memory_region_is_zero(storage, 5),
	      "nonzero memory byte was not bounded correctly");
	return true;
}

static bool test_zero_region_alignment_and_bounds(void)
{
	/* Place the requested end at the allocation boundary so sanitizer runs
	 * also detect an overread, not just a wrong answer from a nonzero guard. */
	for (size_t offset = 0; offset < 16u; ++offset) {
		for (size_t size = 0; size <= 33u; ++size) {
			uint8_t *storage = malloc(offset + size + 1u);
			uint8_t *region;

			CHECK(storage != NULL, "zero-region fixture allocation failed");
			memset(storage, UINT8_MAX, offset + size + 1u);
			region = storage + offset + 1u;
			memset(region, 0, size);
			CHECK(memory_region_is_zero(region, size),
			      "zero region rejected at offset %zu size %zu", offset, size);
			for (size_t index = 0; index < size; ++index) {
				for (unsigned int bit = 0; bit < 8u; ++bit) {
					region[index] = (uint8_t)(1u << bit);
					CHECK(!memory_region_is_zero(region, size) &&
					      memory_region_is_zero(region, index) &&
					      memory_region_is_zero(region + index + 1u,
								    size - index - 1u),
					      "nonzero bit escaped bounds at %zu/%zu byte %zu bit %u",
					      offset, size, index, bit);
				}
				region[index] = 0u;
			}
			free(storage);
		}
	}
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "boundaries and containment", test_boundaries_and_containment },
		{ "empty regions", test_empty_regions },
		{ "region sets", test_region_sets },
		{ "zero regions", test_zero_regions },
		{ "zero region alignment and bounds", test_zero_region_alignment_and_bounds },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (!tests[index].run()) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0 ? 0 : 1;
}
