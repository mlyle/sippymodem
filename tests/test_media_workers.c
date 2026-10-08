// SPDX-License-Identifier: BSD-3-Clause
#include "media_workers.h"
#include "test.h"

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <sys/eventfd.h>
#include <unistd.h>

struct modem_line {
	unsigned int calls;
	pthread_t thread;
	bool blocked;
	sem_t entered, release;
};
static atomic_uint active, peak;
static bool fail_create;
int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
	void *(*start)(void *), void *context)
{
	if (fail_create)
		return EAGAIN;
	return __real_pthread_create(thread, attr, start, context);
}
static void take(sem_t *semaphore)
{
	while (sem_wait(semaphore) != 0)
		if (errno != EINTR)
			_exit(2);
}

enum modem_host_result modem_line_process_media(struct modem_line *line,
	uint64_t now_ns, size_t *datagrams)
{
	line->calls++;
	line->thread = pthread_self();
	unsigned int current = atomic_fetch_add(&active, 1) + 1;
	unsigned int previous = atomic_load(&peak);
	while (previous < current && !atomic_compare_exchange_weak(&peak, &previous, current)) {}
	if (line->blocked) {
		sem_post(&line->entered);
		take(&line->release);
	}
	*datagrams = (size_t)now_ns;
	atomic_fetch_sub(&active, 1);
	return MODEM_HOST_OK;
}

static bool test_independent_lines(void)
{
	struct modem_line slow = { .blocked = true }, fast = { 0 };
	CHECK(sem_init(&slow.entered, 0, 0) == 0 && sem_init(&slow.release, 0, 0) == 0,
	      "line gates failed");
	int wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE);
	struct media_worker *a = media_worker_create(&slow, wake);
	struct media_worker *b = media_worker_create(&fast, wake);
	CHECK(wake >= 0 && a != NULL && b != NULL, "line startup failed");
	pthread_t a_thread = pthread_self(), b_thread = pthread_self();
	for (unsigned pass = 1; pass <= 20; ++pass) {
		struct media_job job;
		CHECK(media_worker_submit(a, pass), "slow submission failed");
		take(&slow.entered);
		CHECK(media_worker_submit(b, pass + 100), "fast submission failed");
		CHECK(media_worker_collect(b, true, &job) && job.result == MODEM_HOST_OK &&
		      job.datagrams == pass + 100 && job.notified, "fast completion failed");
		CHECK(media_worker_busy(a) && !media_worker_collect(a, false, &job) &&
		      !media_worker_submit(a, 0), "unfinished job was overwritten or waited on");
		CHECK(!pthread_equal(fast.thread, pthread_self()), "DSP ran on native owner");
		sem_post(&slow.release);
		CHECK(media_worker_collect(a, true, &job) && job.datagrams == pass && job.notified,
		      "slow completion failed");
		CHECK(!media_worker_busy(a) && !media_worker_collect(a, false, &job), "completion repeated");
		CHECK(slow.calls == pass && fast.calls == pass &&
		      !pthread_equal(slow.thread, fast.thread), "line work was lost or shared a thread");
		if (pass == 1) {
			a_thread = slow.thread;
			b_thread = fast.thread;
		}
		CHECK(pthread_equal(slow.thread, a_thread) && pthread_equal(fast.thread, b_thread),
		      "line changed DSP thread");
		uint64_t one;
		CHECK(read(wake, &one, sizeof(one)) == sizeof(one) && one == 1 &&
		      read(wake, &one, sizeof(one)) == sizeof(one) && one == 1 &&
		      read(wake, &one, sizeof(one)) < 0 && errno == EAGAIN,
		      "completion wake accounting differs");
	}
	CHECK(atomic_load(&peak) == 2, "DSP work did not overlap");
	media_worker_destroy(a);
	media_worker_destroy(b);
	sem_destroy(&slow.entered);
	sem_destroy(&slow.release);
	close(wake);
	return true;
}

static bool test_lifecycle(void)
{
	struct modem_line line = { 0 };
	CHECK(media_worker_create(NULL, -1) == NULL && errno == EINVAL, "missing line accepted");
	CHECK(media_worker_create(&line, -2) == NULL && errno == EINVAL, "invalid wake fd accepted");
	fail_create = true;
	CHECK(media_worker_create(&line, -1) == NULL && errno == EAGAIN, "failed startup not unwound");
	fail_create = false;
	struct media_worker *worker = media_worker_create(&line, -1);
	CHECK(worker != NULL && !media_worker_busy(worker), "restart failed");
	CHECK(media_worker_submit(worker, 1), "submission failed");
	media_worker_destroy(worker);
	CHECK(line.calls == 1, "destroy returned before work finished");
	media_worker_destroy(NULL);
	struct media_job job;
	CHECK(!media_worker_submit(NULL, 1) && !media_worker_collect(NULL, false, &job),
	      "missing worker accepted");
	return true;
}

int main(void)
{
	return test_independent_lines() && test_lifecycle() ? 0 : 1;
}
