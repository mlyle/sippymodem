// SPDX-License-Identifier: BSD-3-Clause
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "sippymodem.h"
#include "net_socket.h"
#include "test.h"

#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static uint64_t now_ns(void)
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static bool tick(struct sippymodem_server *server)
{
	uint64_t deadline;
	CHECK(sippymodem_process(server, now_ns(), &deadline) == SIPPYMODEM_OK,
	      "process failed");
	struct sippymodem_event event;
	while (sippymodem_poll_event(server, &event) == SIPPYMODEM_OK)
		CHECK(event.type != SIPPYMODEM_EVENT_CONNECTED || event.status.ready,
		      "premature connected event");
	enum sippymodem_result result = sippymodem_wait(server, now_ns(), 50);
	CHECK(result == SIPPYMODEM_OK || result == SIPPYMODEM_AGAIN, "wait failed: %d", result);
	return true;
}

static bool test_idle_rtp_wait(void)
{
	struct sippymodem_server_config config;
	sippymodem_server_config_init(&config);
	config.max_lines = 1;
	config.omit_default_endpoint = true;
	config.bound_address = config.public_address = "127.0.0.1";
	struct sippymodem_server *server = sippymodem_server_create(&config);
	CHECK(server != NULL && sippymodem_server_start(server) == SIPPYMODEM_OK,
	      "idle server startup failed");
	struct sippymodem_line_config line_config;
	sippymodem_line_config_init(&line_config);
	struct sippymodem_line *line = sippymodem_line_create(server, &line_config);
	CHECK(line != NULL && sippymodem_line_start(line) == SIPPYMODEM_OK,
	      "idle line startup failed");
	struct sippymodem_status status;
	CHECK(sippymodem_get_status(line, &status) == SIPPYMODEM_OK && status.rtp_port != 0,
	      "idle line has no RTP port");
	net_socket sender = net_udp_open();
	CHECK(sender != NET_INVALID_SOCKET, "UDP sender startup failed");
	struct sockaddr_in target = { .sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK), .sin_port = htons(status.rtp_port) };
	uint64_t now = now_ns(), deadline;
	CHECK(now != 0 && sippymodem_process(server, now, &deadline) == SIPPYMODEM_OK,
	      "idle process failed");
	struct sippymodem_event event;
	while (sippymodem_poll_event(server, &event) == SIPPYMODEM_OK) {}
	CHECK(deadline > now + UINT64_C(2000000) &&
	      sippymodem_wait(server, now, 2) == SIPPYMODEM_AGAIN,
	      "empty idle wait did not sleep");
	/* Hold the protocol clock fixed: a media deadline must not clear a stale
	 * read notification and conceal a spinning owner loop. */
	for (unsigned int burst = 0; burst < 3; ++burst) {
		const unsigned char packet[] = { 1, 2, 3 };
		for (unsigned int i = 0; i < 2; ++i)
			CHECK(net_socket_send(sender, packet, sizeof(packet), &target) == sizeof(packet),
			      "UDP injection failed");
		enum sippymodem_result result;
		unsigned int passes = 0;
		do {
			result = sippymodem_wait(server, now, 2);
		} while (result == SIPPYMODEM_AGAIN && ++passes < 8);
		CHECK(result == SIPPYMODEM_OK && sippymodem_wait(server, now, 0) == SIPPYMODEM_OK,
		      "wait did not preserve RTP readiness");
		passes = 0;
		do {
			CHECK(sippymodem_process(server, now, &deadline) == SIPPYMODEM_OK &&
			      deadline > now + UINT64_C(2000000), "idle process reached a deadline");
			while (sippymodem_poll_event(server, &event) == SIPPYMODEM_OK) {}
			result = sippymodem_wait(server, now, 2);
		} while (result == SIPPYMODEM_OK && ++passes < 8);
		CHECK(result == SIPPYMODEM_AGAIN, "idle RTP notification kept the owner spinning");
	}
	net_socket_close(sender);
	sippymodem_server_destroy(server);
	return true;
}

