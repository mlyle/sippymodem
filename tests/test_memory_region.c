// SPDX-License-Identifier: BSD-3-Clause

#include <memory_region.h>

#include "test.h"

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

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "boundaries and containment", test_boundaries_and_containment },
		{ "empty regions", test_empty_regions },
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
