// SPDX-License-Identifier: BSD-3-Clause
#define _DEFAULT_SOURCE
#include <sippymodem.h>
#include <sippymodem_fd.h>
#include "g711.h"
#include "test.h"
#include "sdk_io.h"
#include <pjsip.h>

#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <poll.h>
#include <semaphore.h>
#include <time.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <unistd.h>

static pthread_t io_owner;
static atomic_bool wrong_io_thread;
ssize_t __real_sendto(int, const void *, size_t, int, const struct sockaddr *, socklen_t);
ssize_t __wrap_sendto(int fd, const void *bytes, size_t count, int flags,
	const struct sockaddr *address, socklen_t length)
{
	if (!pthread_equal(io_owner, pthread_self()))
		atomic_store(&wrong_io_thread, true);
	return __real_sendto(fd, bytes, count, flags, address, length);
}
ssize_t __real_recvmsg(int, struct msghdr *, int);
ssize_t __wrap_recvmsg(int fd, struct msghdr *message, int flags)
{
	if (!pthread_equal(io_owner, pthread_self()))
		atomic_store(&wrong_io_thread, true);
	return __real_recvmsg(fd, message, flags);
}

ssize_t __real_recv(int, void *, size_t, int);
ssize_t __wrap_recv(int fd, void *bytes, size_t count, int flags)
{
	if (!pthread_equal(io_owner, pthread_self()))
		atomic_store(&wrong_io_thread, true);
	return __real_recv(fd, bytes, count, flags);
}

struct modem;
static atomic_uint dsp_epoch, dsp_count;
static atomic_bool wrong_dsp_thread, stall_enabled, stall_expired;
static atomic_uintptr_t stalled_modem;
static atomic_uint other_progress;
static sem_t stall_entered, stall_release, stall_finished;
void __real_modem_process_g711(struct modem *, enum g711_law,
	const uint8_t *, void *, const bool *, uint8_t *, void *, int, bool *);
void __wrap_modem_process_g711(struct modem *modem, enum g711_law law,
	const uint8_t *rx, void *input, const bool *mask, uint8_t *tx,
	void *output, int count, bool *exact)
{
	static _Thread_local unsigned int seen_epoch;
	static _Thread_local struct modem *owned_modem;
	unsigned int epoch = atomic_load(&dsp_epoch);
	if (epoch != 0 && seen_epoch != epoch) {
		seen_epoch = epoch;
		owned_modem = modem;
		atomic_fetch_add(&dsp_count, 1);
	}
	if (epoch != 0 && (owned_modem != modem || pthread_equal(io_owner, pthread_self())))
		atomic_store(&wrong_dsp_thread, true);
	if (atomic_load(&stall_enabled)) {
		uintptr_t empty = 0;
		if (atomic_compare_exchange_strong(&stalled_modem, &empty, (uintptr_t)modem)) {
			sem_post(&stall_entered);
			while (sem_wait(&stall_release) != 0)
				if (errno != EINTR)
					_exit(2);
		} else if (empty != (uintptr_t)modem) {
			atomic_fetch_add(&other_progress, 1);
		}
	}
	__real_modem_process_g711(modem, law, rx, input, mask, tx, output, count, exact);
}
int __real_modem_stop_now(struct modem *modem);
int __real_pjsua_destroy2(unsigned int flags);
static bool fail_native_stop, fail_sdk_stop;
int __wrap_modem_stop_now(struct modem *modem)
{
	int result = __real_modem_stop_now(modem);
	if (fail_native_stop) {
		fail_native_stop = false;
		return -1;
	}
	return result;
}
int __wrap_pjsua_destroy2(unsigned int flags)
{
	int result = __real_pjsua_destroy2(flags);
	if (fail_sdk_stop) {
		fail_sdk_stop = false;
		return -1;
	}
	return result;
}

struct modem_line;
struct media_worker;
struct wake_signal;
struct media_worker *__real_media_worker_create(struct modem_line *line, struct wake_signal *wake);
static bool fail_media_start;
struct media_worker *__wrap_media_worker_create(struct modem_line *line, struct wake_signal *wake)
{
	if (fail_media_start) {
		fail_media_start = false;
		errno = EAGAIN;
		return NULL;
	}
	return __real_media_worker_create(line, wake);
}

#define EXPECT(condition) CHECK((condition), "%s", #condition)

static bool step(struct sippymodem_server *server, uint64_t *now)
{
	uint64_t next;
	*now += 1000000;
	EXPECT(sippymodem_process(server, *now, &next) == SIPPYMODEM_OK);
	EXPECT(next >= *now);
	struct sippymodem_event event;
	while (sippymodem_poll_event(server, &event) == SIPPYMODEM_OK) {
		if (event.type == SIPPYMODEM_EVENT_CONNECTED)
			EXPECT(event.status.ready && event.status.carrier && event.status.phase == SIPPYMODEM_ONLINE && event.status.modulation != 0);
	}
	usleep(100);
	return true;
}

/* A watchdog turns a reintroduced process-wide join into a bounded failure. */
static void *stall_watchdog(void *unused)
{
	(void)unused;
	struct timespec until;
	clock_gettime(CLOCK_REALTIME, &until);
	until.tv_sec += 5;
	while (sem_timedwait(&stall_finished, &until) != 0) {
		if (errno == EINTR)
			continue;
		atomic_store(&stall_expired, true);
		atomic_store(&stall_enabled, false);
		sem_post(&stall_release);
		break;
	}
	return NULL;
}

static bool check_independent_progress(struct sippymodem_server *server, uint64_t *now,
	struct sippymodem_line **lines, size_t count)
{
	EXPECT(sem_init(&stall_entered, 0, 0) == 0);
	EXPECT(sem_init(&stall_release, 0, 0) == 0);
	EXPECT(sem_init(&stall_finished, 0, 0) == 0);
	atomic_store(&stalled_modem, 0);
	atomic_store(&other_progress, 0);
	atomic_store(&stall_expired, false);
	pthread_t watchdog;
	EXPECT(pthread_create(&watchdog, NULL, stall_watchdog, NULL) == 0);
	atomic_store(&stall_enabled, true);
	bool entered = false, passed = true;
	for (unsigned i = 0; i < 1000 && !entered && !atomic_load(&stall_expired); ++i) {
		passed = step(server, now);
		if (!passed)
			break;
		entered = sem_trywait(&stall_entered) == 0;
	}
	for (unsigned i = 0; i < 100 && entered && passed &&
	     !atomic_load(&stall_expired) && atomic_load(&other_progress) < 3; ++i)
		passed = step(server, now);
	atomic_store(&stall_enabled, false);
	sem_post(&stall_release);
	sem_post(&stall_finished);
	pthread_join(watchdog, NULL);
	/* Fence every possible gated worker before destroying its semaphores. */
	for (size_t i = 0; i < count; ++i) {
		struct sippymodem_status status;
		passed = sippymodem_get_status(lines[i], &status) == SIPPYMODEM_OK && passed;
		passed = sippymodem_set_dte_mode(lines[i], status.generation, SIPPYMODEM_DATA) == SIPPYMODEM_OK && passed;
	}
	sem_destroy(&stall_entered);
	sem_destroy(&stall_release);
	sem_destroy(&stall_finished);
	EXPECT(passed && entered && !atomic_load(&stall_expired));
	EXPECT(atomic_load(&other_progress) >= 3);
	return true;
}

