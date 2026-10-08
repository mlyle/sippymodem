// SPDX-License-Identifier: BSD-3-Clause
#include "media_workers.h"
#include "test.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

struct modem_line { unsigned int calls; pthread_t thread; };
static pthread_barrier_t barrier;
static bool synchronize_jobs;
static atomic_uint active, peak;
static unsigned int fail_create;
int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
	void *(*start)(void *), void *context)
{
	if (fail_create != 0 && --fail_create == 0)
		return EAGAIN;
	return __real_pthread_create(thread, attr, start, context);
}

enum modem_host_result modem_line_process_media(struct modem_line *line,
	uint64_t now_ns, size_t *datagrams)
{
	line->calls++;
	line->thread = pthread_self();
	unsigned int current = atomic_fetch_add(&active, 1) + 1;
	unsigned int previous = atomic_load(&peak);
	while (previous < current && !atomic_compare_exchange_weak(&peak, &previous, current)) {}
	if (synchronize_jobs)
		pthread_barrier_wait(&barrier);
	*datagrams = (size_t)now_ns;
	atomic_fetch_sub(&active, 1);
	return MODEM_HOST_OK;
}

static bool test_batches(void)
{
	const unsigned int counts[] = { 1, 2, 4 };
	for (unsigned int c = 0; c < sizeof(counts) / sizeof(counts[0]); ++c) {
		unsigned int threads = counts[c];
		struct media_workers *workers = media_workers_create(threads);
		CHECK(workers != NULL, "create %u workers", threads);
		CHECK(pthread_barrier_init(&barrier, NULL, threads) == 0, "barrier init");
		synchronize_jobs = true;
		atomic_store(&peak, 0);
		for (unsigned int pass = 0; pass < 20; ++pass) {
			struct modem_line lines[8] = { 0 };
			struct media_job jobs[8];
			for (unsigned int i = 0; i < 8; ++i)
				jobs[i] = (struct media_job){ .line = &lines[i], .now_ns = i + pass };
			CHECK(media_workers_run(workers, jobs, 8), "run batch");
			for (unsigned int i = 0; i < 8; ++i)
				CHECK(lines[i].calls == 1 && jobs[i].result == MODEM_HOST_OK &&
				      jobs[i].datagrams == i + pass, "job %u was lost or repeated", i);
		}
		CHECK(atomic_load(&peak) >= threads, "jobs did not overlap across %u threads", threads);
		synchronize_jobs = false;
		pthread_barrier_destroy(&barrier);
		CHECK(media_workers_run(workers, NULL, 0), "empty batch");
		struct modem_line line = { 0 };
		struct media_job one = { .line = &line };
		CHECK(media_workers_run(workers, &one, 1) && line.calls == 1 &&
		      pthread_equal(line.thread, pthread_self()), "single job should stay on owner");
		struct media_job duplicate[] = { one, one };
		CHECK(!media_workers_run(workers, duplicate, 2) && line.calls == 1,
		      "duplicate line ownership was accepted");
		CHECK(!media_workers_run(workers, NULL, 1) && !media_workers_run(workers, &one, 33),
		      "invalid batch bounds were accepted");
		one.line = NULL;
		CHECK(!media_workers_run(workers, &one, 1), "missing line was accepted");
		media_workers_destroy(workers);
	}
	return true;
}

static bool test_failed_startup(void)
{
	CHECK(media_workers_create(0) == NULL && errno == EINVAL, "zero threads accepted");
	CHECK(media_workers_create(33) == NULL && errno == EINVAL, "thread bound ignored");
	for (unsigned int i = 1; i <= 3; ++i) {
		fail_create = i;
		CHECK(media_workers_create(4) == NULL && errno == EAGAIN, "partial startup did not unwind");
		fail_create = 0;
		struct media_workers *workers = media_workers_create(4);
		CHECK(workers != NULL, "could not restart after failed creation");
		media_workers_destroy(workers);
	}
	media_workers_destroy(NULL);
	CHECK(!media_workers_run(NULL, NULL, 0), "missing pool accepted");
	return true;
}

int main(void)
{
	return test_batches() && test_failed_startup() ? 0 : 1;
}
