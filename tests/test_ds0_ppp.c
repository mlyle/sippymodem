// SPDX-License-Identifier: BSD-3-Clause
#include "ds0/ds0_ppp.h"
#include "hdlc_fcs.h"
#include "test.h"
#include <string.h>

static size_t async_frame(const uint8_t *body, size_t n, uint8_t *out)
{
	uint16_t fcs = hdlc_fcs16(body, n) ^ 0xffffu;
	size_t count = 0; out[count++] = 0x7e;
	for (size_t i = 0; i < n + 2; ++i) {
		uint8_t b = i < n ? body[i] : (i == n ? fcs & 0xff : fcs >> 8);
		if (b < 0x20 || b == 0x7e || b == 0x7d) {
			out[count++] = 0x7d; out[count++] = b ^ 0x20;
		} else out[count++] = b;
	}
	out[count++] = 0x7e; return count;
}

static bool partial_frames(void)
{
	uint8_t body[256], async[1024], out[1024];
	for (unsigned i = 0; i < sizeof(body); ++i) body[i] = i;
	size_t n = async_frame(body, sizeof(body), async);
	for (unsigned rate = 0; rate < 2; ++rate) {
		struct ds0_config cfg; ds0_config_default(&cfg);
		cfg.rate_policy = rate ? DS0_RATE_FORCE_64000 : DS0_RATE_FORCE_56000;
		struct ds0_ppp tx, rx; CHECK(ds0_ppp_init(&tx, &cfg) == 0 && ds0_ppp_init(&rx, NULL) == 0, "init");
		size_t sent = 0, collected = 0;
		for (unsigned tick = 0; tick < 4096 && collected < n; ++tick) {
			uint8_t code; size_t consumed = 99;
			size_t amount = sent < n ? 1 : 0;
			CHECK(ds0_ppp_transmit(&tx, async + sent, amount, &code, 1, &consumed) == 1 && consumed <= amount, "exact prefix");
			sent += consumed;
			CHECK(ds0_rx_codec(&rx.codec, &code, 1, ds0_ppp_receive_frame, &rx) >= 0, "receive");
			/* Each receive read owns only its reported prefix. */
			collected += ds0_ppp_read(&rx, out + collected, collected < n ? 1 : 0);
			CHECK(!rx.overflow, "unexpected overflow");
		}
		CHECK(sent == n && collected == n && memcmp(async, out, n) == 0, "byte-exact PPP round trip");
		CHECK(tx.counters.async_tx_valid_frames == 1 && rx.counters.async_rx_frames == 1, "frame counts");
		CHECK(ds0_ppp_read(&rx, out, sizeof(out)) == 0, "no duplicated tail");
	}
	return true;
}

static bool invalid_and_overflow(void)
{
	struct ds0_ppp p; CHECK(ds0_ppp_init(&p, NULL) == 0, "init");
	const uint8_t invalid[] = {0x7e, 0xff, 3, 0, 0, 0x7e, 0x7d, 0x7e};
	uint8_t wire[32]; size_t used;
	CHECK(ds0_ppp_transmit(&p, invalid, sizeof(invalid), wire, sizeof(wire), &used) == sizeof(wire) && used == sizeof(invalid), "invalid consume");
	CHECK(p.counters.async_tx_fcs_fail == 1 && p.counters.async_tx_invalid_escape == 1 && p.counters.async_tx_valid_frames == 0, "invalid rejected");
	uint8_t body[] = {0xff, 3, 0xc0, 0x21};
	ds0_ppp_receive_frame(&p, body, sizeof(body), DS0_RATE_64000);
	size_t retained = p.rx_async_pending_len;
	ds0_ppp_receive_frame(&p, body, sizeof(body), DS0_RATE_64000);
	CHECK(p.overflow && p.counters.async_rx_overflow == 1 && p.rx_async_pending_len == retained, "explicit RX overflow preserves prefix");
	uint8_t expected[32], output[32]; size_t n = async_frame(body, sizeof(body), expected);
	CHECK(ds0_ppp_read(&p, output, 1) == 1 && ds0_ppp_read(&p, output + 1, sizeof(output) - 1) == n - 1 && memcmp(expected, output, n) == 0, "retained output");
	used = 99; CHECK(ds0_ppp_transmit(&p, invalid, sizeof(invalid), NULL, 0, &used) == 0 && used == 0, "zero-capacity input retained by caller");
	return true;
}

