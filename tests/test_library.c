// SPDX-License-Identifier: BSD-3-Clause
#define _DEFAULT_SOURCE
#include <sippymodem.h>
#include <sippymodem_fd.h>
#include "test.h"

#include <stdio.h>
#include <poll.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <unistd.h>

struct modem;
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

static bool test_owned_loopback(void)
{
	struct sippymodem_server_config config;
	sippymodem_server_config_init(&config);
	config.max_lines = 4;
	config.public_address = "127.0.0.1";
	config.bound_address = "127.0.0.1";
	config.identity_uri = "sip:+12025550123@127.0.0.1";
	config.username = "+12025550123";
	config.password = "unused-loopback-password";
	config.domain = "127.0.0.1";
	struct sippymodem_server *server = sippymodem_server_create(&config);
	EXPECT(server != NULL);
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
	EXPECT(strcmp(facts.caller_number, "+12025550123") == 0);
	EXPECT(sippymodem_get_call_facts(caller, &facts) == SIPPYMODEM_OK && !facts.caller_number[0]);
	EXPECT(sippymodem_answer(answerer, remote.generation) == SIPPYMODEM_OK);
	for (unsigned int index = 0; index < 15000 && (!remote.ready || !local.ready); ++index) {
		EXPECT(step(server, &now));
		EXPECT(sippymodem_get_status(answerer, &remote) == SIPPYMODEM_OK);
		EXPECT(sippymodem_get_status(caller, &local) == SIPPYMODEM_OK);
	}
	EXPECT(remote.ready && local.ready);
	EXPECT(remote.modulation == 21 && local.modulation == 21);
	EXPECT(remote.tx_bps == 300 && remote.rx_bps == 300);
	const unsigned char literal[] = { 0, '+', '+', '+', '\r', 'A', 'T', 0x11, 0x13, 255 };
	size_t accepted = 0, received = 0;
	EXPECT(sippymodem_send(caller, literal, sizeof(literal), &accepted) == SIPPYMODEM_OK);
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
	EXPECT(sippymodem_send(answerer, literal, sizeof(literal), &accepted) == SIPPYMODEM_OK);
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
	const unsigned char first_payload[] = { 1, 3, 5, 7 };
	const unsigned char second_payload[] = { 2, 4, 6, 8 };
	EXPECT(sippymodem_send(caller, first_payload, sizeof(first_payload), &accepted) == SIPPYMODEM_OK);
	EXPECT(sippymodem_send(other_caller, second_payload, sizeof(second_payload), &accepted) == SIPPYMODEM_OK);
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
	EXPECT(sippymodem_send(answerer, literal, sizeof(literal), &accepted) == SIPPYMODEM_OK && accepted == sizeof(literal));
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
	EXPECT(sippymodem_send(other_answerer, literal, sizeof(literal), &accepted) == SIPPYMODEM_OK);
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
	sippymodem_server_destroy(server);
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
	EXPECT(sippymodem_send(answerer, payload, 8, &accepted) == SIPPYMODEM_OK && accepted == 8);
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
		EXPECT(sippymodem_send(answerer, payload, 8, &accepted) == SIPPYMODEM_OK && accepted == 8);
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

static bool test_timeout_and_bounds(void)
{
	struct sippymodem_server_config config;
	sippymodem_server_config_init(&config);
	config.max_lines = 2;
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

int main(void)
{
	return test_typed_credentials() && test_typed_accounts() && test_owned_loopback() && test_compressed_command_and_fd_eof() && test_timeout_and_bounds() ? 0 : 1;
}