static bool test_owned_loopback(void)
{
	atomic_store(&dsp_count, 0);
	atomic_store(&dsp_epoch, 1);
	struct sippymodem_server_config config;
	sippymodem_server_config_init(&config);
	config.max_lines = 4;
	config.dsp_threads = 1; /* Legacy field cannot serialize distinct lines. */
	config.public_address = "127.0.0.1";
	config.bound_address = "127.0.0.1";
	config.identity_uri = "sip:+12025550123@127.0.0.1";
	config.username = "+12025550123";
	config.password = "unused-loopback-password";
	config.domain = "127.0.0.1";
	struct sippymodem_server *server = sippymodem_server_create(&config);
	EXPECT(server != NULL);
	EXPECT(sippymodem_default_endpoint(server) != NULL && sippymodem_default_huntgroup(server) != NULL);
	EXPECT(sippymodem_endpoint_destroy(sippymodem_default_endpoint(server)) == SIPPYMODEM_BUSY);
	EXPECT(sippymodem_huntgroup_destroy(sippymodem_default_huntgroup(server)) == SIPPYMODEM_BUSY);
	struct sippymodem_line_config line_config;
	sippymodem_line_config_init(&line_config);
	line_config.modulation = 21;
	line_config.automode = false;
	line_config.ec_policy = SIPPYMODEM_EC_RAW;
	line_config.compression_types = 0;
	line_config.tx_capacity = 16;
	line_config.rx_capacity = 256;
	struct sippymodem_line *caller = sippymodem_line_create(server, &line_config);
	struct sippymodem_line *answerer = sippymodem_line_create(server, &line_config);
	struct sippymodem_line *other_caller = sippymodem_line_create(server, &line_config);
	struct sippymodem_line *other_answerer = sippymodem_line_create(server, &line_config);
	EXPECT(caller != NULL && answerer != NULL && other_caller != NULL && other_answerer != NULL);
	EXPECT(sippymodem_server_start(server) == SIPPYMODEM_OK);
	EXPECT(sippymodem_line_start(caller) == SIPPYMODEM_OK);
	EXPECT(sippymodem_line_start(answerer) == SIPPYMODEM_OK);
	EXPECT(sippymodem_line_start(other_caller) == SIPPYMODEM_OK);
	EXPECT(sippymodem_line_start(other_answerer) == SIPPYMODEM_OK);
	struct sippymodem_status caller_port, answerer_port;
	EXPECT(sippymodem_get_status(caller, &caller_port) == SIPPYMODEM_OK);
	EXPECT(sippymodem_get_status(answerer, &answerer_port) == SIPPYMODEM_OK);
	EXPECT(caller_port.rtp_port != 0 && answerer_port.rtp_port != 0 && caller_port.rtp_port != answerer_port.rtp_port);
	uint64_t now = 1000000000, next, generation;
	EXPECT(sippymodem_dial(caller, "sip:1234@127.0.0.1", &generation) == SIPPYMODEM_BUSY);
	EXPECT(sippymodem_process(server, now, &next) == SIPPYMODEM_OK);
	char target[128];
	snprintf(target, sizeof(target), "sip:1234@127.0.0.1:%u", sippymodem_listen_port(server));
	EXPECT(sippymodem_dial(caller, target, &generation) == SIPPYMODEM_OK);
	struct sippymodem_status remote = { 0 }, local = { 0 };
	for (unsigned int index = 0; index < 1000 && remote.phase != SIPPYMODEM_RINGING; ++index) {
		EXPECT(step(server, &now));
		EXPECT(sippymodem_get_status(answerer, &remote) == SIPPYMODEM_OK);
	}
	EXPECT(remote.phase == SIPPYMODEM_RINGING);
	struct sippymodem_call_facts facts;
	EXPECT(sippymodem_get_call_facts(answerer, &facts) == SIPPYMODEM_OK);
	EXPECT(strcmp(facts.called_number, "1234") == 0);
	EXPECT(facts.endpoint_id == sippymodem_endpoint_id(sippymodem_default_endpoint(server)));
	EXPECT(strcmp(facts.caller_number, "+12025550123") == 0);
	EXPECT(sippymodem_get_call_facts(caller, &facts) == SIPPYMODEM_OK && !facts.caller_number[0]);
	EXPECT(sippymodem_answer(answerer, remote.generation) == SIPPYMODEM_OK);
	/* A terminal server sends its banner at its own READY, without knowing
	 * the other line's readiness or inserting an application delay. */
	const unsigned char startup[] = { 'b', 'a', 'n', 'n', 'e', 'r', '\r', '\n', 0, '+', 255 };
	bool startup_sent[2] = { false, false };
	struct sippymodem_line *peers[] = { answerer, caller };
	for (unsigned int index = 0; index < 15000 && (!remote.ready || !local.ready); ++index) {
		EXPECT(step(server, &now));
		EXPECT(sippymodem_get_status(answerer, &remote) == SIPPYMODEM_OK);
		EXPECT(sippymodem_get_status(caller, &local) == SIPPYMODEM_OK);
		bool ready[] = { remote.ready, local.ready };
		for (unsigned int side = 0; side < 2; ++side) {
			if (ready[side] && !startup_sent[side]) {
				if (side == 0)
					EXPECT(!local.ready);
				size_t count = 0;
				EXPECT(send_when_available(server, &now, peers[side], startup, sizeof(startup), &count, step));
				EXPECT(count == sizeof(startup));
				startup_sent[side] = true;
			}
		}
	}
	EXPECT(remote.ready && local.ready && startup_sent[0] && startup_sent[1]);
	unsigned char startup_result[2][sizeof(startup)];
	size_t startup_received[2] = { 0, 0 };
	for (unsigned int index = 0; index < 5000 &&
	     (startup_received[0] < sizeof(startup) || startup_received[1] < sizeof(startup)); ++index) {
		EXPECT(step(server, &now));
		for (unsigned int side = 0; side < 2; ++side) {
			if (startup_received[side] == sizeof(startup))
				continue;
			size_t count = 0;
			enum sippymodem_result rc = sippymodem_receive(peers[side], startup_result[side] + startup_received[side],
				sizeof(startup) - startup_received[side], &count);
			EXPECT(rc == SIPPYMODEM_OK || rc == SIPPYMODEM_AGAIN);
			startup_received[side] += count;
		}
	}
	for (unsigned int side = 0; side < 2; ++side)
		CHECK(startup_received[side] == sizeof(startup) &&
		      memcmp(startup_result[side], startup, sizeof(startup)) == 0,
		      "immediate V.21 startup banner lost at side %u: %zu bytes", side, startup_received[side]);
	/* Each reader receives copied, attributed logs without consuming events. */
	struct sippymodem_log_record logs[SIPPYMODEM_LOG_CAPACITY];
	uint64_t log_cursor = 0, log_missed, second_cursor = 0;
	size_t log_count;
	EXPECT(sippymodem_read_log(caller, &log_cursor, logs, SIPPYMODEM_LOG_CAPACITY, &log_count, &log_missed) == SIPPYMODEM_OK);
	EXPECT(log_count > 0 && log_missed == 0);
	bool connected_logged = false, progress_logged = false;
	for (size_t i = 0; i < log_count; ++i) {
		EXPECT(logs[i].line_id == local.line_id && (logs[i].generation == 0 || logs[i].generation == generation));
		EXPECT(logs[i].time_ns <= now && logs[i].sequence == i + 1);
		EXPECT(strstr(logs[i].text, "unused-loopback-password") == NULL && strstr(logs[i].text, "sip:") == NULL);
		if (strncmp(logs[i].text, "connected ", 10) == 0) {
			EXPECT(strstr(logs[i].text, "ready=1 carrier=1") != NULL);
			EXPECT(strstr(logs[i].text, "modulation=21 tx=300 rx=300 tx-baud=300 rx-baud=300 ec=0 compression=0") != NULL);
			EXPECT(!logs[i].truncated);
			connected_logged = true;
		}
		if (strncmp(logs[i].text, "progress ", 9) == 0)
			progress_logged = true;
	}
	EXPECT(connected_logged && progress_logged);
	EXPECT(sippymodem_read_log(caller, &second_cursor, logs, 1, &log_count, &log_missed) == SIPPYMODEM_OK &&
	       log_count == 1 && second_cursor == 1 && log_cursor > second_cursor);
	second_cursor = 0;
	EXPECT(sippymodem_read_log(answerer, &second_cursor, logs, SIPPYMODEM_LOG_CAPACITY, &log_count, &log_missed) == SIPPYMODEM_OK);
	for (size_t i = 0; i < log_count; ++i)
		EXPECT(logs[i].line_id == remote.line_id && (logs[i].generation == 0 || logs[i].generation == remote.generation));
	EXPECT(remote.modulation == 21 && local.modulation == 21);
	EXPECT(remote.tx_bps == 300 && remote.rx_bps == 300);
	const unsigned char literal[] = { 0, '+', '+', '+', '\r', 'A', 'T', 0x11, 0x13, 255 };
	size_t accepted = 0, received = 0;
	EXPECT(send_when_available(server, &now, caller, literal, sizeof(literal), &accepted, step));
	EXPECT(accepted == sizeof(literal));
	EXPECT(sippymodem_set_dte_mode(caller, generation, SIPPYMODEM_COMMAND) == SIPPYMODEM_BUSY);
	unsigned char result[sizeof(literal)];
	for (unsigned int index = 0; index < 5000 && received < sizeof(result); ++index) {
		EXPECT(step(server, &now));
		size_t count = 0;
		enum sippymodem_result rc = sippymodem_receive(answerer, result + received,
			sizeof(result) - received, &count);
		EXPECT(rc == SIPPYMODEM_OK || rc == SIPPYMODEM_AGAIN);
		received += count;
	}
	EXPECT(received == sizeof(literal) && memcmp(result, literal, sizeof(literal)) == 0);
	EXPECT(sippymodem_set_dte_mode(caller, generation, SIPPYMODEM_COMMAND) == SIPPYMODEM_OK);
	EXPECT(send_when_available(server, &now, answerer, literal, sizeof(literal), &accepted, step));
	received = 0;
	for (unsigned int index = 0; index < 5000 && received < sizeof(result); ++index) {
		EXPECT(step(server, &now));
		size_t count = 0;
		(void)sippymodem_receive(caller, result + received, sizeof(result) - received, &count);
		received += count;
	}
	EXPECT(received == sizeof(literal) && memcmp(result, literal, sizeof(literal)) == 0);
	EXPECT(sippymodem_get_status(caller, &local) == SIPPYMODEM_OK && local.carrier && local.dte_mode == SIPPYMODEM_COMMAND);
	EXPECT(sippymodem_set_dte_mode(caller, generation, SIPPYMODEM_DATA) == SIPPYMODEM_OK);
	EXPECT(sippymodem_hangup(caller, generation - 1) == SIPPYMODEM_STALE);
	uint64_t other_generation;
	EXPECT(sippymodem_dial(other_caller, target, &other_generation) == SIPPYMODEM_OK);
	struct sippymodem_status other_remote = { 0 }, other_local = { 0 };
	for (unsigned int index = 0; index < 1000 && other_remote.phase != SIPPYMODEM_RINGING; ++index) {
		EXPECT(step(server, &now));
		EXPECT(sippymodem_get_status(other_answerer, &other_remote) == SIPPYMODEM_OK);
	}
	EXPECT(other_remote.phase == SIPPYMODEM_RINGING);
	EXPECT(sippymodem_answer(other_answerer, other_remote.generation) == SIPPYMODEM_OK);
	for (unsigned int index = 0; index < 15000 && (!other_remote.ready || !other_local.ready); ++index) {
		EXPECT(step(server, &now));
		EXPECT(sippymodem_get_status(other_answerer, &other_remote) == SIPPYMODEM_OK);
		EXPECT(sippymodem_get_status(other_caller, &other_local) == SIPPYMODEM_OK);
	}
	EXPECT(other_remote.ready && other_local.ready);
	struct sippymodem_line *all_lines[] = { caller, answerer, other_caller, other_answerer };
	EXPECT(check_independent_progress(server, &now, all_lines, 4));
	const unsigned char first_payload[] = { 1, 3, 5, 7 };
	const unsigned char second_payload[] = { 2, 4, 6, 8 };
	EXPECT(send_when_available(server, &now, caller, first_payload, sizeof(first_payload), &accepted, step));
	EXPECT(send_when_available(server, &now, other_caller, second_payload, sizeof(second_payload), &accepted, step));
	unsigned char first_rx[4], second_rx[4];
	size_t first_count = 0, second_count = 0;
	for (unsigned int index = 0; index < 5000 && (first_count < 4 || second_count < 4); ++index) {
		EXPECT(step(server, &now));
		size_t count;
		(void)sippymodem_receive(answerer, first_rx + first_count, 4 - first_count, &count);
		first_count += count;
		(void)sippymodem_receive(other_answerer, second_rx + second_count, 4 - second_count, &count);
		second_count += count;
	}
	EXPECT(first_count == 4 && second_count == 4);
	EXPECT(memcmp(first_rx, first_payload, 4) == 0 && memcmp(second_rx, second_payload, 4) == 0);
	/* Finishing one line leaves the other call's input and carrier active. */
	enum sippymodem_result finished = sippymodem_finish_tx(caller, generation);
	for (unsigned i = 0; i < 1000 && finished == SIPPYMODEM_AGAIN; ++i) {
		EXPECT(step(server, &now));
		finished = sippymodem_finish_tx(caller, generation);
	}
	EXPECT(finished == SIPPYMODEM_OK);
	EXPECT(send_when_available(server, &now, other_caller, "q", 1, &accepted, step) && accepted == 1);
	received = 0;
	for (unsigned i = 0; i < 1000 && received == 0; ++i) {
		EXPECT(step(server, &now));
		(void)sippymodem_receive(other_answerer, result, 1, &received);
	}
	EXPECT(received == 1 && result[0] == 'q');
	/* Raw receive pressure must retain its accepted prefix and end deliberately. */
	unsigned char overflow_payload[320];
	memset(overflow_payload, 'x', sizeof(overflow_payload));
	size_t overflow_sent = 0;
	for (unsigned int index = 0; index < 30000 && local.disconnect_cause != SIPPYMODEM_DISCONNECT_RX_OVERFLOW; ++index) {
		if (overflow_sent < sizeof(overflow_payload)) {
			size_t count;
			enum sippymodem_result rc = sippymodem_send(answerer, overflow_payload + overflow_sent, sizeof(overflow_payload) - overflow_sent, &count);
			EXPECT(rc == SIPPYMODEM_OK || rc == SIPPYMODEM_AGAIN || rc == SIPPYMODEM_CLOSED);
			overflow_sent += count;
		}
		EXPECT(step(server, &now));
		EXPECT(sippymodem_get_status(caller, &local) == SIPPYMODEM_OK);
	}
	EXPECT(local.disconnect_cause == SIPPYMODEM_DISCONNECT_RX_OVERFLOW && local.disconnect_local && local.rx_overflow);
	EXPECT(local.rx_ready == line_config.rx_capacity && !local.carrier && !local.ready);
	unsigned char retained[256];
	size_t retained_count;
	EXPECT(sippymodem_receive(caller, retained, sizeof(retained), &retained_count) == SIPPYMODEM_OK && retained_count == sizeof(retained));
	for (size_t index = 0; index < sizeof(retained); ++index)
		EXPECT(retained[index] == 'x');
	EXPECT(sippymodem_receive(caller, retained, sizeof(retained), &retained_count) == SIPPYMODEM_CLOSED && retained_count == 0);
	/* A new incoming generation clears per-call overflow while retaining cause. */
	for (unsigned int index = 0; index < 200; ++index)
		EXPECT(step(server, &now));
	uint64_t incoming_origin_generation;
	EXPECT(sippymodem_dial(answerer, target, &incoming_origin_generation) == SIPPYMODEM_OK);
	struct sippymodem_status reconnected = { 0 }, incoming_origin = { 0 };
	for (unsigned int index = 0; index < 1000 && reconnected.phase != SIPPYMODEM_RINGING; ++index) {
		EXPECT(step(server, &now));
		EXPECT(sippymodem_get_status(caller, &reconnected) == SIPPYMODEM_OK);
	}
	EXPECT(reconnected.phase == SIPPYMODEM_RINGING && reconnected.generation > generation && !reconnected.rx_overflow);
	EXPECT(reconnected.disconnect_generation == generation && reconnected.disconnect_cause == SIPPYMODEM_DISCONNECT_RX_OVERFLOW);
	EXPECT(sippymodem_hangup(caller, generation) == SIPPYMODEM_STALE);
	for (unsigned int index = 0; index < 20; ++index)
		EXPECT(step(server, &now));
	EXPECT(sippymodem_get_status(caller, &reconnected) == SIPPYMODEM_OK && reconnected.phase == SIPPYMODEM_RINGING);
	EXPECT(sippymodem_answer(caller, reconnected.generation) == SIPPYMODEM_OK);
	for (unsigned int index = 0; index < 15000 && (!reconnected.ready || !incoming_origin.ready); ++index) {
		EXPECT(step(server, &now));
		EXPECT(sippymodem_get_status(caller, &reconnected) == SIPPYMODEM_OK);
		EXPECT(sippymodem_get_status(answerer, &incoming_origin) == SIPPYMODEM_OK);
	}
	EXPECT(reconnected.ready && incoming_origin.ready);
	EXPECT(send_when_available(server, &now, answerer, literal, sizeof(literal), &accepted, step) && accepted == sizeof(literal));
	received = 0;
	for (unsigned int index = 0; index < 5000 && received < sizeof(result); ++index) {
		EXPECT(step(server, &now));
		size_t count;
		(void)sippymodem_receive(caller, result + received, sizeof(result) - received, &count);
		received += count;
	}
	EXPECT(received == sizeof(literal) && memcmp(result, literal, sizeof(literal)) == 0);
	EXPECT(sippymodem_hangup(answerer, incoming_origin_generation) == SIPPYMODEM_OK);
	for (unsigned int index = 0; index < 200; ++index)
		EXPECT(step(server, &now));
	EXPECT(sippymodem_line_stop(caller) == SIPPYMODEM_OK);
	for (unsigned int index = 0; index < 200; ++index)
		EXPECT(step(server, &now));
	EXPECT(sippymodem_get_status(other_answerer, &other_remote) == SIPPYMODEM_OK && other_remote.ready);
	EXPECT(sippymodem_get_status(other_caller, &other_local) == SIPPYMODEM_OK && other_local.ready);
	EXPECT(sippymodem_get_status(answerer, &remote) == SIPPYMODEM_OK && remote.started);
	EXPECT(sippymodem_line_stop(caller) == SIPPYMODEM_OK);
	/* Native PPP/socket pumping exercises retained partial TX and literal RX. */
	int peer_fd;
	struct sippymodem_fd *adapter = sippymodem_fd_create(other_caller, other_generation, &peer_fd);
	EXPECT(adapter != NULL && peer_fd >= 0);
	unsigned char fd_payload[64], fd_result[64];
	for (size_t index = 0; index < sizeof(fd_payload); ++index)
		fd_payload[index] = (unsigned char)index;
	EXPECT(send(peer_fd, fd_payload, sizeof(fd_payload), MSG_NOSIGNAL) == sizeof(fd_payload));
	size_t fd_received = 0;
	for (unsigned int index = 0; index < 10000 && fd_received < sizeof(fd_payload); ++index) {
		enum sippymodem_result rc = sippymodem_fd_process(adapter);
		EXPECT(rc == SIPPYMODEM_OK || rc == SIPPYMODEM_AGAIN);
		EXPECT(step(server, &now));
		size_t count;
		(void)sippymodem_receive(other_answerer, fd_result + fd_received, sizeof(fd_result) - fd_received, &count);
		fd_received += count;
	}
	EXPECT(fd_received == sizeof(fd_payload) && memcmp(fd_result, fd_payload, sizeof(fd_payload)) == 0);
	EXPECT(send_when_available(server, &now, other_answerer, literal, sizeof(literal), &accepted, step));
	fd_received = 0;
	for (unsigned int index = 0; index < 5000 && fd_received < sizeof(literal); ++index) {
		EXPECT(step(server, &now));
		enum sippymodem_result rc = sippymodem_fd_process(adapter);
		EXPECT(rc == SIPPYMODEM_OK || rc == SIPPYMODEM_AGAIN);
		ssize_t count = recv(peer_fd, fd_result + fd_received, sizeof(literal) - fd_received, MSG_DONTWAIT);
		if (count > 0)
			fd_received += (size_t)count;
	}
	EXPECT(fd_received == sizeof(literal) && memcmp(fd_result, literal, sizeof(literal)) == 0);
	sippymodem_fd_destroy(adapter);
	close(peer_fd);
	/* A slow diagnostic reader must never make a healthy call fail. */
	struct sippymodem_event drained;
	for (unsigned i = 0; i < SIPPYMODEM_LOG_CAPACITY + 10; ++i) {
		EXPECT(sippymodem_set_dte_mode(other_caller, other_generation,
			i % 2 ? SIPPYMODEM_DATA : SIPPYMODEM_COMMAND) == SIPPYMODEM_OK);
		while (sippymodem_poll_event(server, &drained) == SIPPYMODEM_OK) { }
	}
	EXPECT(sippymodem_get_status(other_caller, &other_local) == SIPPYMODEM_OK && other_local.ready && other_local.carrier);
	uint64_t other_cursor = 0;
	EXPECT(sippymodem_read_log(other_caller, &other_cursor, logs, SIPPYMODEM_LOG_CAPACITY, &log_count, &log_missed) == SIPPYMODEM_OK);
	EXPECT(log_count == SIPPYMODEM_LOG_CAPACITY && log_missed > 0);
	/* A stopped line retains evidence for both generations. */
	EXPECT(sippymodem_read_log(caller, &log_cursor, logs, SIPPYMODEM_LOG_CAPACITY, &log_count, &log_missed) == SIPPYMODEM_OK);
	bool new_generation_logged = false, disconnected_logged = false;
	for (size_t i = 0; i < log_count; ++i) {
		EXPECT(logs[i].line_id == local.line_id);
		new_generation_logged |= logs[i].generation == reconnected.generation;
		disconnected_logged |= strncmp(logs[i].text, "disconnected ", 13) == 0;
	}
	EXPECT(new_generation_logged && disconnected_logged);
	/* Excess coarse events fail explicitly, while terminal events survive. */
	for (unsigned int index = 0; index < 150; ++index)
		EXPECT(sippymodem_set_dte_mode(other_caller, other_generation,
			index % 2 ? SIPPYMODEM_DATA : SIPPYMODEM_COMMAND) == SIPPYMODEM_OK);
	now += 1000000;
	EXPECT(sippymodem_process(server, now, &next) == SIPPYMODEM_OVERFLOW);
	bool terminal_seen = false;
	struct sippymodem_event event;
	while (sippymodem_poll_event(server, &event) == SIPPYMODEM_OK) {
		if (event.line_id == other_local.line_id && event.type == SIPPYMODEM_EVENT_DISCONNECTED) {
			CHECK(!event.status.carrier && !event.status.ready && !event.status.off_hook, "terminal carrier=%d ready=%d offhook=%d phase=%d", event.status.carrier, event.status.ready, event.status.off_hook, event.status.phase);
			EXPECT(event.status.disconnect_cause == SIPPYMODEM_DISCONNECT_EVENTS);
			terminal_seen = true;
		}
	}
	EXPECT(terminal_seen);
	EXPECT(sippymodem_get_status(other_caller, &other_local) == SIPPYMODEM_OK);
	EXPECT(other_local.disconnect_generation == other_generation && other_local.disconnect_sequence != 0);
	EXPECT(sippymodem_poll_event(server, &event) == SIPPYMODEM_OVERFLOW);
	EXPECT(sippymodem_server_stop(server) == SIPPYMODEM_OK);
	EXPECT(sippymodem_server_stop(server) == SIPPYMODEM_OK);
	EXPECT(sippymodem_read_log(other_caller, &other_cursor, logs, SIPPYMODEM_LOG_CAPACITY, &log_count, &log_missed) == SIPPYMODEM_OK);
	disconnected_logged = false;
	for (size_t i = 0; i < log_count; ++i) {
		EXPECT(logs[i].line_id == other_local.line_id && logs[i].generation == other_generation);
		if (strstr(logs[i].text, "disconnected origin=local") != NULL && strstr(logs[i].text, "event queue overflow") != NULL)
			disconnected_logged = true;
	}
	EXPECT(disconnected_logged);

	sippymodem_server_destroy(server);
	atomic_store(&dsp_epoch, 0);
	EXPECT(atomic_load(&dsp_count) == 4 && !atomic_load(&wrong_dsp_thread));
	return true;
}

