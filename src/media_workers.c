// SPDX-License-Identifier: BSD-3-Clause
#include "media_workers.h"
#include "line.h"
#include "wake_signal.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

struct media_worker {
	pthread_t thread;
	pthread_mutex_t mutex;
	pthread_cond_t ready, done;
	struct modem_line *line;
	struct media_job job;
	struct wake_signal *wake;
	bool busy; /* Native owner only. */
	bool pending, completed, stopping; /* Mailbox mutex. */
};

static void *worker_main(void *context)
{
	struct media_worker *worker = context;
	pthread_mutex_lock(&worker->mutex);
	for (;;) {
		while (!worker->pending && !worker->stopping)
			pthread_cond_wait(&worker->ready, &worker->mutex);
		if (worker->stopping) {
			pthread_mutex_unlock(&worker->mutex);
			return NULL;
		}
		struct media_job job = worker->job;
		worker->pending = false;
		pthread_mutex_unlock(&worker->mutex);
		job.result = modem_line_process_media(worker->line, job.now_ns, &job.datagrams);
		pthread_mutex_lock(&worker->mutex);
		job.notified = worker->wake != NULL && wake_signal_send(worker->wake);
		/* Publish after notifying so collection cannot consume an absent wake. */
		worker->job = job;
		worker->completed = true;
		pthread_cond_signal(&worker->done);
	}
}

struct media_worker *media_worker_create(struct modem_line *line, struct wake_signal *wake)
{
	if (line == NULL || (wake != NULL && !wake_signal_is_open(wake))) {
		errno = EINVAL;
		return NULL;
	}
	struct media_worker *worker = calloc(1, sizeof(*worker));
	if (worker == NULL)
		return NULL;
	worker->line = line;
	worker->wake = wake;
	int error = pthread_mutex_init(&worker->mutex, NULL);
	if (error != 0)
		goto fail;
	error = pthread_cond_init(&worker->ready, NULL);
	if (error != 0)
		goto fail_mutex;
	error = pthread_cond_init(&worker->done, NULL);
	if (error != 0)
		goto fail_ready;
	/* PCM call transitions nest large scratch frames; macOS defaults to 512 KiB. */
	pthread_attr_t attributes;
	error = pthread_attr_init(&attributes);
	if (error != 0)
		goto fail_done;
	error = pthread_attr_setstacksize(&attributes, 8u * 1024u * 1024u);
	if (error == 0)
		error = pthread_create(&worker->thread, &attributes, worker_main, worker);
	pthread_attr_destroy(&attributes);
	if (error == 0)
		return worker;
fail_done:
	pthread_cond_destroy(&worker->done);
fail_ready:
	pthread_cond_destroy(&worker->ready);
fail_mutex:
	pthread_mutex_destroy(&worker->mutex);
fail:
	free(worker);
	errno = error;
	return NULL;
}

bool media_worker_busy(const struct media_worker *worker)
{
	return worker != NULL && worker->busy;
}

bool media_worker_submit(struct media_worker *worker, uint64_t now_ns)
{
	if (worker == NULL || worker->busy)
		return false;
	pthread_mutex_lock(&worker->mutex);
	worker->job = (struct media_job){ .now_ns = now_ns };
	worker->pending = worker->busy = true;
	pthread_cond_signal(&worker->ready);
	pthread_mutex_unlock(&worker->mutex);
	return true;
}

bool media_worker_collect(struct media_worker *worker, bool wait, struct media_job *job)
{
	if (worker == NULL || job == NULL || !worker->busy)
		return false;
	int error = wait ? pthread_mutex_lock(&worker->mutex) : pthread_mutex_trylock(&worker->mutex);
	if (error == EBUSY && !wait)
		return false;
	if (error != 0)
		abort();
	while (wait && !worker->completed)
		pthread_cond_wait(&worker->done, &worker->mutex);
	bool completed = worker->completed;
	if (completed) {
		*job = worker->job;
		worker->busy = worker->completed = false;
	}
	pthread_mutex_unlock(&worker->mutex);
	return completed;
}

void media_worker_destroy(struct media_worker *worker)
{
	if (worker == NULL)
		return;
	struct media_job job;
	(void)media_worker_collect(worker, true, &job);
	pthread_mutex_lock(&worker->mutex);
	worker->stopping = true;
	pthread_cond_signal(&worker->ready);
	pthread_mutex_unlock(&worker->mutex);
	pthread_join(worker->thread, NULL);
	pthread_cond_destroy(&worker->done);
	pthread_cond_destroy(&worker->ready);
	pthread_mutex_destroy(&worker->mutex);
	free(worker);
}
