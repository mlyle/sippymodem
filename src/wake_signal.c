// SPDX-License-Identifier: BSD-3-Clause
#include "wake_signal.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#if defined(__linux__) && !defined(WAKE_SIGNAL_PIPE)
#include <sys/eventfd.h>
#endif
#endif

bool wake_signal_open(struct wake_signal *signal)
{
	if (signal == NULL) {
		errno = EINVAL;
		return false;
	}
	if (wake_signal_is_open(signal)) {
		errno = EBUSY;
		return false;
	}
#ifdef _WIN32
	*signal = (struct wake_signal){ 0 };
	InitializeSRWLock(&signal->mutex);
	signal->event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (signal->event == NULL)
		errno = EIO;
	return signal->event != NULL;
#else
	*signal = (struct wake_signal){ -1, -1 };
#if defined(__linux__) && !defined(WAKE_SIGNAL_PIPE)
	int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE);
	if (fd < 0)
		return false;
	*signal = (struct wake_signal){ fd, fd };
#else
	int fds[2];
	if (pipe(fds) != 0)
		return false;
	for (unsigned index = 0; index < 2; ++index) {
		int flags = fcntl(fds[index], F_GETFL, 0);
		int descriptor_flags = fcntl(fds[index], F_GETFD, 0);
		if (flags < 0 || descriptor_flags < 0 ||
		    fcntl(fds[index], F_SETFL, flags | O_NONBLOCK) < 0 ||
		    fcntl(fds[index], F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
			int error = errno;
			close(fds[0]);
			close(fds[1]);
			errno = error;
			return false;
		}
	}
	*signal = (struct wake_signal){ fds[0], fds[1] };
#endif
	return true;
#endif
}

bool wake_signal_is_open(const struct wake_signal *signal)
{
#ifdef _WIN32
	return signal != NULL && signal->event != NULL;
#else
	return signal != NULL && signal->read_fd >= 0 && signal->write_fd >= 0;
#endif
}

void wake_signal_close(struct wake_signal *signal)
{
	if (signal == NULL)
		return;
#ifdef _WIN32
	if (signal->event != NULL)
		CloseHandle(signal->event);
	*signal = (struct wake_signal){ 0 };
#else
	if (signal->read_fd >= 0)
		close(signal->read_fd);
	if (signal->write_fd >= 0 && signal->write_fd != signal->read_fd)
		close(signal->write_fd);
	*signal = (struct wake_signal){ -1, -1 };
#endif
}

bool wake_signal_send(struct wake_signal *signal)
{
	if (!wake_signal_is_open(signal)) {
		errno = EBADF;
		return false;
	}
#ifdef _WIN32
	AcquireSRWLockExclusive(&signal->mutex);
	bool sent = signal->pending != UINT_MAX && SetEvent(signal->event);
	if (sent)
		signal->pending++;
	ReleaseSRWLockExclusive(&signal->mutex);
	if (!sent)
		errno = EIO;
	return sent;
#else
#if defined(__linux__) && !defined(WAKE_SIGNAL_PIPE)
	uint64_t one = 1;
#else
	uint8_t one = 1;
#endif
	ssize_t count;
	do {
		count = write(signal->write_fd, &one, sizeof(one));
	} while (count < 0 && errno == EINTR);
	return count == sizeof(one);
#endif
}

bool wake_signal_take(struct wake_signal *signal)
{
	if (!wake_signal_is_open(signal)) {
		errno = EBADF;
		return false;
	}
#ifdef _WIN32
	AcquireSRWLockExclusive(&signal->mutex);
	bool taken = signal->pending != 0;
	if (taken && signal->pending == 1 && !ResetEvent(signal->event)) {
		taken = false;
		errno = EIO;
	} else if (taken) {
		signal->pending--;
	} else {
		errno = EAGAIN;
	}
	ReleaseSRWLockExclusive(&signal->mutex);
	return taken;
#else
#if defined(__linux__) && !defined(WAKE_SIGNAL_PIPE)
	uint64_t one;
#else
	uint8_t one;
#endif
	ssize_t count;
	do {
		count = read(signal->read_fd, &one, sizeof(one));
	} while (count < 0 && errno == EINTR);
	return count == sizeof(one) && one == 1;
#endif
}

int wake_signal_fd(const struct wake_signal *signal)
{
#ifdef _WIN32
	(void)signal;
	return -1;
#else
	return wake_signal_is_open(signal) ? signal->read_fd : -1;
#endif
}

void *wake_signal_handle(const struct wake_signal *signal)
{
#ifdef _WIN32
	return wake_signal_is_open(signal) ? signal->event : NULL;
#else
	(void)signal;
	return NULL;
#endif
}