static bool test_compressed_command_and_fd_eof(void)
{
	struct sippymodem_server_config config;
	sippymodem_server_config_init(&config);
	config.max_lines = 2;
	config.public_address = config.bound_address = "127.0.0.1";
	char oversized[256];
	memset(oversized, 'x', sizeof(oversized));
	config.default_target = oversized;
	EXPECT(sippymodem_server_create(&config) == NULL);
	config.default_target = NULL;
	struct sippymodem_server *server = sippymodem_server_create(&config);
	EXPECT(server != NULL);
	struct sippymodem_line_config lc;
	sippymodem_line_config_init(&lc);
	lc.modulation = 122; /* V.22bis, without V.8 negotiation. */
	lc.automode = false;
	lc.min_bps = lc.max_bps = 2400;
	lc.ec_policy = SIPPYMODEM_EC_REQUIRED;
	lc.tx_capacity = 16;
	lc.rx_capacity = 32;
	struct sippymodem_line *caller = sippymodem_line_create(server, &lc);
	struct sippymodem_line *answerer = sippymodem_line_create(server, &lc);
	EXPECT(caller != NULL && answerer != NULL);
	EXPECT(sippymodem_server_start(server) == SIPPYMODEM_OK);
	EXPECT(sippymodem_line_start(caller) == SIPPYMODEM_OK);
	EXPECT(sippymodem_line_start(answerer) == SIPPYMODEM_OK);
	uint64_t now = 1000000000, next, generation;
	EXPECT(sippymodem_process(server, now, &next) == SIPPYMODEM_OK);
	char target[128];
	snprintf(target, sizeof(target), "sip:5678@127.0.0.1:%u", sippymodem_listen_port(server));
	EXPECT(sippymodem_dial(caller, target, &generation) == SIPPYMODEM_OK);
	struct sippymodem_status local = { 0 }, remote = { 0 };
	for (unsigned int i = 0; i < 1000 && remote.phase != SIPPYMODEM_RINGING; ++i) {
		EXPECT(step(server, &now));
		EXPECT(sippymodem_get_status(answerer, &remote) == SIPPYMODEM_OK);
	}
	EXPECT(remote.phase == SIPPYMODEM_RINGING);
	EXPECT(sippymodem_answer(answerer, remote.generation) == SIPPYMODEM_OK);
	for (unsigned int i = 0; i < 30000 && (!local.ready || !remote.ready); ++i) {
		EXPECT(step(server, &now));
		EXPECT(sippymodem_get_status(caller, &local) == SIPPYMODEM_OK);
		EXPECT(sippymodem_get_status(answerer, &remote) == SIPPYMODEM_OK);
	}
	CHECK(local.ready && remote.ready, "compressed training local=%d/%d remote=%d/%d cause=%s/%s", local.phase, local.ready, remote.phase, remote.ready, local.disconnect_reason, remote.disconnect_reason);
	EXPECT(local.error_control == SIPPYMODEM_LAPM && remote.error_control == SIPPYMODEM_LAPM);
	EXPECT(local.compression == SIPPYMODEM_COMP_V44 && remote.compression == SIPPYMODEM_COMP_V44);
	unsigned char payload[512], result[512];
	for (size_t i = 0; i < sizeof(payload); ++i)
		payload[i] = (unsigned char)(i % 37);
	EXPECT(sippymodem_set_dte_mode(caller, generation, SIPPYMODEM_COMMAND) == SIPPYMODEM_OK);
	size_t sent = 0;
	bool partial = false, pressure = false;
	/* Continue peer payload while local COMMAND retains a full receive queue. */
	for (unsigned int i = 0; i < 5000; ++i) {
		if (sent < sizeof(payload)) {
			size_t count;
			enum sippymodem_result rc = sippymodem_send(answerer, payload + sent, sizeof(payload) - sent, &count);
			EXPECT(rc == SIPPYMODEM_OK || rc == SIPPYMODEM_AGAIN);
			partial |= count < sizeof(payload) - sent;
			sent += count;
		}
		EXPECT(step(server, &now));
		EXPECT(sippymodem_get_status(caller, &local) == SIPPYMODEM_OK);
		EXPECT(local.ready && local.carrier && local.phase == SIPPYMODEM_ONLINE && local.dte_mode == SIPPYMODEM_COMMAND);
		pressure |= local.rx_ready == lc.rx_capacity;
	}
	EXPECT(partial && pressure);
	size_t received = 0;
	for (unsigned int i = 0; i < 30000 && received < sizeof(result); ++i) {
		if (sent < sizeof(payload)) {
			size_t count;
			enum sippymodem_result rc = sippymodem_send(answerer, payload + sent, sizeof(payload) - sent, &count);
			EXPECT(rc == SIPPYMODEM_OK || rc == SIPPYMODEM_AGAIN);
			sent += count;
		}
		size_t count, capacity = sizeof(result) - received;
		if (capacity > 3)
			capacity = 3;
		enum sippymodem_result rc = sippymodem_receive(caller, result + received, capacity, &count);
		EXPECT(rc == SIPPYMODEM_OK || rc == SIPPYMODEM_AGAIN);
		EXPECT(count <= capacity);
		received += count;
		EXPECT(step(server, &now));
	}
	EXPECT(sent == sizeof(payload) && received == sizeof(result) && memcmp(payload, result, sizeof(result)) == 0);
	EXPECT(sippymodem_get_status(caller, &local) == SIPPYMODEM_OK && local.ready && local.carrier && local.dte_mode == SIPPYMODEM_COMMAND);
	EXPECT(sippymodem_set_dte_mode(caller, generation, SIPPYMODEM_DATA) == SIPPYMODEM_OK);
	EXPECT(sippymodem_get_status(caller, &local) == SIPPYMODEM_OK && local.phase == SIPPYMODEM_ONLINE && local.compression == SIPPYMODEM_COMP_V44);
	/* On carrier loss, native adapter drains retained RX then signals peer EOF. */
	int peer_fd;
	struct sippymodem_fd *adapter = sippymodem_fd_create(caller, generation, &peer_fd);
	EXPECT(adapter != NULL);
	size_t accepted;
	EXPECT(send_when_available(server, &now, answerer, payload, 8, &accepted, step) && accepted == 8);
	for (unsigned int i = 0; i < 5000 && local.rx_ready < 8; ++i) {
		EXPECT(step(server, &now));
		EXPECT(sippymodem_get_status(caller, &local) == SIPPYMODEM_OK);
	}
	EXPECT(local.rx_ready == 8);
	/* More old RTP than one cleanup batch must delay replacement admission. */
	int old_rtp = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in media = { .sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK), .sin_port = htons(local.rtp_port) };
	EXPECT(old_rtp >= 0);
	const unsigned char old_packet[16] = {0x80};
	for (unsigned int i = 0; i < 200; ++i)
		EXPECT(sendto(old_rtp, old_packet, sizeof(old_packet), 0,
			(struct sockaddr *)&media, sizeof(media)) == sizeof(old_packet));
	close(old_rtp);
	fail_native_stop = true;
	EXPECT(sippymodem_hangup(caller, generation) == SIPPYMODEM_ERROR);
	enum sippymodem_result drain = sippymodem_fd_process(adapter);
	EXPECT(drain == SIPPYMODEM_OK || drain == SIPPYMODEM_CLOSED);
	EXPECT(sippymodem_get_status(caller, &local) == SIPPYMODEM_OK && local.rx_ready == 0);
	EXPECT(sippymodem_dial(caller, target, &generation) == SIPPYMODEM_BUSY);
	received = 0;
	bool eof = false;
	for (unsigned int i = 0; i < 1000 && !eof; ++i) {
		enum sippymodem_result rc = sippymodem_fd_process(adapter);
		EXPECT(rc == SIPPYMODEM_OK || rc == SIPPYMODEM_AGAIN || rc == SIPPYMODEM_CLOSED);
		ssize_t count = recv(peer_fd, result + received, sizeof(result) - received, MSG_DONTWAIT);
		if (count > 0)
			received += (size_t)count;
		else if (count == 0)
			eof = true;
		EXPECT(step(server, &now));
	}
	EXPECT(eof && received == 8 && memcmp(payload, result, 8) == 0);
	EXPECT(sippymodem_fd_poll_fd(adapter) == -1);
	enum sippymodem_result dial = SIPPYMODEM_BUSY;
	for (unsigned int i = 0; i < 8 && dial == SIPPYMODEM_BUSY; ++i) {
		dial = sippymodem_dial(caller, target, &generation);
		if (dial == SIPPYMODEM_BUSY)
			EXPECT(step(server, &now));
	}
	EXPECT(dial == SIPPYMODEM_OK);
	EXPECT(sippymodem_fd_process(adapter) == SIPPYMODEM_STALE);
	EXPECT(sippymodem_get_status(caller, &local) == SIPPYMODEM_OK && local.phase == SIPPYMODEM_DIALING);
	sippymodem_fd_destroy(adapter);
	close(peer_fd);
	/* Peer EOF retires COMMAND calls with quiet or paused queued input. */
	for (unsigned int closure_case = 0; closure_case < 2; ++closure_case) {
		remote.ready = false;
		remote.phase = SIPPYMODEM_IDLE;
		for (unsigned int i = 0; i < 1000 && remote.phase != SIPPYMODEM_RINGING; ++i) {
			EXPECT(step(server, &now));
			EXPECT(sippymodem_get_status(answerer, &remote) == SIPPYMODEM_OK);
		}
		EXPECT(remote.phase == SIPPYMODEM_RINGING);
		EXPECT(sippymodem_answer(answerer, remote.generation) == SIPPYMODEM_OK);
		local.ready = false;
		for (unsigned int i = 0; i < 30000 && (!local.ready || !remote.ready); ++i) {
			EXPECT(step(server, &now));
			EXPECT(sippymodem_get_status(caller, &local) == SIPPYMODEM_OK);
			EXPECT(sippymodem_get_status(answerer, &remote) == SIPPYMODEM_OK);
		}
		EXPECT(local.ready && remote.ready);
		EXPECT(send_when_available(server, &now, answerer, payload, 8, &accepted, step) && accepted == 8);
		received = 0;
		for (unsigned int i = 0; i < 5000 && received != 8; ++i) {
			size_t count;
			enum sippymodem_result rc = sippymodem_receive(caller, result + received, 8 - received, &count);
			EXPECT(rc == SIPPYMODEM_OK || rc == SIPPYMODEM_AGAIN);
			received += count;
			EXPECT(step(server, &now));
		}
		EXPECT(received == 8 && memcmp(payload, result, 8) == 0);
		adapter = sippymodem_fd_create(caller, generation, &peer_fd);
		EXPECT(adapter != NULL);
		EXPECT(sippymodem_set_dte_mode(caller, generation, SIPPYMODEM_COMMAND) == SIPPYMODEM_OK);
		if (closure_case != 0) {
			EXPECT(send(peer_fd, payload, 8, MSG_NOSIGNAL) == 8);
			EXPECT(sippymodem_fd_process(adapter) == SIPPYMODEM_AGAIN);
			EXPECT(sippymodem_get_status(caller, &local) == SIPPYMODEM_OK && local.tx_pending == 0);
		}
		EXPECT(shutdown(peer_fd, SHUT_WR) == 0);
		EXPECT(sippymodem_fd_process(adapter) == SIPPYMODEM_CLOSED);
		close(peer_fd);
		EXPECT(sippymodem_get_status(caller, &local) == SIPPYMODEM_OK &&
			local.disconnect_generation == generation && local.disconnect_cause == SIPPYMODEM_DISCONNECT_LOCAL && local.disconnect_local);
		EXPECT(sippymodem_fd_poll_fd(adapter) == -1);
		sippymodem_fd_destroy(adapter);
		if (closure_case == 0) {
			for (unsigned int i = 0; i < 200; ++i)
				EXPECT(step(server, &now));
			EXPECT(sippymodem_dial(caller, target, &generation) == SIPPYMODEM_OK);
		}
	}
	fail_sdk_stop = true;
	EXPECT(sippymodem_server_stop(server) == SIPPYMODEM_ERROR);
	EXPECT(sippymodem_server_stop(server) == SIPPYMODEM_OK);
	EXPECT(sippymodem_get_status(caller, &local) == SIPPYMODEM_OK && !local.started && !local.carrier);
	sippymodem_server_destroy(server);
	return true;
}

