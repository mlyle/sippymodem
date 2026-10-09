// SPDX-License-Identifier: BSD-3-Clause
#include "wake_signal.h"
#include "test.h"

#include <errno.h>
#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#endif
#include <pthread.h>

static void *writer(void *context)
{
	struct wake_signal *wake = context;
	for (unsigned count = 0; count < 48; ++count)
		if (!wake_signal_send(wake))
			return (void *)1;
	return NULL;
}

static bool test_counted_wakes(void)
{
	struct wake_signal wake = WAKE_SIGNAL_INITIALIZER;
	CHECK(wake_signal_open(&wake), "open failed");
	CHECK(!wake_signal_open(&wake) && errno == EBUSY, "reopen leaked owner");
#ifndef _WIN32
	CHECK((fcntl(wake.read_fd, F_GETFL) & O_NONBLOCK) &&
	      (fcntl(wake.write_fd, F_GETFL) & O_NONBLOCK) &&
	      (fcntl(wake.read_fd, F_GETFD) & FD_CLOEXEC) &&
	      (fcntl(wake.write_fd, F_GETFD) & FD_CLOEXEC), "descriptor flags differ");
	CHECK(wake_signal_handle(&wake) == NULL, "POSIX owner has Windows handle");
#else
	HANDLE event = wake_signal_handle(&wake);
	DWORD flags = HANDLE_FLAG_INHERIT;
	CHECK(event != NULL && wake_signal_fd(&wake) == -1 &&
	      GetHandleInformation(event, &flags) && !(flags & HANDLE_FLAG_INHERIT),
	      "Windows handle flags differ");
	CHECK(WaitForSingleObject(event, 0) == WAIT_TIMEOUT, "empty event was signaled");
#endif
	CHECK(!wake_signal_take(&wake) && errno == EAGAIN, "empty take blocked or succeeded");
	pthread_t threads[4];
	for (unsigned i = 0; i < 4; ++i)
		CHECK(pthread_create(&threads[i], NULL, writer, &wake) == 0, "writer failed");
	for (unsigned i = 0; i < 4; ++i) {
		void *result;
		CHECK(pthread_join(threads[i], &result) == 0 && result == NULL, "bounded send failed");
	}
	for (unsigned i = 0; i < 192; ++i) {
#ifdef _WIN32
		CHECK(WaitForSingleObject(event, 0) == WAIT_OBJECT_0 &&
		      WaitForSingleObject(event, 0) == WAIT_OBJECT_0,
		      "waiting consumed wake %u", i);
#else
		struct pollfd observed = { .fd = wake_signal_fd(&wake), .events = POLLIN };
		CHECK(poll(&observed, 1, 0) == 1, "missing readiness %u", i);
#endif
		CHECK(wake_signal_take(&wake), "lost wake %u", i);
	}
#ifdef _WIN32
	CHECK(WaitForSingleObject(event, 0) == WAIT_TIMEOUT, "drained event stayed signaled");
#endif
	CHECK(!wake_signal_take(&wake) && errno == EAGAIN, "extra wake");
	wake_signal_close(&wake);
	wake_signal_close(&wake);
	CHECK(!wake_signal_is_open(&wake) && wake_signal_fd(&wake) == -1 &&
	      wake_signal_handle(&wake) == NULL, "close retained wake owner");
	CHECK(!wake_signal_send(&wake) && errno == EBADF, "closed send succeeded");
	CHECK(!wake_signal_open(NULL) && errno == EINVAL, "null signal accepted");
	wake_signal_close(NULL);
	return true;
}

int main(void)
{
	return test_counted_wakes() ? 0 : 1;
}
