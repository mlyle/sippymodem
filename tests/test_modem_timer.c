// SPDX-License-Identifier: BSD-3-Clause

#include <modem_timer.h>
#include "test.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static struct timespec monotonic_time;
static struct timeval wall_time;
static bool clock_failed;
static bool wrong_clock;

int __wrap_clock_gettime(clockid_t id, struct timespec *now)
{
	wrong_clock |= id != CLOCK_MONOTONIC;
	if (clock_failed) {
		errno = EIO;
		return -1;
	}
	*now = monotonic_time;
	return 0;
}

int __wrap_gettimeofday(struct timeval *now, void *zone)
{
	(void)zone;
	*now = wall_time;
	return 0;
}

static bool test_clock_ticks(void)
{
	monotonic_time = (struct timespec){ 12, 9999999 };
	wall_time = (struct timeval){ 2000, 0 };
	CHECK(get_time() == 1200, "clock did not retain 100 Hz resolution");
	monotonic_time.tv_nsec = 10000000;
	CHECK(get_time() == 1201, "clock missed the first complete tick");
	monotonic_time.tv_nsec = 999999999;
	CHECK(get_time() == 1299, "clock rounded a partial final tick upward");
	monotonic_time = (struct timespec){ 13, 0 };
	CHECK(get_time() == 1300 && !wrong_clock,
	      "clock did not use continuous monotonic ticks across a second");
	return true;
}

static bool test_deadlines_ignore_wall_clock(void)
{
	monotonic_time = (struct timespec){ 50, 0 };
	const unsigned long deadline = get_time() + MODEM_HZ / 2;

	wall_time.tv_sec += 3600;
	monotonic_time.tv_nsec = 490000000;
	CHECK(time_before(get_time(), deadline),
	      "wall clock adjustment expired a future deadline");
	wall_time.tv_sec -= 7200;
	monotonic_time.tv_nsec = 500000000;
	CHECK(time_after(get_time(), deadline),
	      "wall clock adjustment postponed a due deadline");
	return true;
}

static bool test_deadline_wrap(void)
{
	monotonic_time = (struct timespec){ (time_t)(ULONG_MAX / MODEM_HZ), 0 };
	const unsigned long before = get_time();
	const unsigned long deadline = before + MODEM_HZ / 2;

	CHECK(deadline < before, "fixture did not cross the unsigned tick limit");
	monotonic_time.tv_nsec = 490000000;
	CHECK(time_before(get_time(), deadline),
	      "wrapped deadline expired one tick early");
	monotonic_time.tv_nsec = 500000000;
	CHECK(get_time() == deadline && time_after(get_time(), deadline),
	      "wrapped deadline was not due at its boundary");
	return true;
}

static bool test_clock_failure(void)
{
	pid_t child = fork();
	int status;

	CHECK(child >= 0, "clock failure fixture could not fork");
	if (child == 0) {
		const struct rlimit core_limit = { 0, 0 };
		(void)setrlimit(RLIMIT_CORE, &core_limit);
		clock_failed = true;
		(void)get_time();
		_exit(0);
	}
	CHECK(waitpid(child, &status, 0) == child &&
		      WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,
	      "failed clock silently supplied a valid-looking deadline tick");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "monotonic modem tick resolution", test_clock_ticks },
		{ "modem deadlines ignore wall clock", test_deadlines_ignore_wall_clock },
		{ "modem tick wrap", test_deadline_wrap },
		{ "modem clock failure", test_clock_failure },
	};
	unsigned failures = 0;

	for (size_t i = 0; i < ARRAY_LENGTH(tests); ++i) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[i].name);
		if (tests[i].run())
			fprintf(stderr, "[       OK ] %s\n", tests[i].name);
		else {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[i].name);
			failures++;
		}
	}
	return failures != 0;
}