static bool test_typed_credentials(void)
{
	struct sippymodem_server_config config;
	sippymodem_server_config_init(&config);
	config.max_lines = 1;
	config.bound_address = config.public_address = "127.0.0.1";
	config.identity_uri = "sip:native@127.0.0.1";
	config.username = "native";
	config.password = "secret";
	config.domain = "127.0.0.1";
	config.register_account = false;
	struct sippymodem_server *server = sippymodem_server_create(&config);
	EXPECT(server != NULL);
	enum sippymodem_result result = sippymodem_server_start(server);
	sippymodem_server_destroy(server);
	EXPECT(result == SIPPYMODEM_OK);
	return true;
}

static bool test_typed_accounts(void)
{
	/* Use owned loopback peers for registration and direct-account calls. */
	for (unsigned int account_case = 0; account_case < 2; ++account_case) {
		bool registered = account_case == 0;
		int sink = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
		struct sockaddr_in bound = { .sin_family = AF_INET,
			.sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
		EXPECT(sink >= 0 && bind(sink, (struct sockaddr *)&bound, sizeof(bound)) == 0);
		socklen_t size = sizeof(bound);
		EXPECT(getsockname(sink, (struct sockaddr *)&bound, &size) == 0);
		char target[128];
		snprintf(target, sizeof(target), "sip:127.0.0.1:%u", ntohs(bound.sin_port));
		struct sippymodem_server_config config;
		sippymodem_server_config_init(&config);
		config.max_lines = 1;
		config.public_address = config.bound_address = "127.0.0.1";
		config.identity_uri = "sip:native@127.0.0.1";
		if (registered) {
			config.username = "native";
			config.password = "secret";
			config.domain = "127.0.0.1";
			config.registrar_uri = target;
			config.register_account = true;
		}
		struct sippymodem_server *server = sippymodem_server_create(&config);
		EXPECT(server != NULL && sippymodem_server_start(server) == SIPPYMODEM_OK);
		struct sippymodem_line_config lc;
		sippymodem_line_config_init(&lc);
		struct sippymodem_line *line = sippymodem_line_create(server, &lc);
		EXPECT(line != NULL && sippymodem_line_start(line) == SIPPYMODEM_OK);
		uint64_t now = 1000000000, next, generation;
		EXPECT(sippymodem_process(server, now, &next) == SIPPYMODEM_OK);
		if (!registered)
			EXPECT(sippymodem_dial(line, target, &generation) == SIPPYMODEM_OK);
		char packet[4096];
		ssize_t received = -1;
		for (unsigned int i = 0; i < 1000 && received < 0; ++i) {
			EXPECT(step(server, &now));
			received = recv(sink, packet, sizeof(packet) - 1, 0);
		}
		EXPECT(received > 0);
		packet[received] = '\0';
		EXPECT(strncmp(packet, registered ? "REGISTER " : "INVITE ", registered ? 9 : 7) == 0);
		EXPECT(strstr(packet, "sip:native@127.0.0.1") != NULL);
		EXPECT(sippymodem_server_stop(server) == SIPPYMODEM_OK);
		sippymodem_server_destroy(server);
		close(sink);
	}
	return true;
}

static bool test_signaling_timeout(void)
{
	struct sippymodem_server_config sc;
	sippymodem_server_config_init(&sc);
	sc.max_lines = 1;
	sc.public_address = sc.bound_address = "127.0.0.1";
	struct sippymodem_server *server = sippymodem_server_create(&sc);
	EXPECT(server != NULL);
	struct sippymodem_line_config lc;
	sippymodem_line_config_init(&lc);
	lc.modulation = 21;
	lc.automode = false;
	lc.ec_policy = SIPPYMODEM_EC_RAW;
	lc.compression_types = 0;
	lc.call_timeout_ms = 5000;
	struct sippymodem_line *line = sippymodem_line_create(server, &lc);
	EXPECT(line != NULL && sippymodem_server_start(server) == SIPPYMODEM_OK &&
	       sippymodem_line_start(line) == SIPPYMODEM_OK);
	uint64_t now = 1000000000, next, generation;
	EXPECT(sippymodem_process(server, now, &next) == SIPPYMODEM_OK);
	int sink = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in bound = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	EXPECT(sink >= 0 && bind(sink, (struct sockaddr *)&bound, sizeof(bound)) == 0);
	socklen_t size = sizeof(bound);
	EXPECT(getsockname(sink, (struct sockaddr *)&bound, &size) == 0);
	char target[128];
	snprintf(target, sizeof(target), "sip:100@127.0.0.1:%u", ntohs(bound.sin_port));
	/* Exercise the real SIP transaction timer before the SDK's call deadline. */
	unsigned int old_timeout = pjsip_cfg()->tsx.td;
	pjsip_tsx_set_timers(0, 0, 0, 100);
	enum sippymodem_result dialed = sippymodem_dial(line, target, &generation);
	struct sippymodem_status status = { 0 };
	bool processed = true;
	for (unsigned int i = 0; i < 1000 && !status.disconnect_sequence && processed; ++i) {
		processed = step(server, &now) && sippymodem_get_status(line, &status) == SIPPYMODEM_OK;
		usleep(1000);
	}
	pjsip_tsx_set_timers(0, 0, 0, old_timeout);
	bool correct = dialed == SIPPYMODEM_OK && processed &&
		status.disconnect_cause == SIPPYMODEM_DISCONNECT_TIMEOUT && status.disconnect_local &&
		status.disconnect_generation == generation && status.disconnect_sequence == 1 &&
		strcmp(status.disconnect_reason, "SIP transaction timed out") == 0;
	close(sink);
	sippymodem_server_destroy(server);
	CHECK(correct, "SIP timer misreported: cause=%u local=%d reason=%s",
	      status.disconnect_cause, status.disconnect_local, status.disconnect_reason);
	return true;
}

static bool test_timeout_and_bounds(void)
{
	struct sippymodem_server_config config;
	sippymodem_server_config_init(&config);
	config.max_lines = 2;
	config.dsp_threads = 33;
	EXPECT(sippymodem_server_create(&config) == NULL && errno == EINVAL);
	config.dsp_threads = 1;
	config.public_address = config.bound_address = "127.0.0.1";
	struct sippymodem_server *server = sippymodem_server_create(&config);
	EXPECT(server != NULL);
	struct sippymodem_line_config lc;
	sippymodem_line_config_init(&lc);
	lc.modulation = 21;
	lc.automode = false;
	lc.ec_policy = SIPPYMODEM_EC_RAW;
	lc.compression_types = 0;
	lc.call_timeout_ms = 1000;
	lc.tx_capacity = 0;
	EXPECT(sippymodem_line_create(server, &lc) == NULL);
	lc.tx_capacity = 16;
	lc.modulation = 1;
	EXPECT(sippymodem_line_create(server, &lc) == NULL);
	lc.modulation = 21;
	lc.min_bps = UINT32_MAX;
	EXPECT(sippymodem_line_create(server, &lc) == NULL);
	lc.min_bps = 1;
	EXPECT(sippymodem_line_create(server, &lc) == NULL);
	lc.min_bps = 0;
	struct sippymodem_line *line = sippymodem_line_create(server, &lc);
	EXPECT(line != NULL);
	EXPECT(sippymodem_server_start(server) == SIPPYMODEM_OK);
	fail_media_start = true;
	EXPECT(sippymodem_line_start(line) == SIPPYMODEM_ERROR);
	EXPECT(sippymodem_line_start(line) == SIPPYMODEM_OK);
	EXPECT(sippymodem_line_start(line) == SIPPYMODEM_OK);
	struct sippymodem_status reserved;
	EXPECT(sippymodem_get_status(line, &reserved) == SIPPYMODEM_OK && reserved.rtp_port != 0);
	lc.rtp_port = reserved.rtp_port;
	struct sippymodem_line *conflict = sippymodem_line_create(server, &lc);
	EXPECT(conflict != NULL && sippymodem_line_start(conflict) == SIPPYMODEM_ERROR);
	sippymodem_line_destroy(conflict);
	EXPECT(sippymodem_get_status(line, &reserved) == SIPPYMODEM_OK && reserved.started && reserved.rtp_port != 0);
	uint64_t now = 1000000000000, next, generation;
	EXPECT(sippymodem_process(server, now, &next) == SIPPYMODEM_OK);
	EXPECT(sippymodem_process(server, now - 1, &next) == SIPPYMODEM_INVALID);
	int sink = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in bound = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	EXPECT(sink >= 0 && bind(sink, (struct sockaddr *)&bound, sizeof(bound)) == 0);
	socklen_t size = sizeof(bound);
	EXPECT(getsockname(sink, (struct sockaddr *)&bound, &size) == 0);
	char target[128];
	snprintf(target, sizeof(target), "sip:100@127.0.0.1:%u", ntohs(bound.sin_port));
	EXPECT(sippymodem_dial(line, target, &generation) == SIPPYMODEM_OK);
	EXPECT(sippymodem_process(server, now + 1000000001, &next) == SIPPYMODEM_OK);
	struct sippymodem_status status;
	EXPECT(sippymodem_get_status(line, &status) == SIPPYMODEM_OK);
	EXPECT(status.phase == SIPPYMODEM_IDLE && status.disconnect_cause == SIPPYMODEM_DISCONNECT_TIMEOUT);
	EXPECT(status.disconnect_generation == generation && status.disconnect_sequence == 1);
	EXPECT(sippymodem_hangup(line, generation - 1) == SIPPYMODEM_STALE);
	EXPECT(sippymodem_dial(line, target, &generation) == SIPPYMODEM_OK);
	EXPECT(generation == 2);
	EXPECT(sippymodem_get_status(line, &status) == SIPPYMODEM_OK && status.disconnect_sequence == 1);
	EXPECT(sippymodem_hangup(line, generation) == SIPPYMODEM_OK);
	EXPECT(sippymodem_line_stop(line) == SIPPYMODEM_OK);
	EXPECT(sippymodem_process(server, UINT64_MAX, &next) == SIPPYMODEM_OK && next == UINT64_MAX);
	close(sink);
	sippymodem_server_destroy(server);
	return true;
}

static bool answer_registration(int socket_fd, const char *request,
	const struct sockaddr_in *remote, socklen_t remote_size)
{
	char response[4096] = "SIP/2.0 200 OK\r\n";
	size_t used = strlen(response);
	const char *names[] = { "Via:", "From:", "To:", "Call-ID:", "CSeq:", "Contact:" };
	for (unsigned int index = 0; index < 6; ++index) {
		char prefix[32];
		snprintf(prefix, sizeof(prefix), "\r\n%s", names[index]);
		const char *begin = strstr(request, prefix);
		EXPECT(begin != NULL);
		begin += 2;
		const char *end = strstr(begin, "\r\n");
		EXPECT(end != NULL && (size_t)(end - begin) < sizeof(response) - used - 64);
		memcpy(response + used, begin, (size_t)(end - begin));
		used += (size_t)(end - begin);
		if (index == 2) {
			memcpy(response + used, ";tag=owned", 10);
			used += 10;
		}
		memcpy(response + used, "\r\n", 2);
		used += 2;
	}
	const char *tail = strstr(request, "\r\nExpires: 0\r\n") != NULL ?
		"Expires: 0\r\nContent-Length: 0\r\n\r\n" : "Expires: 60\r\nContent-Length: 0\r\n\r\n";
	EXPECT(strlen(tail) < sizeof(response) - used);
	memcpy(response + used, tail, strlen(tail));
	used += strlen(tail);
	EXPECT(sendto(socket_fd, response, used, 0, (const struct sockaddr *)remote, remote_size) == (ssize_t)used);
	return true;
}

static bool test_endpoint_registrations(void)
{
	struct sippymodem_server_config sc;
	sippymodem_server_config_init(&sc);
	sc.omit_default_endpoint = true;
	struct sippymodem_server *server = sippymodem_server_create(&sc);
	EXPECT(server != NULL);
	struct sippymodem_huntgroup *group = sippymodem_huntgroup_create(server);
	EXPECT(group != NULL);
	struct sippymodem_endpoint *endpoints[2];
	int sinks[2];
	for (unsigned int index = 0; index < 2; ++index) {
		sinks[index] = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
		struct sockaddr_in address = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
		EXPECT(sinks[index] >= 0 && bind(sinks[index], (struct sockaddr *)&address, sizeof(address)) == 0);
		socklen_t size = sizeof(address);
		EXPECT(getsockname(sinks[index], (struct sockaddr *)&address, &size) == 0);
		struct sippymodem_endpoint_config ec;
		sippymodem_endpoint_config_init(&ec);
		ec.bound_address = ec.public_address = ec.domain = "127.0.0.1";
		ec.username = "authentication-user";
		ec.password = "secret";
		ec.register_account = true;
		/* Derive the incoming key from the canonical bare identity, not the
		 * deliberately different authentication username. */
		ec.identity_uri = index == 0 ? "101" : "sip:102@127.0.0.1";
		char registrar[128];
		snprintf(registrar, sizeof(registrar), "sip:127.0.0.1:%u", ntohs(address.sin_port));
		ec.registrar_uri = registrar;
		endpoints[index] = sippymodem_endpoint_create(server, group, &ec);
		EXPECT(endpoints[index] != NULL);
	}
	EXPECT(sippymodem_server_start(server) == SIPPYMODEM_OK);
	for (unsigned int index = 0; index < 2; ++index)
		EXPECT(sippymodem_endpoint_start(endpoints[index]) == SIPPYMODEM_OK);
	uint64_t now = UINT64_C(1000000000);
	for (unsigned int index = 0; index < 2; ++index) {
		struct sippymodem_endpoint_status status = { 0 };
		bool replied = false;
		for (unsigned int tick = 0; tick < 1000 && status.registration != SIPPYMODEM_REG_REGISTERED; ++tick) {
			EXPECT(step(server, &now));
			char request[4096];
			struct sockaddr_in remote;
			socklen_t remote_size = sizeof(remote);
			ssize_t size = recvfrom(sinks[index], request, sizeof(request) - 1, 0,
				(struct sockaddr *)&remote, &remote_size);
			if (size > 0) {
				request[size] = '\0';
				EXPECT(strncmp(request, "REGISTER ", 9) == 0);
				const char *contact = strstr(request, "\r\nContact:");
				if (contact != NULL)
					contact += 2;
				const char *expected = index == 0 ? "sip:101@127.0.0.1" : "sip:102@127.0.0.1";
				EXPECT(contact != NULL && strstr(contact, expected) != NULL &&
					strstr(contact, expected) < strstr(contact, "\r\n"));
				EXPECT(answer_registration(sinks[index], request, &remote, remote_size));
				replied = true;
			}
			EXPECT(sippymodem_endpoint_get_status(endpoints[index], &status) == SIPPYMODEM_OK);
		}
		EXPECT(replied && status.started && status.registration == SIPPYMODEM_REG_REGISTERED &&
			status.registration_code == 200 && status.library_status == 0);
		if (index == 0) {
			EXPECT(sippymodem_endpoint_get_status(endpoints[1], &status) == SIPPYMODEM_OK);
			EXPECT(status.registration == SIPPYMODEM_REG_REGISTERING);
		}
	}
	EXPECT(sippymodem_endpoint_stop(endpoints[0]) == SIPPYMODEM_OK);
	struct sippymodem_endpoint_status status;
	EXPECT(sippymodem_endpoint_get_status(endpoints[0], &status) == SIPPYMODEM_OK &&
		!status.started && status.registration == SIPPYMODEM_REG_UNREGISTERED);
	EXPECT(sippymodem_endpoint_get_status(endpoints[1], &status) == SIPPYMODEM_OK &&
		status.started && status.registration == SIPPYMODEM_REG_REGISTERED);
	EXPECT(sippymodem_endpoint_start(endpoints[0]) == SIPPYMODEM_OK);
	EXPECT(sippymodem_endpoint_get_status(endpoints[0], &status) == SIPPYMODEM_OK &&
		status.started && status.registration == SIPPYMODEM_REG_REGISTERING);
	EXPECT(sippymodem_endpoint_stop(endpoints[0]) == SIPPYMODEM_OK);
	EXPECT(sippymodem_endpoint_get_status(endpoints[0], &status) == SIPPYMODEM_OK &&
		!status.started && status.registration == SIPPYMODEM_REG_UNREGISTERED);
	sippymodem_server_destroy(server);
	for (unsigned int index = 0; index < 2; ++index)
		close(sinks[index]);
	return true;
}

static bool test_huntgroups(void)
{
	struct sippymodem_server_config config;
	sippymodem_server_config_init(&config);
	config.max_lines = 16;
	config.public_address = config.bound_address = "127.0.0.1";
	config.omit_default_endpoint = true;
	struct sippymodem_server *server = sippymodem_server_create(&config);
	EXPECT(server != NULL && sippymodem_default_endpoint(server) == NULL);
	struct sippymodem_huntgroup *pool = sippymodem_huntgroup_create(server);
	struct sippymodem_huntgroup *isolated = sippymodem_huntgroup_create(server);
	struct sippymodem_huntgroup *originators = sippymodem_huntgroup_create(server);
	EXPECT(pool != NULL && isolated != NULL && originators != NULL);
	struct sippymodem_line_config lc;
	sippymodem_line_config_init(&lc);
	lc.modulation = 21;
	lc.automode = false;
	lc.ec_policy = SIPPYMODEM_EC_RAW;
	lc.compression_types = 0;
	struct sippymodem_line *callers[6], *answerers[5];
	for (unsigned int index = 0; index < 6; ++index)
		EXPECT((callers[index] = sippymodem_line_create(server, &lc)) != NULL);
	for (unsigned int index = 0; index < 5; ++index) {
		EXPECT((answerers[index] = sippymodem_line_create(server, &lc)) != NULL);
		EXPECT(sippymodem_huntgroup_add(pool, answerers[index]) == SIPPYMODEM_OK);
	}
	lc.modulation = 122;
	lc.min_bps = lc.max_bps = 2400;
	struct sippymodem_line *slow_caller = sippymodem_line_create(server, &lc);
	struct sippymodem_line *slow_answerer = sippymodem_line_create(server, &lc);
	EXPECT(slow_caller != NULL && slow_answerer != NULL);
	EXPECT(sippymodem_huntgroup_add(isolated, slow_answerer) == SIPPYMODEM_OK);
	struct sippymodem_endpoint_config ec;
	sippymodem_endpoint_config_init(&ec);
	ec.public_address = ec.bound_address = ec.domain = "127.0.0.1";
	ec.request_host = "127.0.0.1";
	ec.identity_uri = "sip:out@127.0.0.1";
	struct sippymodem_endpoint *out = sippymodem_endpoint_create(server, originators, &ec);
	ec.identity_uri = "100"; /* Identity-only bare user shares canonical route derivation. */
	struct sippymodem_endpoint *first = sippymodem_endpoint_create(server, pool, &ec);
	ec.identity_uri = "sip:200@127.0.0.1";
	struct sippymodem_endpoint *second = sippymodem_endpoint_create(server, pool, &ec);
	/* Keep the distinct transport port reserved until the shared one starts. */
	int reserved_socket = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in reserved_address = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	EXPECT(reserved_socket >= 0 && bind(reserved_socket, (struct sockaddr *)&reserved_address, sizeof(reserved_address)) == 0);
	socklen_t reserved_size = sizeof(reserved_address);
	EXPECT(getsockname(reserved_socket, (struct sockaddr *)&reserved_address, &reserved_size) == 0);
	ec.listen_port = ntohs(reserved_address.sin_port);
	ec.identity_uri = "sip:300@127.0.0.1";
	struct sippymodem_endpoint *third = sippymodem_endpoint_create(server, isolated, &ec);
	EXPECT(out != NULL && first != NULL && second != NULL && third != NULL);
	EXPECT(sippymodem_huntgroup_destroy(pool) == SIPPYMODEM_BUSY);
	EXPECT(sippymodem_endpoint_start(first) == SIPPYMODEM_CLOSED);
	EXPECT(sippymodem_server_start(server) == SIPPYMODEM_OK);
	EXPECT(sippymodem_endpoint_start(out) == SIPPYMODEM_OK);
	EXPECT(sippymodem_endpoint_start(first) == SIPPYMODEM_OK);
	EXPECT(sippymodem_endpoint_start(second) == SIPPYMODEM_OK);
	close(reserved_socket);
	EXPECT(sippymodem_endpoint_start(third) == SIPPYMODEM_OK);
	unsigned int separate_port = sippymodem_endpoint_listen_port(third);
	unsigned int port = sippymodem_endpoint_listen_port(first);
	EXPECT(port != 0 && port == sippymodem_endpoint_listen_port(second) && port != separate_port);
	struct sippymodem_endpoint_status endpoint_status;
	EXPECT(sippymodem_endpoint_get_status(first, &endpoint_status) == SIPPYMODEM_OK);
	EXPECT(endpoint_status.started && endpoint_status.listen_port == port && endpoint_status.registration == SIPPYMODEM_REG_DISABLED);
	EXPECT(sippymodem_huntgroup_remove(pool, answerers[0]) == SIPPYMODEM_BUSY);
	EXPECT(sippymodem_huntgroup_add(pool, callers[0]) == SIPPYMODEM_BUSY);
	/* The owned transport cannot have two overlapping Request-URI policies. */
	ec.listen_port = 0;
	ec.identity_uri = "sip:100@127.0.0.1";
	struct sippymodem_endpoint *ambiguous = sippymodem_endpoint_create(server, pool, &ec);
	EXPECT(ambiguous != NULL && sippymodem_endpoint_start(ambiguous) == SIPPYMODEM_BUSY);
	EXPECT(sippymodem_endpoint_destroy(ambiguous) == SIPPYMODEM_OK);
	for (unsigned int index = 0; index < 6; ++index)
		EXPECT(sippymodem_line_start(callers[index]) == SIPPYMODEM_OK);
	for (unsigned int index = 0; index < 5; ++index)
		EXPECT(sippymodem_line_start(answerers[index]) == SIPPYMODEM_OK);
	EXPECT(sippymodem_line_start(slow_caller) == SIPPYMODEM_OK);
	EXPECT(sippymodem_line_start(slow_answerer) == SIPPYMODEM_OK);
	uint64_t now = UINT64_C(1000000000), next, generations[6];
	EXPECT(sippymodem_process(server, now, &next) == SIPPYMODEM_OK);
	char target[128];
	snprintf(target, sizeof(target), "sip:100@127.0.0.1:%u", port);
	EXPECT(sippymodem_dial(callers[0], target, &generations[0]) == SIPPYMODEM_CLOSED);
	for (unsigned int index = 0; index < 5; ++index)
		EXPECT(sippymodem_dial_from(callers[index], out, target, &generations[index]) == SIPPYMODEM_OK);
	struct sippymodem_status remote[5] = { 0 }, local[6] = { 0 };
	bool answered[5] = { 0 }, ready = false;
	for (unsigned int tick = 0; tick < 20000 && !ready; ++tick) {
		EXPECT(step(server, &now));
		ready = true;
		for (unsigned int index = 0; index < 5; ++index) {
			EXPECT(sippymodem_get_status(answerers[index], &remote[index]) == SIPPYMODEM_OK);
			EXPECT(sippymodem_get_status(callers[index], &local[index]) == SIPPYMODEM_OK);
			if (remote[index].phase == SIPPYMODEM_RINGING && !answered[index]) {
				struct sippymodem_call_facts facts;
				EXPECT(sippymodem_get_call_facts(answerers[index], &facts) == SIPPYMODEM_OK);
				EXPECT(facts.endpoint_id == sippymodem_endpoint_id(first));
				EXPECT(strcmp(facts.called_number, "100") == 0);
				EXPECT(sippymodem_answer(answerers[index], remote[index].generation) == SIPPYMODEM_OK);
				answered[index] = true;
			}
			ready = ready && remote[index].ready && local[index].ready;
		}
	}
	if (!ready)
		for (unsigned int index = 0; index < 5; ++index)
			fprintf(stderr, "hunt %u: answered=%u caller=%u/%u sip=%u disc=%s remote=%u/%u sip=%u disc=%s\n", index, answered[index], local[index].phase, local[index].ready, local[index].sip_state, local[index].disconnect_reason, remote[index].phase, remote[index].ready, remote[index].sip_state, remote[index].disconnect_reason);
	EXPECT(ready);
	/* Distinct markers prove five simultaneous calls are routed independently. */
	size_t accepted;
	for (unsigned int index = 0; index < 5; ++index) {
		unsigned char marker = (unsigned char)('a' + index);
		EXPECT(send_when_available(server, &now, callers[index], &marker, 1, &accepted, step) && accepted == 1);
	}
	for (unsigned int tick = 0; tick < 2000; ++tick)
		EXPECT(step(server, &now));
	for (unsigned int index = 1; index < 5; ++index) {
		unsigned char marker;
		size_t received;
		EXPECT(sippymodem_receive(answerers[index], &marker, 1, &received) == SIPPYMODEM_OK);
		EXPECT(received == 1 && marker == (unsigned char)('a' + index));
	}
	EXPECT(sippymodem_hangup(callers[0], generations[0]) == SIPPYMODEM_OK);
	for (unsigned int tick = 0; tick < 2000; ++tick)
		EXPECT(step(server, &now));
	EXPECT(sippymodem_get_status(answerers[0], &remote[0]) == SIPPYMODEM_OK);
	EXPECT(remote[0].phase == SIPPYMODEM_IDLE && remote[0].rx_ready == 1);
	/* Retained RX blocks the only idle member: do not spill into isolated pool. */
	EXPECT(sippymodem_dial_from(callers[5], out, target, &generations[5]) == SIPPYMODEM_OK);
	for (unsigned int tick = 0; tick < 1000; ++tick)
		EXPECT(step(server, &now));
	EXPECT(sippymodem_get_status(callers[5], &local[5]) == SIPPYMODEM_OK && local[5].phase == SIPPYMODEM_IDLE);
	struct sippymodem_status separate;
	EXPECT(sippymodem_get_status(slow_answerer, &separate) == SIPPYMODEM_OK && separate.generation == 0);
	unsigned char marker;
	size_t received;
	EXPECT(sippymodem_receive(answerers[0], &marker, 1, &received) == SIPPYMODEM_OK && received == 1 && marker == 'a');
	/* Both endpoints share the same five-line pool, with exact owner facts. */
	snprintf(target, sizeof(target), "sip:200@127.0.0.1:%u", port);
	EXPECT(sippymodem_dial_from(callers[0], out, target, &generations[0]) == SIPPYMODEM_OK);
	bool second_answered = false;
	ready = false;
	for (unsigned int tick = 0; tick < 20000 && !ready; ++tick) {
		EXPECT(step(server, &now));
		EXPECT(sippymodem_get_status(answerers[0], &remote[0]) == SIPPYMODEM_OK);
		EXPECT(sippymodem_get_status(callers[0], &local[0]) == SIPPYMODEM_OK);
		if (remote[0].phase == SIPPYMODEM_RINGING && !second_answered) {
			struct sippymodem_call_facts facts;
			EXPECT(sippymodem_get_call_facts(answerers[0], &facts) == SIPPYMODEM_OK);
			EXPECT(facts.endpoint_id == sippymodem_endpoint_id(second));
			EXPECT(sippymodem_answer(answerers[0], remote[0].generation) == SIPPYMODEM_OK);
			second_answered = true;
		}
		ready = remote[0].ready && local[0].ready;
	}
	EXPECT(ready);
	fail_native_stop = true;
	EXPECT(sippymodem_endpoint_stop(first) == SIPPYMODEM_ERROR);
	EXPECT(sippymodem_endpoint_stop(first) == SIPPYMODEM_OK);
	uint64_t previous_sequence = endpoint_status.state_sequence;
	EXPECT(sippymodem_endpoint_get_status(first, &endpoint_status) == SIPPYMODEM_OK);
	EXPECT(!endpoint_status.started && endpoint_status.listen_port == 0 && endpoint_status.state_sequence > previous_sequence);
	for (unsigned int tick = 0; tick < 1000; ++tick)
		EXPECT(step(server, &now));
	EXPECT(sippymodem_get_status(answerers[0], &remote[0]) == SIPPYMODEM_OK && remote[0].ready);
	for (unsigned int index = 1; index < 5; ++index)
		EXPECT(sippymodem_get_status(answerers[index], &remote[index]) == SIPPYMODEM_OK && remote[index].phase == SIPPYMODEM_IDLE);
	marker = 'z';
	EXPECT(send_when_available(server, &now, callers[0], &marker, 1, &accepted, step));
	for (unsigned int tick = 0; tick < 1500; ++tick)
		EXPECT(step(server, &now));
	EXPECT(sippymodem_receive(answerers[0], &marker, 1, &received) == SIPPYMODEM_OK && received == 1 && marker == 'z');
	/* Unknown users and unknown URI hosts cannot reach another account's pool. */
	const char *bad[] = { "sip:999@127.0.0.1", "sip:200@localhost", "sip:200@127.0.0.1" };
	for (unsigned int test = 0; test < 3; ++test) {
		snprintf(target, sizeof(target), "%s:%u", bad[test], test == 2 ? separate_port : port);
		EXPECT(sippymodem_dial_from(callers[5], out, target, &generations[5]) == SIPPYMODEM_OK);
		for (unsigned int tick = 0; tick < 1000; ++tick)
			EXPECT(step(server, &now));
		EXPECT(sippymodem_get_status(callers[5], &local[5]) == SIPPYMODEM_OK && local[5].phase == SIPPYMODEM_IDLE);
		for (unsigned int index = 1; index < 5; ++index)
			EXPECT(sippymodem_get_status(answerers[index], &remote[index]) == SIPPYMODEM_OK && remote[index].generation == 1);
	}
	/* A separate group keeps its own modem settings even with idle faster lines. */
	snprintf(target, sizeof(target), "sip:300@127.0.0.1:%u", separate_port);
	uint64_t slow_generation;
	EXPECT(sippymodem_dial_from(slow_caller, out, target, &slow_generation) == SIPPYMODEM_OK);
	struct sippymodem_status slow_local = { 0 };
	bool slow_answered = false;
	ready = false;
	for (unsigned int tick = 0; tick < 20000 && !ready; ++tick) {
		EXPECT(step(server, &now));
		EXPECT(sippymodem_get_status(slow_answerer, &separate) == SIPPYMODEM_OK);
		EXPECT(sippymodem_get_status(slow_caller, &slow_local) == SIPPYMODEM_OK);
		if (separate.phase == SIPPYMODEM_RINGING && !slow_answered) {
			EXPECT(sippymodem_answer(slow_answerer, separate.generation) == SIPPYMODEM_OK);
			slow_answered = true;
		}
		ready = separate.ready && slow_local.ready;
	}
	EXPECT(ready && separate.modulation == 122 && separate.rx_bps == 2400);
	unsigned int old_id = sippymodem_endpoint_id(second);
	EXPECT(sippymodem_endpoint_destroy(second) == SIPPYMODEM_OK);
	struct sippymodem_call_facts retained;
	EXPECT(sippymodem_get_call_facts(answerers[0], &retained) == SIPPYMODEM_OK && retained.endpoint_id == old_id);
	EXPECT(sippymodem_get_status(slow_answerer, &separate) == SIPPYMODEM_OK && separate.ready);
	ec.identity_uri = "sip:200@127.0.0.1";
	second = sippymodem_endpoint_create(server, pool, &ec);
	EXPECT(second != NULL && sippymodem_endpoint_id(second) > old_id);
	EXPECT(sippymodem_endpoint_start(second) == SIPPYMODEM_OK);
	for (unsigned int tick = 0; tick < 1000; ++tick)
		EXPECT(step(server, &now));
	snprintf(target, sizeof(target), "sip:200@127.0.0.1:%u", port);
	EXPECT(sippymodem_dial_from(callers[0], out, target, &generations[0]) == SIPPYMODEM_OK);
	for (unsigned int tick = 0; tick < 1000 && remote[0].phase != SIPPYMODEM_RINGING; ++tick) {
		EXPECT(step(server, &now));
		EXPECT(sippymodem_get_status(answerers[0], &remote[0]) == SIPPYMODEM_OK);
	}
	EXPECT(remote[0].phase == SIPPYMODEM_RINGING);
	EXPECT(sippymodem_get_call_facts(answerers[0], &retained) == SIPPYMODEM_OK && retained.endpoint_id == sippymodem_endpoint_id(second));
	sippymodem_server_destroy(server);
	return true;
}

static bool test_startup_transmit_completion(void)
{
	const struct { unsigned modulation, jitter_ms, length; } profiles[] = {
		{ 21, 0, 7 }, { 21, 60, 7 }, { 103, 60, 7 }, { 21, 60, 0 },
	};
	static const unsigned char greeting[] = "login: ";
	for (unsigned profile = 0; profile < sizeof(profiles) / sizeof(profiles[0]); ++profile) {
		struct sippymodem_server_config config;
		sippymodem_server_config_init(&config);
		config.max_lines = 2;
		config.dsp_threads = profile == 0 ? 1 : 2;
		config.public_address = config.bound_address = "127.0.0.1";
		struct sippymodem_server *server = sippymodem_server_create(&config);
		EXPECT(server != NULL);
		struct sippymodem_line_config lc;
		sippymodem_line_config_init(&lc);
		lc.modulation = profiles[profile].modulation;
		lc.automode = false;
		lc.min_bps = lc.max_bps = 300;
		lc.ec_policy = SIPPYMODEM_EC_RAW;
		lc.compression_types = 0;
		lc.tx_capacity = 16;
		lc.jitter_ms = profiles[profile].jitter_ms;
		lc.remote_clock = false;
		struct sippymodem_line *caller = sippymodem_line_create(server, &lc);
		lc.remote_clock = true;
		struct sippymodem_line *answerer = sippymodem_line_create(server, &lc);
		EXPECT(caller != NULL && answerer != NULL);
		EXPECT(sippymodem_server_start(server) == SIPPYMODEM_OK);
		EXPECT(sippymodem_line_start(caller) == SIPPYMODEM_OK);
		EXPECT(sippymodem_line_start(answerer) == SIPPYMODEM_OK);
		uint64_t now = 1000000000, next, generation;
		EXPECT(sippymodem_process(server, now, &next) == SIPPYMODEM_OK);
		char target[128];
		snprintf(target, sizeof(target), "sip:5678@127.0.0.1:%u", sippymodem_listen_port(server));
		EXPECT(sippymodem_dial(caller, target, &generation) == SIPPYMODEM_OK);
		bool answered = false, submitted = false, complete = false, caller_ready = false;
		size_t count = 0;
		unsigned char received[sizeof(greeting)] = { 0 };
		for (unsigned i = 0; i < 30000; ++i) {
			EXPECT(step(server, &now));
			struct sippymodem_status local, remote;
			EXPECT(sippymodem_get_status(caller, &local) == SIPPYMODEM_OK);
			EXPECT(sippymodem_get_status(answerer, &remote) == SIPPYMODEM_OK);
			caller_ready |= local.ready;
			if (remote.phase == SIPPYMODEM_RINGING && !answered) {
				EXPECT(sippymodem_answer(answerer, remote.generation) == SIPPYMODEM_OK);
				EXPECT(sippymodem_answer(answerer, remote.generation) == SIPPYMODEM_BUSY);
				answered = true;
			}
			if (remote.ready && !submitted) {
				/* A service can close before the caller reports CONNECT. */
				EXPECT(!local.ready);
				size_t accepted;
				EXPECT(send_when_available(server, &now, answerer, greeting, profiles[profile].length, &accepted, step));
				EXPECT(accepted == profiles[profile].length);
				submitted = true;
			}
			if (submitted && !complete) {
				enum sippymodem_result result = sippymodem_finish_tx(answerer, remote.generation);
				EXPECT(result == SIPPYMODEM_OK || result == SIPPYMODEM_AGAIN);
				if (result == SIPPYMODEM_OK) {
					EXPECT(caller_ready);
					/* Teardown immediately, without another media tick. */
					EXPECT(sippymodem_hangup(answerer, remote.generation) == SIPPYMODEM_OK);
					complete = true;
				}
			}
			size_t amount;
			enum sippymodem_result result = sippymodem_receive(caller, received + count, sizeof(received) - count, &amount);
			EXPECT(result == SIPPYMODEM_OK || result == SIPPYMODEM_AGAIN || result == SIPPYMODEM_CLOSED);
			count += amount;
			if (complete && result == SIPPYMODEM_CLOSED)
				break;
		}
		CHECK(complete && count == profiles[profile].length && !memcmp(received, greeting, count),
		      "startup EOF profile=%u complete=%d received=%zu", profile, complete, count);
		sippymodem_server_destroy(server);
	}
	return true;
}

static bool test_transmit_completion(void)
{
	const struct {
		unsigned modulation, compression;
		bool caller_remote_clock;
		unsigned jitter_ms;
		enum sippymodem_error_control ec;
		enum sippymodem_compression negotiated;
	} profiles[] = {
		{ 21, 0, false, 60, SIPPYMODEM_RAW, SIPPYMODEM_COMP_NONE },
		{ 21, 0, false, 0, SIPPYMODEM_RAW, SIPPYMODEM_COMP_NONE },
		{ 21, 0, true, 60, SIPPYMODEM_RAW, SIPPYMODEM_COMP_NONE },
		{ 21, 0, false, 200, SIPPYMODEM_RAW, SIPPYMODEM_COMP_NONE },
		{ 122, 0, true, 60, SIPPYMODEM_LAPM, SIPPYMODEM_COMP_NONE },
		{ 122, 1, true, 60, SIPPYMODEM_LAPM, SIPPYMODEM_COMP_V42BIS },
		{ 122, 2, true, 60, SIPPYMODEM_LAPM, SIPPYMODEM_COMP_V44 },
	};
	EXPECT(sippymodem_finish_tx(NULL, 0) == SIPPYMODEM_INVALID);
	for (unsigned profile = 0; profile < sizeof(profiles) / sizeof(profiles[0]); ++profile) {
		struct sippymodem_server_config config;
		sippymodem_server_config_init(&config);
		config.max_lines = 2;
		config.public_address = config.bound_address = "127.0.0.1";
		struct sippymodem_server *server = sippymodem_server_create(&config);
		EXPECT(server != NULL);
		struct sippymodem_line_config lc;
		sippymodem_line_config_init(&lc);
		lc.modulation = profiles[profile].modulation;
		lc.automode = false;
		lc.min_bps = lc.max_bps = lc.modulation == 21 ? 300 : 2400;
		lc.ec_policy = profiles[profile].ec == SIPPYMODEM_RAW ?
			SIPPYMODEM_EC_RAW : SIPPYMODEM_EC_REQUIRED;
		lc.compression_types = profiles[profile].compression;
		lc.tx_capacity = 16;
		lc.jitter_ms = profiles[profile].jitter_ms;
		lc.remote_clock = profiles[profile].caller_remote_clock;
		lc.rx_capacity = 512;
		struct sippymodem_line *caller = sippymodem_line_create(server, &lc);
		lc.remote_clock = true;
		struct sippymodem_line *answerer = sippymodem_line_create(server, &lc);
		EXPECT(caller != NULL && answerer != NULL);
		EXPECT(sippymodem_finish_tx(answerer, 0) == SIPPYMODEM_CLOSED);
		EXPECT(sippymodem_server_start(server) == SIPPYMODEM_OK);
		EXPECT(sippymodem_line_start(caller) == SIPPYMODEM_OK);
		EXPECT(sippymodem_line_start(answerer) == SIPPYMODEM_OK);
		uint64_t now = 1000000000, next, generation;
		EXPECT(sippymodem_process(server, now, &next) == SIPPYMODEM_OK);
		char target[128];
		snprintf(target, sizeof(target), "sip:5678@127.0.0.1:%u", sippymodem_listen_port(server));
		EXPECT(sippymodem_dial(caller, target, &generation) == SIPPYMODEM_OK);
		struct sippymodem_status local = { 0 }, remote = { 0 };
		for (unsigned i = 0; i < 1000 && remote.phase != SIPPYMODEM_RINGING; ++i) {
			EXPECT(step(server, &now));
			EXPECT(sippymodem_get_status(answerer, &remote) == SIPPYMODEM_OK);
		}
		EXPECT(remote.phase == SIPPYMODEM_RINGING);
		EXPECT(sippymodem_finish_tx(answerer, remote.generation) == SIPPYMODEM_AGAIN);
		EXPECT(sippymodem_answer(answerer, remote.generation) == SIPPYMODEM_OK);
		for (unsigned i = 0; i < 30000 && (!local.ready || !remote.ready); ++i) {
			EXPECT(step(server, &now));
			EXPECT(sippymodem_get_status(caller, &local) == SIPPYMODEM_OK);
			EXPECT(sippymodem_get_status(answerer, &remote) == SIPPYMODEM_OK);
		}
		EXPECT(local.ready && remote.ready);
		EXPECT(local.error_control == profiles[profile].ec &&
		       local.compression == profiles[profile].negotiated);
		/* Submit the final service message through a small partial-write queue. */
		unsigned char payload[128], received[128];
		for (size_t i = 0; i < sizeof(payload); ++i)
			payload[i] = (unsigned char)(i * 31u);
		size_t accepted, count = 0;
		EXPECT(sippymodem_finish_tx(answerer, remote.generation + 1) == SIPPYMODEM_STALE);
		size_t sent = 0;
		for (unsigned i = 0; i < 30000 && sent < sizeof(payload); ++i) {
			enum sippymodem_result result = sippymodem_send(answerer, payload + sent, sizeof(payload) - sent, &accepted);
			EXPECT(result == SIPPYMODEM_OK || result == SIPPYMODEM_AGAIN);
			EXPECT(accepted <= sizeof(payload) - sent);
			sent += accepted;
			if (sent != sizeof(payload)) {
				EXPECT(step(server, &now));
				size_t amount;
				result = sippymodem_receive(caller, received + count, sizeof(received) - count, &amount);
				EXPECT(result == SIPPYMODEM_OK || result == SIPPYMODEM_AGAIN);
				count += amount;
			}
		}
		EXPECT(sent == sizeof(payload));
		EXPECT(sippymodem_finish_tx(answerer, remote.generation) == SIPPYMODEM_AGAIN);
		EXPECT(sippymodem_send(answerer, payload, 1, &accepted) == SIPPYMODEM_AGAIN && accepted == 0);
		/* The operation leaves receive and carrier active in online COMMAND. */
		EXPECT(sippymodem_set_dte_mode(caller, generation, SIPPYMODEM_COMMAND) == SIPPYMODEM_OK);
		enum sippymodem_result completion = SIPPYMODEM_AGAIN;
		bool empty_but_pending = false, command_during_finish = false;
		for (unsigned i = 0; i < 30000 && completion == SIPPYMODEM_AGAIN; ++i) {
			EXPECT(step(server, &now));
			EXPECT(sippymodem_get_status(answerer, &remote) == SIPPYMODEM_OK);
			completion = sippymodem_finish_tx(answerer, remote.generation);
			EXPECT(completion == SIPPYMODEM_OK || completion == SIPPYMODEM_AGAIN);
			empty_but_pending |= remote.tx_pending == 0 && completion == SIPPYMODEM_AGAIN;
			if (!command_during_finish && remote.tx_pending == 0 && completion == SIPPYMODEM_AGAIN) {
				EXPECT(sippymodem_set_dte_mode(answerer, remote.generation, SIPPYMODEM_COMMAND) == SIPPYMODEM_OK);
				command_during_finish = true;
			}
			size_t amount;
			enum sippymodem_result result = sippymodem_receive(caller, received + count, sizeof(received) - count, &amount);
			EXPECT(result == SIPPYMODEM_OK || result == SIPPYMODEM_AGAIN);
			count += amount;
		}
		CHECK(completion == SIPPYMODEM_OK && empty_but_pending && command_during_finish,
		      "finish profile=%u result=%d bytes=%zu pending after local queue emptied=%d",
		      profile, completion, count, empty_but_pending);
		EXPECT(sippymodem_finish_tx(answerer, remote.generation) == SIPPYMODEM_OK);
		EXPECT(sippymodem_get_status(answerer, &remote) == SIPPYMODEM_OK && remote.carrier && remote.dte_mode == SIPPYMODEM_COMMAND);
		EXPECT(sippymodem_get_status(caller, &local) == SIPPYMODEM_OK &&
		       local.carrier && local.dte_mode == SIPPYMODEM_COMMAND);
		/* No process step between OK and hangup: remote RX must retain its tail. */
		EXPECT(sippymodem_hangup(answerer, remote.generation) == SIPPYMODEM_OK);
		EXPECT(sippymodem_finish_tx(answerer, remote.generation) == SIPPYMODEM_CLOSED);
		bool saw_receive_tail = false;
		for (unsigned i = 0; i < 1000; ++i) {
			EXPECT(step(server, &now));
			EXPECT(sippymodem_get_status(caller, &local) == SIPPYMODEM_OK);
			if (local.phase == SIPPYMODEM_DISCONNECTING && local.carrier) {
				saw_receive_tail = true;
				EXPECT(!local.ready && local.modulation == profiles[profile].modulation);
				EXPECT(sippymodem_send(caller, payload, 1, &accepted) == SIPPYMODEM_CLOSED && accepted == 0);
				EXPECT(sippymodem_finish_tx(caller, generation) == SIPPYMODEM_CLOSED);
			}
		}
		EXPECT(profiles[profile].jitter_ms != 200 || saw_receive_tail);
		size_t amount;
		enum sippymodem_result result = sippymodem_receive(caller, received + count, sizeof(received) - count, &amount);
		EXPECT(result == SIPPYMODEM_OK || result == SIPPYMODEM_CLOSED);
		count += amount;
		CHECK(count == sizeof(payload) && memcmp(received, payload, sizeof(payload)) == 0,
		      "final message lost at profile=%u: %zu bytes", profile, count);
		/* Reusing a line clears its seal; an old completion cannot affect it. */
		uint64_t old_generation = remote.generation;
		EXPECT(sippymodem_dial(caller, target, &generation) == SIPPYMODEM_OK);
		bool answered = false;
		local.ready = remote.ready = false;
		for (unsigned i = 0; i < 30000 && (!local.ready || !remote.ready); ++i) {
			EXPECT(step(server, &now));
			EXPECT(sippymodem_get_status(caller, &local) == SIPPYMODEM_OK);
			EXPECT(sippymodem_get_status(answerer, &remote) == SIPPYMODEM_OK);
			if (remote.phase == SIPPYMODEM_RINGING && !answered) {
				EXPECT(sippymodem_answer(answerer, remote.generation) == SIPPYMODEM_OK);
				answered = true;
			}
		}
		EXPECT(local.ready && remote.ready && remote.generation > old_generation);
		EXPECT(sippymodem_finish_tx(answerer, old_generation) == SIPPYMODEM_STALE);
		EXPECT(send_when_available(server, &now, answerer, payload, sizeof(payload), &accepted, step) &&
		       accepted == lc.tx_capacity);
		EXPECT(sippymodem_finish_tx(answerer, remote.generation) == SIPPYMODEM_AGAIN);
		/* Cancellation remains immediate even with an unfinished final message. */
		EXPECT(sippymodem_hangup(answerer, remote.generation) == SIPPYMODEM_OK);
		EXPECT(sippymodem_finish_tx(answerer, remote.generation) == SIPPYMODEM_CLOSED);
		sippymodem_server_destroy(server);
	}
	return true;
}

int main(void)
{
	io_owner = pthread_self();
	bool passed = test_signaling_timeout() && test_startup_transmit_completion() && test_transmit_completion() && test_endpoint_registrations() && test_huntgroups() && test_typed_credentials() && test_typed_accounts() && test_owned_loopback() && test_compressed_command_and_fd_eof() && test_timeout_and_bounds();
	if (atomic_load(&wrong_io_thread))
		fprintf(stderr, "network I/O escaped the SDK owner thread\n");
	return passed && !atomic_load(&wrong_io_thread) ? 0 : 1;
}