static bool short_frames(void)
{
	const uint8_t body[] = {0x21, 0};
	uint8_t async[32], wire[32]; size_t used;
	for (unsigned rate = 0; rate < 2; ++rate) {
		struct ds0_config cfg; ds0_config_default(&cfg);
		cfg.rate_policy = rate ? DS0_RATE_FORCE_64000 : DS0_RATE_FORCE_56000;
		struct ds0_ppp p; CHECK(ds0_ppp_init(&p, &cfg) == 0, "init");
		for (size_t length = 0; length < 2; ++length) {
			size_t n = async_frame(body, length, async);
			CHECK(ds0_ppp_encode(&p, async, n, wire, sizeof(wire), &used) == 0 && used == n,
			      "short frame consumed without transmission");
		}
		CHECK(p.counters.async_tx_short == 2 && !p.counters.async_tx_fcs_fail &&
		      !p.counters.async_tx_valid_frames && !p.overflow, "short frames classified before FCS");
		size_t n = async_frame(body, sizeof(body), async);
		CHECK(ds0_ppp_encode(&p, async, n, wire, sizeof(wire), &used) > 0 && used == n &&
		      p.counters.async_tx_valid_frames == 1, "minimum frame accepted after rejection");
		ds0_ppp_receive_frame(&p, body, sizeof(body), DS0_RATE_64000);
		size_t pending = p.rx_async_pending_len;
		ds0_ppp_receive_frame(&p, body, 1, DS0_RATE_64000);
		CHECK(p.rx_async_pending_len == pending && !p.overflow && p.counters.async_rx_frames == 1,
		      "short received frame preserves queued valid bytes");
	}
	return true;
}

static bool assembly_without_idle(void)
{
	struct ds0_ppp p; CHECK(ds0_ppp_init(&p, NULL) == 0, "init");
	uint8_t body[1024], async[2200], out[2048]; memset(body, 0x55, sizeof(body));
	size_t n = async_frame(body, sizeof(body), async), used;
	CHECK(ds0_ppp_encode(&p, async, 512, out, sizeof(out), &used) == 0 && used == 512,
	      "partial assembly inserts no idle");
	CHECK(ds0_ppp_encode(&p, async + 512, n - 512, out, sizeof(out), &used) > 0 &&
	      used == n - 512 && p.counters.async_tx_valid_frames == 1,
	      "refill completes same packet");
	return true;
}
static bool oversized_frame(void)
{
	struct ds0_ppp p; CHECK(ds0_ppp_init(&p, NULL) == 0, "init");
	uint8_t body[DS0_MAX_FRAME_OCTETS + 1], async[DS0_MAX_ASYNC_FRAME_OCTETS + 4], wire[4096];
	memset(body, 0x55, sizeof(body));
	size_t n = async_frame(body, sizeof(body), async), used;
	CHECK(ds0_ppp_transmit(&p, async, n, wire, sizeof(wire), &used) == sizeof(wire) && used <= n,
	      "bounded oversize input");
	CHECK(p.overflow && p.counters.async_tx_overflow == 1 && !p.counters.async_tx_valid_frames,
	      "TX overflow is explicit");
	CHECK(ds0_ppp_transmit(&p, async + used, n - used, wire, sizeof(wire), &used) == sizeof(wire) && !used,
	      "fatal overflow retains caller suffix");
	return true;
}
static bool opening_flag_and_rate_change(void)
{
	uint8_t body[128], async[512], wire[512], out[512];
	memset(body, 0x55, sizeof(body)); body[0] = 0xff; body[1] = 3;
	size_t n = async_frame(body, sizeof(body), async), used;
	struct ds0_ppp tx, rx;
	CHECK(ds0_ppp_init(&tx, NULL) == 0 && ds0_ppp_init(&rx, NULL) == 0, "init");
	/* A CRC-valid byte sequence without its opening flag is not a frame. */
	CHECK(ds0_ppp_transmit(&tx, async + 1, n - 1, wire, sizeof(wire), &used) == sizeof(wire) &&
	      used == n - 1 && tx.counters.async_tx_valid_frames == 0, "opening flag required");
	CHECK(ds0_ppp_init(&tx, NULL) == 0, "fresh sender");
	CHECK(ds0_ppp_transmit(&tx, async, n, wire, 1, &used) == 1 && used == n &&
	      tx.tx_codec_pending_len > 1, "queued partial frame");
	tx.codec.tx_rate = DS0_RATE_56000;
	CHECK(ds0_ppp_transmit(&tx, NULL, 0, wire, sizeof(wire), &used) == sizeof(wire) &&
	      used == 0 && tx.counters.tx_reencoded == 1, "rate choice reencodes owned payload");
	CHECK(ds0_rx_codec(&rx.codec, wire, sizeof(wire), ds0_ppp_receive_frame, &rx) == 1, "new-rate frame");
	CHECK(ds0_ppp_read(&rx, out, sizeof(out)) == n && !memcmp(async, out, n), "queued payload preserved");
	return true;
}
int main(void)
{
	if (!partial_frames() || !invalid_and_overflow() || !short_frames() || !oversized_frame() || !assembly_without_idle() || !opening_flag_and_rate_change()) return 1;
	puts("DS0 PPP: partial byte I/O, both rates, escapes, FCS and explicit overflow passed");
	return 0;
}
