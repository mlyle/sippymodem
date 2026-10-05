// SPDX-License-Identifier: BSD-3-Clause
#include "ds0/ds0_probe.h"
#include "test.h"
#include <string.h>

static bool candidate(enum ds0_rate rate, bool robbed)
{
	struct ds0 tx; struct ds0_config cfg; ds0_config_default(&cfg);
	cfg.rate_policy = rate == DS0_RATE_56000 ? DS0_RATE_FORCE_56000 : DS0_RATE_FORCE_64000;
	CHECK(ds0_init(&tx, &cfg) == 0, "transmitter");
	struct ds0_probe p; CHECK(ds0_probe_init(&p, true, true) == 0, "probe");
	uint8_t bytes[4096], history[4096];
	const uint8_t body[] = {0xff,3,0xc0,0x21,1,7,0,4,0x7f};
	size_t n = ds0_tx_frame(&tx, body, sizeof(body), bytes, sizeof(bytes));
	CHECK(n > 0, "frame");
	for (size_t i = n; i < 1500; ++i)
		CHECK(ds0_tx_idle(&tx, bytes + i, 1) == 1, "split idle");
	for (size_t i = 0; i < 1500; ++i) {
		if (robbed && i % 6 == 0) bytes[i] ^= 1;
		enum ds0_rate selected = ds0_probe_sample(&p, bytes[i], true);
		if (i < 1199) CHECK(selected == DS0_RATE_NONE, "early selection at%zu", i);
		if (selected != DS0_RATE_NONE) {
			CHECK(selected == rate && i >= 1199 && i < 1220, "rate/threshold at%zu rate=%u", i,selected);
			CHECK(ds0_probe_copy(&p, history, sizeof(history)) >= 1200, "retained prefix");
			return true;
		}
	}
	CHECK(false, "did not recognize rate%u", rate); return false;
}
/* No stuffing is needed in this normative FCS check vector. */
static const uint8_t frame_wire[] = {
	0x7e, 0x8c, 0x4c, 0xcc, 0x2c, 0xac, 0x6c,
	0xec, 0x1c, 0x9c, 0x76, 0x09, 0x7e,
};

static bool mark_idle(enum ds0_rate rate)
{
	unsigned width = rate == DS0_RATE_56000 ? 7 : 8;
	uint8_t bits[13000]; size_t count = 0;
	for (size_t i = 0; i < sizeof(frame_wire); ++i)
		for (int bit = 7; bit >= 0; --bit) bits[count++] = (frame_wire[i] >> bit) & 1u;
	for (unsigned i = 0; i < 1250 * width; ++i) bits[count++] = 1;
	/* Mark idle must be followed by an opening flag, then a frame. */
	for (size_t i = 0; i < sizeof(frame_wire); ++i)
		for (int bit = 7; bit >= 0; --bit) bits[count++] = (frame_wire[i] >> bit) & 1u;
	for (unsigned bit = 0; bit < 24; ++bit)
		bits[count++] = (0x7e >> (7 - bit % 8)) & 1u;
	struct ds0_probe p; CHECK(ds0_probe_init(&p, true, true) == 0, "mark probe");
	uint8_t history[DS0_PROBE_HISTORY];
	bool selected = false;
	for (size_t at = 0; at + width <= count; at += width) {
		uint8_t codeword = 0;
		for (unsigned bit = 0; bit < width; ++bit)
			codeword |= bits[at + bit] << (7 - bit);
		if (width == 7 && (at / width) % 6 == 0) codeword |= 1;
		enum ds0_rate result = ds0_probe_sample(&p, codeword, true);
		if (at / width < 1199) CHECK(result == DS0_RATE_NONE, "early mark selection");
		if (result != DS0_RATE_NONE) {
			CHECK(result == rate && (selected || at / width < 1220), "mark threshold/rate");
			selected = true;
		}
	}
	CHECK(selected, "valid frame plus mark idle not recognized: %u", rate);
	size_t n = ds0_probe_copy(&p, history, sizeof(history));
	struct ds0_config cfg; ds0_config_default(&cfg);
	cfg.rate_policy = width == 7 ? DS0_RATE_FORCE_56000 : DS0_RATE_FORCE_64000;
	struct ds0 rx; CHECK(ds0_init(&rx, &cfg) == 0, "mark replay");
	CHECK(n >= 1200 && ds0_rx_codec(&rx, history, n, NULL, NULL) == 2,
	      "mark handoff lost proving or following frame");
	const struct ds0_rx_counters *counts = width == 7 ? &rx.counters.rx56 : &rx.counters.rx64;
	CHECK(!counts->invalid && !counts->fcs_fail, "legal mark counted as damaged frame");
	return true;
}

