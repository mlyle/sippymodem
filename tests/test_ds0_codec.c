// SPDX-License-Identifier: BSD-3-Clause
#include "ds0/ds0_codec.h"
#include "hdlc_fcs.h"
#include "modem.h"
#include "test.h"
#include <string.h>

struct capture { unsigned count; size_t length; enum ds0_rate rate; uint8_t bytes[2048]; };
static void received(void *context, const uint8_t *bytes, size_t length, enum ds0_rate rate)
{
	struct capture *c = context;
	c->count++; c->length = length; c->rate = rate;
	if (length <= sizeof(c->bytes)) memcpy(c->bytes, bytes, length);
}

static bool fcs_vectors(void)
{
	CHECK(hdlc_fcs16((const uint8_t *)"123456789", 9) == 0x6f91, "FCS check vector");
	for (unsigned initial = 0; initial < 65536; initial += 257) {
		for (unsigned byte = 0; byte < 256; ++byte) {
			uint16_t expected = initial ^ byte;
			for (unsigned bit = 0; bit < 8; ++bit)
				expected = (expected >> 1) ^ ((expected & 1) ? 0x8408 : 0);
			CHECK(hdlc_fcs16_update(initial, byte) == expected, "FCS recurrence");
		}
	}
	return true;
}

struct oracle { struct hdlc_frame frame; bool served; const uint8_t *bytes; size_t length; };
static struct hdlc_frame *next(void *context)
{
	struct oracle *o = context;
	if (o->served) return NULL;
	o->served = true;
	o->frame = (struct hdlc_frame) { .buf = (uint8_t *)o->bytes, .size = (int)o->length };
	return &o->frame;
}
static void completed(void *context, struct hdlc_frame *frame) { (void)context; (void)frame; }

static bool both_rates(void)
{
	uint8_t payload[193], wire[512], idle[16];
	for (size_t i = 0; i < sizeof(payload); ++i) payload[i] = (uint8_t)(i * 29u);
	payload[0] = 0xff; payload[1] = 0x03; payload[2] = 0x7e; payload[3] = 0x7d;
	for (unsigned bits = 7; bits <= 8; ++bits) {
		struct ds0_config cfg; ds0_config_default(&cfg);
		cfg.rate_policy = bits == 7 ? DS0_RATE_FORCE_56000 : DS0_RATE_FORCE_64000;
		struct ds0 tx, rx;
		CHECK(ds0_init(&tx, &cfg) == 0 && ds0_init(&rx, NULL) == 0, "codec init");
		size_t n = ds0_tx_frame(&tx, payload, sizeof(payload), wire, sizeof(wire));
		CHECK(n > 0 && ds0_tx_frame(&tx, payload, sizeof(payload), idle, 1) == 0, "bounded TX");
		struct capture c = { 0 };
		for (size_t i = 0; i < n; ++i) {
			/* One robbed LSB in each six samples cannot corrupt 56k payload. */
			if (bits == 7 && i % 6 == 2) wire[i] ^= 1;
			CHECK(ds0_rx_codec(&rx, wire + i, 1, received, &c) >= 0, "fragment RX");
		}
		CHECK(c.count == 1 && c.length == sizeof(payload) &&
		      memcmp(c.bytes, payload, sizeof(payload)) == 0, "round trip");
		CHECK(c.rate == (bits == 7 ? DS0_RATE_56000 : DS0_RATE_64000) &&
		      rx.tx_rate == c.rate && rx.saw_valid_frame, "automatic selection");

		/* Decode the existing LAPM HDLC encoder independently of DS0 TX. */
		struct modem m = { 0 }; struct oracle o = { .bytes = payload, .length = sizeof(payload) };
		modem_hdlc_start(&m); m.packer.hdlc.framer = &o;
		m.packer.hdlc.get_tx_frame = next; m.packer.hdlc.tx_complete = completed;
		CHECK(ds0_init(&rx, NULL) == 0, "oracle RX init"); c = (struct capture){ 0 };
		for (unsigned block = 0; block < 32 && !c.count; ++block) {
			CHECK(modem_hdlc_get_bits(&m, bits, wire, 16) == 16, "HDLC encoder");
			for (unsigned i = 0; i < 16; ++i) if (bits == 7) wire[i] <<= 1;
			CHECK(ds0_rx_codec(&rx, wire, 16, received, &c) >= 0, "HDLC oracle RX");
		}
		CHECK(c.count == 1 && c.length == sizeof(payload) &&
		      memcmp(c.bytes, payload, sizeof(payload)) == 0, "normal HDLC interop bits=%u count=%u length=%zu rate=%d fcs56=%u fcs64=%u invalid64=%u", bits, c.count, c.length, c.rate, rx.counters.rx56.fcs_fail, rx.counters.rx64.fcs_fail, rx.counters.rx64.invalid);
	}
	return true;
}

