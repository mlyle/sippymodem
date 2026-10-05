// SPDX-License-Identifier: BSD-3-Clause

#include <frame_clock.h>
#include <frame_pacer.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

static bool test_exact_deadlines_and_catchup(void)
{
	struct frame_clock clock;
	size_t frames = 99;

	CHECK(frame_clock_init(&clock, 80, 8000, 3) ==
		      FRAME_CLOCK_OK &&
		      clock.interval_ns == 10000000,
	      "frame clock did not derive a 10 ms interval");
	frame_clock_start(&clock, 1000000000);
	CHECK(frame_clock_wait_ns(&clock, 1000000000) == 0 &&
		      frame_clock_take_due(
			      &clock, 1000000000, &frames) ==
			      FRAME_CLOCK_OK &&
		      frames == 1 && clock.next_deadline_ns == 1010000000,
	      "frame clock did not make its first frame immediately due");
	CHECK(frame_clock_take_due(
		      &clock, 1005000000, &frames) ==
		      FRAME_CLOCK_NOT_DUE &&
		      frames == 0 &&
		      frame_clock_wait_ns(&clock, 1005000000) ==
			      5000000,
	      "frame clock changed an early deadline");
	CHECK(frame_clock_take_due(
		      &clock, 1030000000, &frames) ==
		      FRAME_CLOCK_OK &&
		      frames == 3 && clock.next_deadline_ns == 1040000000 &&
		      clock.stats.frames_due == 4,
	      "frame clock did not preserve phase during bounded catch-up");
	return true;
}

static bool test_large_backlog_is_drained_in_bounded_batches(void)
{
	struct frame_clock clock;
	size_t frames = 99;

	CHECK(frame_clock_init(&clock, 8, 8000, 4) ==
		      FRAME_CLOCK_OK,
	      "backlog clock initialization failed");
	frame_clock_start(&clock, 1000000);
	CHECK(frame_clock_take_due(&clock, 5000001, &frames) ==
		      FRAME_CLOCK_OK &&
		      frames == 4 && clock.running &&
		      clock.next_deadline_ns == 5000000 &&
		      clock.stats.frames_due == 4 && clock.stats.overruns == 1 &&
		      frame_clock_wait_ns(&clock, 5000001) == 0,
	      "large backlog was not bounded while remaining immediately due");
	CHECK(frame_clock_take_due(&clock, 5000001, &frames) ==
		      FRAME_CLOCK_OK &&
		      frames == 1 && clock.running &&
		      clock.next_deadline_ns == 6000000 &&
		      clock.stats.frames_due == 5 && clock.stats.overruns == 1,
	      "frame clock did not drain the remaining backlog");
	return true;
}

static bool test_validation_and_integer_overflow(void)
{
	struct frame_clock clock;
	struct frame_clock original;
	size_t frames = 99;

	CHECK(frame_clock_init(&clock, 80, 8000, 2) ==
		      FRAME_CLOCK_OK,
	      "validation clock initialization failed");
	original = clock;
	CHECK(frame_clock_init(&clock, 0, 8000, 2) ==
		      FRAME_CLOCK_INVALID_ARGUMENT &&
		      memcmp(&clock, &original, sizeof(clock)) == 0,
	      "invalid initialization partially changed the clock");
	CHECK(frame_clock_init(&clock, 1, 3, 2) ==
		      FRAME_CLOCK_INEXACT_INTERVAL &&
		      memcmp(&clock, &original, sizeof(clock)) == 0,
	      "inexact initialization partially changed the clock");
	CHECK(frame_clock_take_due(NULL, 0, &frames) ==
		      FRAME_CLOCK_INVALID_ARGUMENT &&
		      frames == 0,
	      "null frame clock validation is inconsistent");

	CHECK(frame_clock_init(
		      &clock, 1, 1000000000, SIZE_MAX) ==
		      FRAME_CLOCK_OK,
	      "one-nanosecond clock initialization failed");
	frame_clock_start(&clock, 0);
	CHECK(frame_clock_take_due(&clock, UINT64_MAX, &frames) ==
		      FRAME_CLOCK_OVERRUN &&
		      frames == 0 && !clock.running,
	      "deadline arithmetic wrapped instead of reporting overrun");
	CHECK(strcmp(frame_clock_result_name(
			     FRAME_CLOCK_OVERRUN),
		     "frame clock deadline overflow") == 0,
	      "frame clock result name is wrong");
	return true;
}

static bool test_exact_four_thirds_pacing(void)
{
	struct frame_pacer pacer;
	unsigned int frames = 0;

	CHECK(frame_pacer_init(&pacer, UINT64_C(20000000) / 4 * 3, 4),
	      "exact pacing init failed");
	for (unsigned int tick = 0; tick <= 240; ++tick) {
		uint64_t now = tick * UINT64_C(5000000);

		while (frame_pacer_take(&pacer, now))
			frames++;
		CHECK(frame_pacer_wait_ns(&pacer, now) > 0,
		      "empty pacing budget exposed a busy wait");
	}
	CHECK(frames == 4 + 80 && pacer.interval_ns == UINT64_C(15000000),
	      "1.2 seconds supplied %u frames instead of four burst plus 80 paced", frames);
	for (unsigned int frame = 0; frame < 4; ++frame)
		CHECK(frame_pacer_take(&pacer, UINT64_C(60000000000)),
		      "idle pacer did not restore its bounded burst");
	CHECK(!frame_pacer_take(&pacer, UINT64_C(60000000000)) &&
	      frame_pacer_wait_ns(&pacer, UINT64_C(60000000000)) == UINT64_C(15000000),
	      "idle time banked more than four frames");
	return true;
}

static bool test_pacer_backward_time_and_saturation(void)
{
	struct frame_pacer pacer;

	CHECK(frame_pacer_init(&pacer, 15, 4), "overflow pacing init failed");
	for (unsigned int frame = 0; frame < 4; ++frame)
		CHECK(frame_pacer_take(&pacer, 100), "initial pacing burst failed");
	CHECK(!frame_pacer_take(&pacer, 10) &&
	      frame_pacer_wait_ns(&pacer, 10) == 15 &&
	      !frame_pacer_take(&pacer, 114) && frame_pacer_take(&pacer, 115),
	      "backward time minted credit or moved the refill origin");
	for (unsigned int frame = 0; frame < 4; ++frame)
		CHECK(frame_pacer_take(&pacer, UINT64_MAX), "saturated-time burst failed");
	CHECK(!frame_pacer_take(&pacer, UINT64_MAX) &&
	      frame_pacer_wait_ns(&pacer, UINT64_MAX) == 15,
	      "time saturation minted unbounded credit or a zero wait");
	struct frame_pacer original = pacer;
	CHECK(!frame_pacer_init(&pacer, UINT64_MAX, 4) &&
	      memcmp(&pacer, &original, sizeof(pacer)) == 0 &&
	      !frame_pacer_take(NULL, 0) && frame_pacer_wait_ns(NULL, 0) == UINT64_MAX,
	      "invalid pacing initialization changed state");
	frame_pacer_reset(&pacer);
	CHECK(frame_pacer_take(&pacer, 0), "reset retained the previous call's debt");
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
		{ "bounded backlog",
		  test_large_backlog_is_drained_in_bounded_batches },
		{ "validation and overflow", test_validation_and_integer_overflow },
		{ "exact 4/3 pacing", test_exact_four_thirds_pacing },
		{ "pacing backward time and saturation", test_pacer_backward_time_and_saturation },
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