static bool mark_boundaries(void)
{
	for (unsigned ones = 7; ones <= 20; ++ones)
	for (unsigned data = 0; data <= 1; ++data) {
		struct hdlc_rx rx; hdlc_rx_reset(&rx);
		uint8_t storage[16]; unsigned events = 0;
		for (unsigned bit = 0; bit < 8; ++bit)
			hdlc_rx_bit(&rx, (0x7e >> (7 - bit)) & 1u, storage, sizeof(storage));
		if (data) for (unsigned bit = 0; bit < 8; ++bit)
			events |= hdlc_rx_bit(&rx, 0, storage, sizeof(storage));
		for (unsigned bit = 0; bit < ones; ++bit)
			events |= hdlc_rx_bit(&rx, 1, storage, sizeof(storage));
		for (unsigned bit = 0; bit < 8; ++bit)
			events |= hdlc_rx_bit(&rx, (0x7e >> (7 - bit)) & 1u, storage, sizeof(storage));
		bool legal = !data && ones >= 15;
		CHECK(!!(events & HDLC_RX_MARK_IDLE) == legal &&
		      !!(events & HDLC_RX_ABORT) == !legal,
		      "mark/abort classification ones=%u data=%u events=%u", ones, data, events);
	}
	return true;
}

static bool rejection(void)
{
	struct ds0_probe p; CHECK(ds0_probe_init(&p,true,true) == 0, "probe");
	for (unsigned i = 0; i < 8000; ++i)
		CHECK(ds0_probe_sample(&p,0x7e,true) == DS0_RATE_NONE, "idle lacks FCS");
	for (unsigned i = 0; i < 8000; ++i)
		CHECK(ds0_probe_sample(&p,0xff,true) == DS0_RATE_NONE, "silence lacks FCS");
	struct ds0 tx; struct ds0_config cfg; ds0_config_default(&cfg);
	CHECK(ds0_init(&tx,&cfg) == 0, "tx");
	uint8_t bytes[4096]; const uint8_t body[] = {0xff,3,0xc0,0x21,1,1,0,4};
	size_t n = ds0_tx_frame(&tx,body,sizeof(body),bytes,sizeof(bytes));
	for(size_t i=n;i<1000;++i) ds0_tx_idle(&tx,bytes+i,1);
	CHECK(ds0_probe_init(&p,true,true)==0,"reset");
	for(size_t i=0;i<1000;++i) CHECK(ds0_probe_sample(&p,bytes[i],true)==DS0_RATE_NONE,"short run");
	CHECK(ds0_probe_sample(&p,0xff,false)==DS0_RATE_NONE,"gap");
	for(size_t i=0;i<1000;++i) CHECK(ds0_probe_sample(&p,bytes[i],true)==DS0_RATE_NONE,"gap reset");
	CHECK(ds0_probe_init(&p,true,true)==0,"reset");
	bytes[5] ^= 0x10; /* Corrupt body, retain framing and flags. */
	for(size_t i=0;i<1000;++i) CHECK(ds0_probe_sample(&p,bytes[i],true)==DS0_RATE_NONE,"bad FCS");
	for(unsigned i=0;i<1000;++i) CHECK(ds0_probe_sample(&p,0x7e,true)==DS0_RATE_NONE,"bad FCS plus idle");
	CHECK(ds0_probe_init(&p,true,false)==0,"56 only");
	bytes[5] ^= 0x10;
	for(size_t i=0;i<1000;++i) CHECK(ds0_probe_sample(&p,bytes[i],true)==DS0_RATE_NONE,"rate not allowed");
	return true;
}
struct capture { const uint8_t *expected; size_t length, frames; bool exact; };
static void received(void *context, const uint8_t *bytes, size_t count, enum ds0_rate rate)
{
	(void)rate;
	struct capture *c = context;
	c->frames++;
	c->exact = count == c->length && !memcmp(bytes, c->expected, count);
}
static bool history_wrap(enum ds0_rate rate)
{
	struct ds0 tx; struct ds0_config cfg; ds0_config_default(&cfg);
	cfg.rate_policy = rate == DS0_RATE_56000 ? DS0_RATE_FORCE_56000 : DS0_RATE_FORCE_64000;
	CHECK(ds0_init(&tx, &cfg) == 0, "transmitter");
	struct ds0_probe p; CHECK(ds0_probe_init(&p, true, true) == 0, "probe");
	uint8_t byte;
	for (unsigned i = 0; i < 5000; ++i) {
		CHECK(ds0_tx_idle(&tx, &byte, 1) == 1, "idle");
		CHECK(ds0_probe_sample(&p, byte, true) == DS0_RATE_NONE, "wrapped idle lacks FCS");
	}
	uint8_t body[DS0_MAX_FRAME_OCTETS], wire[4096], history[4096];
	memset(body, 0xff, sizeof(body));
	size_t n = ds0_tx_frame(&tx, body, sizeof(body), wire, sizeof(wire));
	CHECK(n > 1200 && n < sizeof(wire), "maximum stuffed frame bound");
	for (size_t i = 0; i < n; ++i) {
		enum ds0_rate selected = ds0_probe_sample(&p, wire[i], true);
		if (i + 2 < n) CHECK(selected == DS0_RATE_NONE, "unfinished frame is unverified");
	}
	CHECK(p.selected == rate, "long run qualifies only after complete FCS");
	CHECK(ds0_probe_copy(&p, history, 1199) == 0, "copy capacity is exact");
	/* Preserve the remaining selecting fragment; replay crosses a ring wrap. */
	for (unsigned i = 0; i < 40; ++i) {
		CHECK(ds0_tx_idle(&tx, &byte, 1) == 1, "tail idle");
		CHECK(ds0_probe_sample(&p, byte, true) == rate, "retained tail");
	}
	size_t count = ds0_probe_copy(&p, history, sizeof(history));
	CHECK(count >= n && count <= sizeof(history), "bounded proving history");
	struct ds0 rx; CHECK(ds0_init(&rx, &cfg) == 0, "replay receiver");
	struct capture capture = { .expected = body, .length = sizeof(body) };
	CHECK(ds0_rx_codec(&rx, history, count, received, &capture) == 1 &&
	      capture.frames == 1 && capture.exact, "maximum proving frame survives history replay");
	return true;
}
static bool invalid_resets_proof(bool bad_fcs)
{
	struct ds0 tx; struct ds0_config cfg; ds0_config_default(&cfg);
	CHECK(ds0_init(&tx, &cfg) == 0, "transmitter");
	struct ds0_probe p; CHECK(ds0_probe_init(&p, false, true) == 0, "64 probe");
	const uint8_t body[] = {0xff,3,0xc0,0x21,1,1,0,4};
	uint8_t wire[4096];
	size_t n = ds0_tx_frame(&tx, body, sizeof(body), wire, sizeof(wire));
	for (size_t i = 0; i < n; ++i) CHECK(ds0_probe_sample(&p, wire[i], true) == DS0_RATE_NONE, "short proof");
	CHECK(p.rx[1].rx64.clean_frame, "initial FCS is valid");
	if (bad_fcs) {
		n = ds0_tx_frame(&tx, body, sizeof(body), wire, sizeof(wire));
		wire[5] ^= 0x10;
		for (size_t i = 0; i < n; ++i) CHECK(ds0_probe_sample(&p, wire[i], true) == DS0_RATE_NONE, "bad FCS");
	} else {
		/* Abort an actual frame body, rather than a legal empty-frame mark. */
		CHECK(ds0_probe_sample(&p, 0, true) == DS0_RATE_NONE, "data before abort");
		for (unsigned i = 0; i < 4; ++i) CHECK(ds0_probe_sample(&p, 0xff, true) == DS0_RATE_NONE, "invalid stuffing/abort");
	}
	for (unsigned i = 0; i < 1500; ++i) CHECK(ds0_probe_sample(&p, 0x7e, true) == DS0_RATE_NONE, "old FCS cannot qualify a new clean run");
	CHECK(!p.rx[1].rx64.clean_frame, "failed framing retires proof");
	return true;
}
int main(void)
{
	if (!mark_boundaries() || !mark_idle(DS0_RATE_56000) || !mark_idle(DS0_RATE_64000) ||
	    !candidate(DS0_RATE_64000, false) || !candidate(DS0_RATE_56000, true) ||
	    !rejection() || !history_wrap(DS0_RATE_56000) || !history_wrap(DS0_RATE_64000) ||
	    !invalid_resets_proof(true) || !invalid_resets_proof(false)) return 1;
	puts("DS0 probe: 1200 clean codewords, valid FCS, robbed bits, retained maximum frames and rejection passed");
	return 0;
}