static bool recovery_and_bounds(void)
{
	const uint8_t payload[] = { 0xff, 3, 0xc0, 0x21, 1, 2, 3, 4, 5, 6 };
	uint8_t wire[128]; struct ds0 tx, rx; struct capture c = { 0 };
	CHECK(ds0_init(&tx, NULL) == 0 && ds0_init(&rx, NULL) == 0, "init");
	size_t n = ds0_tx_frame(&tx, payload, sizeof(payload), wire, sizeof(wire));
	CHECK(ds0_rx_codec(&rx, wire, n / 2, received, &c) == 0, "partial frame");
	ds0_rx_gap(&rx);
	CHECK(ds0_rx_codec(&rx, wire + n / 2, n - n / 2, received, &c) == 0, "gap abort");
	CHECK(ds0_rx_codec(&rx, wire, n, received, &c) == 1 && c.count == 1, "gap recovery");
	wire[n / 2] ^= 0x20;
	CHECK(ds0_rx_codec(&rx, wire, n, received, &c) == 0 && rx.counters.rx64.fcs_fail > 0, "FCS rejection");
	struct ds0_config cfg; ds0_config_default(&cfg); cfg.rx_frame_limit = 4;
	CHECK(ds0_init(&rx, &cfg) == 0, "bounded RX init"); wire[n / 2] ^= 0x20;
	CHECK(ds0_rx_codec(&rx, wire, n, received, &c) == 0 && rx.counters.rx64.overflow > 0, "RX bound");
	CHECK(ds0_tx_frame(&tx, payload, DS0_MAX_FRAME_OCTETS + 1, wire, sizeof(wire)) == 0, "TX bound");
	cfg.rx_frame_limit = 0; cfg.answerer = true;
	CHECK(ds0_init(&rx, &cfg) == 0, "auto answer init");
	ds0_advance_samples(&rx, DS0_ANSWER_TIMEOUT_SAMPLES - 1);
	CHECK(rx.tx_rate == DS0_RATE_64000, "fallback premature");
	ds0_advance_samples(&rx, 1);
	CHECK(rx.tx_rate == DS0_RATE_56000 && rx.counters.answerer_timeout_fallbacks == 1, "fallback");
	CHECK(ds0_rx_codec(&rx, wire, n, received, &c) == 1 && rx.tx_rate == DS0_RATE_64000, "64k recovery");
	cfg.rate_policy = DS0_RATE_FORCE_56000;
	CHECK(ds0_init(&rx, &cfg) == 0 && ds0_rx_codec(&rx, wire, n, received, &c) == 0, "forced restriction");
	cfg.rate_policy = 99; CHECK(ds0_init(&rx, &cfg) == -1, "invalid policy");
	return true;
}

int main(void)
{
	if (!fcs_vectors() || !both_rates() || !recovery_and_bounds()) return 1;
	puts("DS0 codec: both rates, HDLC interop, FCS, bounds and recovery passed");
	return 0;
}
