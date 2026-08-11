// SPDX-License-Identifier: BSD-3-Clause

#include <frame_clock.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

static bool test_exact_deadlines_and_catchup(void)
{
	struct sippymodem_frame_clock clock;
	size_t frames = 99;

	CHECK(sippymodem_frame_clock_init(&clock, 80, 8000, 3) ==
		      SIPPYMODEM_FRAME_CLOCK_OK &&
		      clock.interval_ns == 10000000,
	      "frame clock did not derive a 10 ms interval");
	sippymodem_frame_clock_start(&clock, 1000000000);
	CHECK(sippymodem_frame_clock_wait_ns(&clock, 1000000000) == 0 &&
		      sippymodem_frame_clock_take_due(
			      &clock, 1000000000, &frames) ==
			      SIPPYMODEM_FRAME_CLOCK_OK &&
		      frames == 1 && clock.next_deadline_ns == 1010000000,
	      "frame clock did not make its first frame immediately due");
	CHECK(sippymodem_frame_clock_take_due(
		      &clock, 1005000000, &frames) ==
		      SIPPYMODEM_FRAME_CLOCK_NOT_DUE &&
		      frames == 0 &&
		      sippymodem_frame_clock_wait_ns(&clock, 1005000000) ==
			      5000000,
	      "frame clock changed an early deadline");
	CHECK(sippymodem_frame_clock_take_due(
		      &clock, 1030000000, &frames) ==
		      SIPPYMODEM_FRAME_CLOCK_OK &&
		      frames == 3 && clock.next_deadline_ns == 1040000000 &&
		      clock.stats.frames_due == 4,
	      "frame clock did not preserve phase during bounded catch-up");
	return true;
}

static bool test_overrun_stops_transactionally(void)
{
	struct sippymodem_frame_clock clock;
	size_t frames = 99;

	CHECK(sippymodem_frame_clock_init(&clock, 8, 8000, 4) ==
		      SIPPYMODEM_FRAME_CLOCK_OK,
	      "overrun clock initialization failed");
	sippymodem_frame_clock_start(&clock, 1000000);
	CHECK(sippymodem_frame_clock_take_due(&clock, 5000001, &frames) ==
		      SIPPYMODEM_FRAME_CLOCK_OVERRUN &&
		      frames == 0 && !clock.running &&
		      clock.stats.frames_due == 0 && clock.stats.overruns == 1 &&
		      sippymodem_frame_clock_wait_ns(&clock, 5000001) ==
			      UINT64_MAX,
	      "frame overrun advanced or left the clock running");
	CHECK(sippymodem_frame_clock_take_due(&clock, 5000001, &frames) ==
		      SIPPYMODEM_FRAME_CLOCK_NOT_RUNNING,
	      "stopped overrun clock still produced frames");
	sippymodem_frame_clock_start(&clock, 9000000);
	CHECK(sippymodem_frame_clock_take_due(&clock, 9000000, &frames) ==
		      SIPPYMODEM_FRAME_CLOCK_OK &&
		      frames == 1 && clock.stats.frames_due == 1,
	      "frame clock could not restart after an overrun");
	return true;
}

static bool test_validation_and_integer_overflow(void)
{
	struct sippymodem_frame_clock clock;
	struct sippymodem_frame_clock original;
	size_t frames = 99;

	CHECK(sippymodem_frame_clock_init(&clock, 80, 8000, 2) ==
		      SIPPYMODEM_FRAME_CLOCK_OK,
	      "validation clock initialization failed");
	original = clock;
	CHECK(sippymodem_frame_clock_init(&clock, 0, 8000, 2) ==
		      SIPPYMODEM_FRAME_CLOCK_INVALID_ARGUMENT &&
		      memcmp(&clock, &original, sizeof(clock)) == 0,
	      "invalid initialization partially changed the clock");
	CHECK(sippymodem_frame_clock_init(&clock, 1, 3, 2) ==
		      SIPPYMODEM_FRAME_CLOCK_INEXACT_INTERVAL &&
		      memcmp(&clock, &original, sizeof(clock)) == 0,
	      "inexact initialization partially changed the clock");
	CHECK(sippymodem_frame_clock_take_due(NULL, 0, &frames) ==
		      SIPPYMODEM_FRAME_CLOCK_INVALID_ARGUMENT &&
		      frames == 0,
	      "null frame clock validation is inconsistent");

	CHECK(sippymodem_frame_clock_init(
		      &clock, 1, 1000000000, SIZE_MAX) ==
		      SIPPYMODEM_FRAME_CLOCK_OK,
	      "one-nanosecond clock initialization failed");
	sippymodem_frame_clock_start(&clock, 0);
	CHECK(sippymodem_frame_clock_take_due(&clock, UINT64_MAX, &frames) ==
		      SIPPYMODEM_FRAME_CLOCK_OVERRUN &&
		      frames == 0 && !clock.running,
	      "deadline arithmetic wrapped instead of reporting overrun");
	CHECK(strcmp(sippymodem_frame_clock_result_name(
			     SIPPYMODEM_FRAME_CLOCK_OVERRUN),
		     "frame clock catch-up limit exceeded") == 0,
	      "frame clock result name is wrong");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "exact deadlines and catch-up", test_exact_deadlines_and_catchup },
		{ "transactional overrun", test_overrun_stops_transactionally },
		{ "validation and overflow", test_validation_and_integer_overflow },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		printf("[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			printf("[       OK ] %s\n", tests[index].name);
		else {
			printf("[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0 ? 0 : 1;
}
