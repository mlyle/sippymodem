// SPDX-License-Identifier: BSD-3-Clause
#define _DEFAULT_SOURCE
#include "sippymodem.h"
#include "hdlc_fcs.h"
#include "test.h"
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void)
{
	struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000u + t.tv_nsec;
}
static bool process(struct sippymodem_server *server)
{
	uint64_t next;
	CHECK(sippymodem_process(server, now_ns(), &next) == SIPPYMODEM_OK, "native process");
	struct sippymodem_event e;
	while (sippymodem_poll_event(server, &e) == SIPPYMODEM_OK) {
		if (e.type == SIPPYMODEM_EVENT_CONNECTED)
			CHECK(e.status.ready && e.status.carrier && e.status.modulation == 64, "readiness event");
	}
	usleep(100); return true;
}
static bool receive(struct sippymodem_line *line, uint8_t *bytes, size_t capacity, size_t *count)
{
    enum sippymodem_result result = sippymodem_receive(line, bytes, capacity, count);
    CHECK(result == SIPPYMODEM_OK || result == SIPPYMODEM_AGAIN, "receive status=%d", result);
    return true;
}
static size_t ppp_frame(uint8_t *out)
{
	const uint8_t body[] = { 0xff, 3, 0xc0, 0x21, 1, 2, 0, 4, '+', '+', '+', 0x7e, 0x7d, 0x11, 0x13 };
	uint16_t fcs = hdlc_fcs16(body, sizeof(body)) ^ 0xffffu;
	size_t n = 0; out[n++] = 0x7e;
	for (size_t i = 0; i < sizeof(body) + 2; ++i) {
		uint8_t b = i < sizeof(body) ? body[i] : i == sizeof(body) ? fcs & 0xff : fcs >> 8;
		if (b < 0x20 || b == 0x7e || b == 0x7d) { out[n++] = 0x7d; out[n++] = b ^ 0x20; }
		else out[n++] = b;
	}
	out[n++] = 0x7e; return n;
}
static bool pair(bool lapm, unsigned rate, bool alaw)
{
	struct sippymodem_server_config config; sippymodem_server_config_init(&config);
	config.max_lines = 2; config.bound_address = config.public_address = "127.0.0.1";
	struct sippymodem_server *server = sippymodem_server_create(&config);
	CHECK(server != NULL, "server create");
	struct sippymodem_line_config cfg; sippymodem_line_config_init(&cfg);
	cfg.modulation = 64; cfg.min_bps = cfg.max_bps = rate;
	cfg.ds0_lapm = lapm; cfg.alaw = alaw;
	/* PPP per-call framing also overrides a stored REQUIRED policy. */
	cfg.ec_policy = SIPPYMODEM_EC_REQUIRED;
	cfg.compression_types = 0; cfg.rx_capacity = 256;
	struct sippymodem_line *a = sippymodem_line_create(server, &cfg), *b = sippymodem_line_create(server, &cfg);
	CHECK(a && b && sippymodem_server_start(server) == SIPPYMODEM_OK &&
	      sippymodem_line_start(a) == SIPPYMODEM_OK && sippymodem_line_start(b) == SIPPYMODEM_OK, "start");
	CHECK(process(server), "owner initial clock");
	char target[128]; snprintf(target, sizeof(target), "sip:1234@127.0.0.1:%u", sippymodem_listen_port(server));
	uint64_t generation;
	CHECK(sippymodem_dial(a, target, &generation) == SIPPYMODEM_OK, "dial");
	struct sippymodem_status sa = {0}, sb = {0};
	uint64_t limit = now_ns() + 3000000000ull;
	while (now_ns() < limit && sb.phase != SIPPYMODEM_RINGING) {
		CHECK(process(server), "ring tick"); sippymodem_get_status(b, &sb);
	}
	CHECK(sb.phase == SIPPYMODEM_RINGING && sippymodem_answer(b, sb.generation) == SIPPYMODEM_OK, "answer");
	uint8_t payload[64]; size_t n;
	if (lapm) { const uint8_t bytes[] = {0, '+','+','+','A','T','\r',0x11,0x13,0xff}; n = sizeof(bytes); memcpy(payload, bytes, n); }
	else n = ppp_frame(payload);
	bool sent_b = false; limit = now_ns() + 8000000000ull;
	while (now_ns() < limit && (!sa.ready || !sb.ready)) {
		CHECK(process(server), "training tick");
		sippymodem_get_status(a, &sa); sippymodem_get_status(b, &sb);
		if (!lapm && sb.ready && !sent_b) {
			size_t accepted;
			CHECK(sippymodem_send(b, payload, n, &accepted) == SIPPYMODEM_OK && accepted == n, "PPP bootstrap"); sent_b = true;
		}
	}
	CHECK(sa.ready && sb.ready && sa.modulation == 64 && sb.modulation == 64 && sa.tx_bps == rate && sa.rx_bps == rate, "native DS0 ready lapm=%d rate=%u alaw=%d phases=%d/%d", lapm,rate,alaw,sa.phase,sb.phase);
	CHECK(sa.tx_baud == 8000 && sa.rx_baud == 8000 && sa.error_control == (lapm ? SIPPYMODEM_LAPM : SIPPYMODEM_RAW) && sa.compression == SIPPYMODEM_COMP_NONE, "native status");
	size_t accepted;
	CHECK(sippymodem_send(a, payload, n, &accepted) == SIPPYMODEM_OK && accepted == n, "send A");
	if (lapm) CHECK(sippymodem_send(b, payload, n, &accepted) == SIPPYMODEM_OK && accepted == n, "send B");
	uint8_t a_rx[64], b_rx[64]; size_t a_n = 0, b_n = 0; limit = now_ns() + 4000000000ull;
	while (now_ns() < limit && (a_n < n || b_n < n)) {
		CHECK(process(server), "data tick"); size_t count;
		CHECK(receive(a, a_rx + a_n, a_n < n ? 1 : 0, &count), "read A"); a_n += count;
		CHECK(receive(b, b_rx + b_n, b_n < n ? 1 : 0, &count), "read B"); b_n += count;
	}
	CHECK(a_n == n && b_n == n && !memcmp(payload,a_rx,n) && !memcmp(payload,b_rx,n), "exact literal byte payload");
	CHECK(sippymodem_set_dte_mode(a, generation, SIPPYMODEM_COMMAND) == SIPPYMODEM_OK, "command mode");
	CHECK(sippymodem_send(b, payload, n, &accepted) == SIPPYMODEM_OK && accepted == n, "send to command peer");
	a_n = 0; limit = now_ns() + 4000000000ull;
	while (now_ns() < limit && a_n < n) {
		CHECK(process(server), "command RX tick"); size_t count;
		CHECK(receive(a, a_rx + a_n, 1, &count), "command receive"); a_n += count;
	}
	CHECK(a_n == n && !memcmp(payload,a_rx,n), "command RX preserved");
	CHECK(sippymodem_set_dte_mode(a, generation, SIPPYMODEM_DATA) == SIPPYMODEM_OK &&
	      sippymodem_hangup(a, generation) == SIPPYMODEM_OK && sippymodem_server_stop(server) == SIPPYMODEM_OK, "stop");
	sippymodem_server_destroy(server); return true;
}
int main(void)
{
	for (unsigned lapm = 0; lapm < 2; ++lapm)
		for (unsigned rate = 56000; rate <= 64000; rate += 8000)
			for (unsigned alaw = 0; alaw < 2; ++alaw)
				if (!pair(lapm, rate, alaw)) return 1;
	puts("DS0 SIP: PPP/LAPM at56/64, both laws, literal byte queues and command preservation passed");
	return 0;
}
