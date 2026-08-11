// SPDX-License-Identifier: BSD-3-Clause

#include <differential_qpsk.h>

#include "test.h"

#include <string.h>

static bool test_dibit_mapping(void)
{
	static const uint8_t expected_turns[] = { 1, 0, 2, 3 };

	for (uint8_t previous = 0; previous < SIPPYMODEM_QPSK_QUADRANT_COUNT;
	     ++previous) {
		for (uint8_t dibit = 0; dibit < SIPPYMODEM_QPSK_QUADRANT_COUNT;
		     ++dibit) {
			uint8_t decoded = UINT8_MAX;
			uint8_t quadrant = UINT8_MAX;

			CHECK(sippymodem_differential_qpsk_encode(
				      previous, dibit, &quadrant) ==
				      SIPPYMODEM_DIFFERENTIAL_QPSK_OK &&
				      quadrant ==
					      (uint8_t)((previous +
							 expected_turns[dibit]) &
							3u) &&
				      sippymodem_differential_qpsk_decode(
					      previous, quadrant, &decoded) ==
					      SIPPYMODEM_DIFFERENTIAL_QPSK_OK &&
				      decoded == dibit,
			      "differential QPSK mapping changed at %u/%u",
			      previous, dibit);
		}
	}
	return true;
}

static bool test_validation(void)
{
	uint8_t value = UINT8_MAX;

	CHECK(sippymodem_differential_qpsk_encode(
		      SIPPYMODEM_QPSK_QUADRANT_COUNT, 0, &value) ==
		      SIPPYMODEM_DIFFERENTIAL_QPSK_INVALID_ARGUMENT &&
		      value == UINT8_MAX &&
		      sippymodem_differential_qpsk_encode(0, 0, NULL) ==
			      SIPPYMODEM_DIFFERENTIAL_QPSK_INVALID_ARGUMENT &&
		      sippymodem_differential_qpsk_decode(
			      0, SIPPYMODEM_QPSK_QUADRANT_COUNT, &value) ==
			      SIPPYMODEM_DIFFERENTIAL_QPSK_INVALID_ARGUMENT &&
		      value == UINT8_MAX &&
		      strcmp(sippymodem_differential_qpsk_result_name(
			     SIPPYMODEM_DIFFERENTIAL_QPSK_INVALID_ARGUMENT),
			     "invalid argument") == 0,
	      "differential QPSK accepted invalid input");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "dibit mapping", test_dibit_mapping },
		{ "validation", test_validation },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		bool passed;

		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		passed = tests[index].run();
		if (!passed) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0 ? 0 : 1;
}
