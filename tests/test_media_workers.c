// SPDX-License-Identifier: BSD-3-Clause
#include "media_workers.h"
#include "wake_signal.h"
#include "test.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

struct gate {
	pthread_mutex_t mutex;
	pthread_cond_t ready;
	unsigned count;
};

struct modem_line {
	unsigned int calls;
	pthread_t thread;
	bool blocked;
	struct gate entered, release;
};
static atomic_uint active, peak;
static bool fail_create;
static size_t worker_stack_size;
int worker_test_thread_create(pthread_t *thread, const pthread_attr_t *attr,
	void *(*start)(void *), void *context)
{
	if (attr == NULL || pthread_attr_getstacksize(attr, &worker_stack_size) != 0)
		return EINVAL;
	if (fail_create)
		return EAGAIN;
	return pthread_create(thread, attr, start, context);
}
static bool gate_init(struct gate *gate)
{
	int error = pthread_mutex_init(&gate->mutex, NULL);
	if (error != 0)
		return false;
	error = pthread_cond_init(&gate->ready, NULL);
	if (error != 0)
		pthread_mutex_destroy(&gate->mutex);
	return error == 0;
}

static void take(struct gate *gate)
{
	pthread_mutex_lock(&gate->mutex);
	while (gate->count == 0)
		pthread_cond_wait(&gate->ready, &gate->mutex);
	gate->count--;
	pthread_mutex_unlock(&gate->mutex);
}

static void give(struct gate *gate)
{
	pthread_mutex_lock(&gate->mutex);
	gate->count++;
	pthread_cond_signal(&gate->ready);
	pthread_mutex_unlock(&gate->mutex);
}

static void gate_destroy(struct gate *gate)
{
	pthread_cond_destroy(&gate->ready);
	pthread_mutex_destroy(&gate->mutex);
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
		give(&line->entered);
		take(&line->release);
	}
	*datagrams = (size_t)now_ns;
	atomic_fetch_sub(&active, 1);
	return MODEM_HOST_OK;
}

static bool test_independent_lines(void)
{
	struct modem_line slow = { .blocked = true }, fast = { 0 };
	CHECK(gate_init(&slow.entered) && gate_init(&slow.release),
	      "line gates failed");
	struct wake_signal wake = WAKE_SIGNAL_INITIALIZER;
	CHECK(wake_signal_open(&wake), "wake startup failed");
	struct media_worker *a = media_worker_create(&slow, &wake);
	struct media_worker *b = media_worker_create(&fast, &wake);
	CHECK(a != NULL && b != NULL, "line startup failed");
	CHECK(worker_stack_size >= 8u * 1024u * 1024u, "DSP stack is too small");
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
		give(&slow.release);
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
		CHECK(wake_signal_take(&wake) && wake_signal_take(&wake) &&
		      !wake_signal_take(&wake) && errno == EAGAIN,
		      "completion wake accounting differs");
	}
	CHECK(atomic_load(&peak) == 2, "DSP work did not overlap");
	media_worker_destroy(a);
	media_worker_destroy(b);
	gate_destroy(&slow.entered);
	gate_destroy(&slow.release);
	wake_signal_close(&wake);
	return true;
}

static bool test_lifecycle(void)
{
	struct modem_line line = { 0 };
	struct wake_signal closed = WAKE_SIGNAL_INITIALIZER;
	CHECK(media_worker_create(NULL, NULL) == NULL && errno == EINVAL, "missing line accepted");
	CHECK(media_worker_create(&line, &closed) == NULL && errno == EINVAL, "closed wake owner accepted");
	fail_create = true;
	CHECK(media_worker_create(&line, NULL) == NULL && errno == EAGAIN, "failed startup not unwound");
	fail_create = false;
	struct media_worker *worker = media_worker_create(&line, NULL);
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
