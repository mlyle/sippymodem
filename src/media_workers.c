// SPDX-License-Identifier: BSD-3-Clause
#include "media_workers.h"
#include "line.h"

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdlib.h>

enum { MEDIA_JOB_LIMIT = 32 };
struct media_worker {
	pthread_t thread;
	sem_t ready;
	struct media_workers *owner;
};
struct media_workers {
	struct media_worker workers[MEDIA_JOB_LIMIT - 1];
	unsigned int worker_count;
	sem_t done;
	struct media_job *jobs;
	atomic_size_t next;
	size_t count;
	bool stopping;
};

static void wait_for(sem_t *semaphore)
{
	while (sem_wait(semaphore) != 0)
		if (errno != EINTR)
			abort();
}

static void process_job(struct media_job *job)
{
	job->datagrams = 0;
	job->result = modem_line_process_media(job->line, job->now_ns, &job->datagrams);
}

static void run_jobs(struct media_workers *workers)
{
	for (;;) {
		size_t index = atomic_fetch_add(&workers->next, 1);
		if (index >= workers->count)
			return;
		process_job(&workers->jobs[index]);
	}
}

static void *worker_main(void *context)
{
	struct media_worker *worker = context;
	struct media_workers *workers = worker->owner;
	for (;;) {
		wait_for(&worker->ready);
		if (workers->stopping)
			return NULL;
		run_jobs(workers);
		sem_post(&workers->done);
	}
}

struct media_workers *media_workers_create(unsigned int threads)
{
	if (threads == 0 || threads > MEDIA_JOB_LIMIT) {
		errno = EINVAL;
		return NULL;
	}
	struct media_workers *workers = calloc(1, sizeof(*workers));
	if (workers == NULL)
		return NULL;
	atomic_init(&workers->next, 0);
	if (sem_init(&workers->done, 0, 0) != 0) {
		free(workers);
		return NULL;
	}
	for (unsigned int i = 1; i < threads; ++i) {
		struct media_worker *worker = &workers->workers[workers->worker_count];
		worker->owner = workers;
		int error;
		if (sem_init(&worker->ready, 0, 0) != 0)
			error = errno;
		else {
			error = pthread_create(&worker->thread, NULL, worker_main, worker);
			if (error != 0)
				sem_destroy(&worker->ready);
		}
		if (error != 0) {
			media_workers_destroy(workers);
			errno = error;
			return NULL;
		}
		workers->worker_count++;
	}
	return workers;
}

void media_workers_destroy(struct media_workers *workers)
{
	if (workers == NULL)
		return;
	workers->stopping = true;
	for (unsigned int i = 0; i < workers->worker_count; ++i)
		sem_post(&workers->workers[i].ready);
	for (unsigned int i = 0; i < workers->worker_count; ++i) {
		pthread_join(workers->workers[i].thread, NULL);
		sem_destroy(&workers->workers[i].ready);
	}
	sem_destroy(&workers->done);
	free(workers);
}

bool media_workers_run(struct media_workers *workers, struct media_job *jobs,
	size_t count)
{
	if (workers == NULL || count > MEDIA_JOB_LIMIT || (count != 0 && jobs == NULL))
		return false;
	for (size_t i = 0; i < count; ++i) {
		if (jobs[i].line == NULL)
			return false;
		for (size_t j = 0; j < i; ++j)
			if (jobs[i].line == jobs[j].line)
				return false;
	}
	if (workers->worker_count == 0 || count < 2) {
		for (size_t i = 0; i < count; ++i)
			process_job(&jobs[i]);
		return true;
	}
	workers->jobs = jobs;
	workers->count = count;
	atomic_store(&workers->next, 0);
	unsigned int helpers = count - 1 < workers->worker_count ?
		(unsigned int)count - 1 : workers->worker_count;
	for (unsigned int i = 0; i < helpers; ++i)
		sem_post(&workers->workers[i].ready);
	run_jobs(workers);
	for (unsigned int i = 0; i < helpers; ++i)
		wait_for(&workers->done);
	workers->jobs = NULL;
	return true;
}