static bool test_portable_owner_loop(void)
{
	struct sippymodem_server_config config;
	sippymodem_server_config_init(&config);
	config.max_lines = 2;
	config.bound_address = config.public_address = "127.0.0.1";
	config.identity_uri = "sip:100@127.0.0.1";
	config.username = "100";
	config.password = "unused-loopback-password";
	config.domain = "127.0.0.1";
	struct sippymodem_server *server = sippymodem_server_create(&config);
	CHECK(server != NULL, "server create failed");
	CHECK(sippymodem_wait(NULL, 0, 0) == SIPPYMODEM_INVALID &&
	      sippymodem_wait(server, 0, 0) == SIPPYMODEM_CLOSED, "invalid/closed wait");
	CHECK(sippymodem_server_start(server) == SIPPYMODEM_OK, "server start failed");
	CHECK(sippymodem_wait(server, now_ns(), 0) == SIPPYMODEM_INVALID,
	      "wait accepted an unestablished clock");
	uint64_t now = now_ns(), deadline;
	CHECK(now != 0 && sippymodem_process(server, now, &deadline) == SIPPYMODEM_OK,
	      "initial process failed");
	CHECK(sippymodem_wait(server, now - 1, 0) == SIPPYMODEM_INVALID &&
	      sippymodem_wait(server, now, 0) == SIPPYMODEM_AGAIN &&
	      sippymodem_wait(server, deadline, 50) == SIPPYMODEM_OK,
	      "wait timeout/deadline semantics differ");
	/* An early timeout does not run SIP or consume an event. */
	CHECK(sippymodem_wait(server, now, 1) == SIPPYMODEM_AGAIN,
	      "empty short wait reported readiness");
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
	CHECK(caller != NULL && answerer != NULL &&
	      sippymodem_line_start(caller) == SIPPYMODEM_OK &&
	      sippymodem_line_start(answerer) == SIPPYMODEM_OK, "line startup failed");
	CHECK(tick(server), "initial tick failed");
	char target[128];
	snprintf(target, sizeof(target), "sip:100@127.0.0.1:%u", sippymodem_listen_port(server));
	uint64_t generation;
	CHECK(sippymodem_dial(caller, target, &generation) == SIPPYMODEM_OK, "dial failed");
	struct sippymodem_status local = { 0 }, remote = { 0 };
	bool answered = false, saw_queued = false;
	uint64_t stop = now_ns() + UINT64_C(15000000000);
	while (now_ns() < stop && (!local.ready || !remote.ready)) {
		CHECK(sippymodem_process(server, now_ns(), &deadline) == SIPPYMODEM_OK,
		      "training process failed");
		/* Observe readiness before draining: the wait must preserve INCOMING. */
		enum sippymodem_result observed = sippymodem_wait(server, now_ns(), 0);
		CHECK(observed == SIPPYMODEM_OK || observed == SIPPYMODEM_AGAIN, "zero wait failed");
		struct sippymodem_event event;
		while (sippymodem_poll_event(server, &event) == SIPPYMODEM_OK) {
			CHECK(observed == SIPPYMODEM_OK, "queued event did not wake the owner");
			if (event.type == SIPPYMODEM_EVENT_INCOMING) {
				/* The portable wait preserved the incoming event. */
				saw_queued = true;
			}
		}
		CHECK(sippymodem_get_status(caller, &local) == SIPPYMODEM_OK &&
		      sippymodem_get_status(answerer, &remote) == SIPPYMODEM_OK, "status failed");
		if (!answered && remote.phase == SIPPYMODEM_RINGING) {
			CHECK(sippymodem_answer(answerer, remote.generation) == SIPPYMODEM_OK, "answer failed");
			answered = true;
			continue; /* Service the changed control state before waiting. */
		}
		enum sippymodem_result result = sippymodem_wait(server, now_ns(), 50);
		CHECK(result == SIPPYMODEM_OK || result == SIPPYMODEM_AGAIN, "training wait failed");
	}
	CHECK(saw_queued && local.ready && remote.ready, "portable loop did not connect");
	const unsigned char payload[] = { 0, 'A', 'T', '+', '+', '+', '\r', '\n', 255, 128, 42 };
	unsigned char received[sizeof(payload)];
	size_t sent = 0, count = 0;
	stop = now_ns() + UINT64_C(5000000000);
	while (now_ns() < stop && count < sizeof(payload)) {
		size_t amount;
		enum sippymodem_result result = sippymodem_send(answerer, payload + sent,
			sizeof(payload) - sent, &amount);
		CHECK(result == SIPPYMODEM_OK || result == SIPPYMODEM_AGAIN, "send failed");
		sent += amount;
		result = sippymodem_receive(caller, received + count, sizeof(received) - count, &amount);
		CHECK(result == SIPPYMODEM_OK || result == SIPPYMODEM_AGAIN, "receive failed");
		count += amount;
		CHECK(tick(server), "data tick failed");
	}
	CHECK(count == sizeof(payload) && !memcmp(received, payload, sizeof(payload)),
	      "portable byte I/O lost literal data: %zu", count);
	CHECK(sippymodem_server_stop(server) == SIPPYMODEM_OK &&
	      sippymodem_wait(server, now_ns(), 0) == SIPPYMODEM_CLOSED, "stop/wait failed");
	sippymodem_server_destroy(server);
	return true;
}

#ifdef _WIN32
static bool test_wait_handle_cleanup(void)
{
	struct sippymodem_server_config config;
	sippymodem_server_config_init(&config);
	config.max_lines = 1;
	config.omit_default_endpoint = true;
	DWORD baseline = 0;
	for (unsigned int pass = 0; pass < 16; ++pass) {
		struct sippymodem_server *server = sippymodem_server_create(&config);
		CHECK(server != NULL && sippymodem_server_start(server) == SIPPYMODEM_OK,
		      "cleanup fixture startup failed");
		uint64_t now = now_ns(), deadline;
		CHECK(sippymodem_process(server, now, &deadline) == SIPPYMODEM_OK &&
		      sippymodem_wait(server, now, 1) == SIPPYMODEM_AGAIN,
		      "empty timer wait failed");
		sippymodem_server_destroy(server);
		DWORD handles;
		CHECK(GetProcessHandleCount(GetCurrentProcess(), &handles), "handle count failed");
		/* Allow one-time SDK/CRT initialization before checking owned handles. */
		if (pass == 3)
			baseline = handles;
		else if (pass > 3)
			CHECK(handles == baseline, "wait lifecycle leaked handles: %lu -> %lu",
			      (unsigned long)baseline, (unsigned long)handles);
	}
	return true;
}
#endif

int main(void)
{
	if (!test_idle_rtp_wait() || !test_portable_owner_loop())
		return 1;
#ifdef _WIN32
	if (!test_wait_handle_cleanup())
		return 1;
#endif
	return 0;
}
