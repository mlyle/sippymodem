// SPDX-License-Identifier: BSD-3-Clause
#include "ds0/ds0_ppp.h"
#include "hdlc_fcs.h"
#include "test.h"
#include <string.h>

static size_t async_frame_map(const uint8_t *body, size_t n, uint32_t map, uint8_t *out)
{
	uint16_t fcs = hdlc_fcs16(body, n) ^ 0xffffu;
	size_t count = 0; out[count++] = 0x7e;
	for (size_t i = 0; i < n + 2; ++i) {
		uint8_t b = i < n ? body[i] : (i == n ? fcs & 0xff : fcs >> 8);
		if ((b < 0x20 && (map & (UINT32_C(1) << b))) || b == 0x7e || b == 0x7d) {
			out[count++] = 0x7d; out[count++] = b ^ 0x20;
		} else out[count++] = b;
	}
	out[count++] = 0x7e; return count;
}

static size_t async_frame(const uint8_t *body, size_t n, uint8_t *out)
{
	return async_frame_map(body, n, UINT32_MAX, out);
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
	const uint8_t invalid[] = {0x7e, 0xff, 0x55, 0x55, 0x55, 0x7e, 0x7d, 0x7e};
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

static bool host_frame(struct ds0_ppp *p, const uint8_t *body, size_t n, uint32_t map)
{
	uint8_t async[DS0_MAX_ASYNC_FRAME_OCTETS], wire[DS0_MAX_TX_CODEC_OCTETS];
	size_t length = async_frame_map(body, n, map, async), used;
	uint32_t valid = p->counters.async_tx_valid_frames;
	CHECK(ds0_ppp_encode(p, async, length, wire, sizeof(wire), &used) > 0 &&
	      used == length && !p->tx_codec_pending_len && !p->overflow,
	      "complete host frame queued");
	CHECK(p->counters.async_tx_valid_frames == valid + 1 &&
	      p->tx_body_len == n && !memcmp(p->tx_body, body, n), "host body retained exactly");
	return true;
}

static bool peer_frame(struct ds0_ppp *p, const uint8_t *body, size_t n)
{
	uint8_t expected[DS0_MAX_ASYNC_FRAME_OCTETS], actual[DS0_MAX_ASYNC_FRAME_OCTETS];
	size_t length = async_frame(body, n, expected);
	ds0_ppp_receive_frame(p, body, n, p->codec.tx_rate);
	CHECK(!p->overflow && ds0_ppp_read(p, actual, sizeof(actual)) == length &&
	      !memcmp(actual, expected, length), "peer body converted exactly");
	return true;
}

static void map_request(uint8_t packet[14], uint8_t id, uint32_t map)
{
	const uint8_t prefix[] = {0xff, 3, 0xc0, 0x21, 1, id, 0, 10, 2, 6};
	memcpy(packet, prefix, sizeof(prefix));
	for (size_t i = 0; i < 4; ++i) packet[10 + i] = (uint8_t)(map >> (24 - 8 * i));
}

static bool map_opened(const struct ds0_ppp *p)
{
	return p->requests[0].acknowledged && p->requests[1].acknowledged;
}

static bool open_map(struct ds0_ppp *p, uint32_t local_map, uint32_t peer_map, bool ack_first)
{
	uint8_t local[14], peer[14], ack[14];
	map_request(local, 17, local_map); map_request(peer, 29, peer_map);
	CHECK(host_frame(p, local, sizeof(local), UINT32_MAX), "local request");
	memcpy(ack, local, sizeof(ack)); ack[4] = 2;
	++ack[5]; CHECK(peer_frame(p, ack, sizeof(ack)), "wrong ACK identifier");
	--ack[5]; ack[13] ^= 1; CHECK(peer_frame(p, ack, sizeof(ack)), "wrong ACK option");
	ack[13] ^= 1;
	CHECK(!map_opened(p) && p->host_accm == UINT32_MAX, "invalid ACK cannot open");
	if (ack_first) CHECK(peer_frame(p, ack, sizeof(ack)), "own request ACK first");
	CHECK(peer_frame(p, peer, sizeof(peer)), "peer request");
	peer[4] = 2; CHECK(host_frame(p, peer, sizeof(peer), UINT32_MAX), "peer request ACK");
	CHECK(p->host_accm == (ack_first ? peer_map : UINT32_MAX), "both ACKs required");
	if (!ack_first) CHECK(peer_frame(p, ack, sizeof(ack)), "own request ACK second");
	CHECK(map_opened(p) && p->host_accm == peer_map, "peer request governs host send map");
	return true;
}

static bool accm_negotiation(void)
{
	const uint32_t maps[] = {0, UINT32_MAX, UINT32_C(0x000a0000)};
	uint8_t body[260] = {0xff, 3, 0, 0x21};
	for (size_t i = 4; i < sizeof(body); ++i) body[i] = (uint8_t)(i - 4);
	for (unsigned rate = 0; rate < 2; ++rate) {
		struct ds0_config cfg; ds0_config_default(&cfg);
		cfg.rate_policy = rate ? DS0_RATE_FORCE_64000 : DS0_RATE_FORCE_56000;
		for (size_t local = 0; local < 3; ++local) for (size_t peer = 0; peer < 3; ++peer)
		for (unsigned order = 0; order < 2; ++order) {
			struct ds0_ppp p; CHECK(ds0_ppp_init(&p, &cfg) == 0, "init");
			CHECK(open_map(&p, maps[local], maps[peer], order != 0), "open map");
			CHECK(host_frame(&p, body, sizeof(body), maps[peer]), "all byte values after negotiation");
		}
	}
	return true;
}

static bool accm_noise(void)
{
	const uint8_t body[] = {0xff, 3, 0, 0x21, 0x7d, 0x7e, 0x11, 0x13, 0};
	uint8_t async[64], noisy[65], wire[64];
	size_t n = async_frame(body, sizeof(body), async), used;
	/* Every insertion point includes the middle of escape sequences. */
	for (size_t offset = 1; offset < n; ++offset) for (uint8_t b = 0; b < 32; ++b) {
		struct ds0_ppp p; CHECK(ds0_ppp_init(&p, NULL) == 0, "init");
		memcpy(noisy, async, offset); noisy[offset] = b;
		memcpy(noisy + offset + 1, async + offset, n - offset);
		CHECK(ds0_ppp_encode(&p, noisy, n + 1, wire, sizeof(wire), &used) > 0 && used == n + 1,
		      "mapped injected control discarded");
		CHECK(p.counters.async_tx_valid_frames == 1 && p.tx_body_len == sizeof(body) &&
		      !memcmp(p.tx_body, body, sizeof(body)), "default-map payload unchanged");
	}
	return true;
}

static bool accm_partial_restart(void)
{
	const uint8_t body[] = {0xff, 3, 0, 0x21, 0x11, 0x13, 0};
	uint8_t local[14], peer[14], async[64], wire[128]; size_t used;
	struct ds0_ppp p; CHECK(ds0_ppp_init(&p, NULL) == 0, "init");
	CHECK(open_map(&p, 0, 0, false), "open zero map");
	size_t n = async_frame_map(body, sizeof(body), 0, async);
	CHECK(ds0_ppp_encode(&p, async, 2, wire, sizeof(wire), &used) == 0 && used == 2,
	      "old-map frame partly assembled");
	map_request(peer, 30, UINT32_C(0x000a0000));
	CHECK(peer_frame(&p, peer, sizeof(peer)) && !map_opened(&p) && p.host_accm == 0,
	      "peer restart preserves already queued host map");
	CHECK(ds0_ppp_encode(&p, async + 2, n - 2, wire, sizeof(wire), &used) > 0 && used == n - 2 &&
	      p.tx_body_len == sizeof(body) && !memcmp(p.tx_body, body, sizeof(body)), "partial old-map frame survives");
	map_request(local, 18, 0); CHECK(host_frame(&p, local, sizeof(local), UINT32_MAX), "host restarts");
	CHECK(p.host_accm == UINT32_MAX, "host restart restores default");
	peer[4] = 2; CHECK(host_frame(&p, peer, sizeof(peer), UINT32_MAX), "ACK peer request");
	/* Start an old-default frame before the incoming ACK opens the new map. */
	n = async_frame(body, sizeof(body), async);
	CHECK(ds0_ppp_encode(&p, async, 2, wire, sizeof(wire), &used) == 0 && used == 2, "default frame prefix");
	local[4] = 2; CHECK(peer_frame(&p, local, sizeof(local)) && map_opened(&p) &&
	      p.host_accm == UINT32_C(0x000a0000), "new map opens during partial frame");
	/* NUL is mapped by the old policy only. Place it inside the partial frame. */
	memmove(async + 3, async + 2, n - 2); async[2] = 0; ++n;
	CHECK(ds0_ppp_encode(&p, async + 2, n - 2, wire, sizeof(wire), &used) > 0 && used == n - 2 &&
	      p.tx_body_len == sizeof(body) && !memcmp(p.tx_body, body, sizeof(body)), "partial frame keeps its original policy");
	CHECK(host_frame(&p, body, sizeof(body), UINT32_C(0x000a0000)), "next frame uses new policy");
	const uint8_t terminate[] = {0xff, 3, 0xc0, 0x21, 5, 19, 0, 4};
	CHECK(peer_frame(&p, terminate, sizeof(terminate)) && !map_opened(&p) &&
	      p.host_accm == UINT32_C(0x000a0000), "peer terminate preserves queued bytes");
	uint8_t reply[sizeof(terminate)]; memcpy(reply, terminate, sizeof(reply)); reply[4] = 6;
	CHECK(host_frame(&p, reply, sizeof(reply), UINT32_MAX) && p.host_accm == UINT32_MAX,
	      "host terminate response restores default");
	CHECK(ds0_ppp_init(&p, NULL) == 0 && p.host_accm == UINT32_MAX && !map_opened(&p) &&
	      !p.requests[0].length && !p.requests[1].length, "new call has no stale negotiation");
	return true;
}


static bool accm_lcp_bounds(void)
{
	for (unsigned variant = 0; variant < 10; ++variant) {
		struct ds0_ppp p; CHECK(ds0_ppp_init(&p, NULL) == 0, "init");
		uint8_t packet[20]; map_request(packet, 17, 0); size_t n = 14;
		switch (variant) {
		case 0: n = 7; break;
		case 1: packet[3] = 0x23; break;
		case 2: packet[7] = 3; break;
		case 3: packet[7] = 11; break;
		case 4: packet[9] = 0; break;
		case 5: packet[9] = 1; break;
		case 6: packet[9] = 7; break;
		case 7: packet[9] = 5; break;
		case 8: packet[7] = 5; n = 9; break;
		case 9: memcpy(packet + 14, packet + 8, 6); packet[7] = 16; n = 20; break;
		}
		CHECK(peer_frame(&p, packet, n) && !p.requests[0].length && !map_opened(&p),
		      "malformed peer negotiation ignored");
		CHECK(host_frame(&p, packet, n, UINT32_MAX) && !p.requests[1].length &&
		      p.host_accm == UINT32_MAX, "malformed host negotiation ignored");
	}
	for (unsigned rate = 0; rate < 2; ++rate) {
		struct ds0_config cfg; ds0_config_default(&cfg);
		cfg.rate_policy = rate ? DS0_RATE_FORCE_64000 : DS0_RATE_FORCE_56000;
		struct ds0_ppp p; CHECK(ds0_ppp_init(&p, &cfg) == 0, "init");
		uint8_t local[14], peer[DS0_MAX_FRAME_OCTETS], ack[DS0_MAX_FRAME_OCTETS];
		map_request(local, 17, 0); map_request(peer, 29, 0);
		/* Exact maximum request storage, including otherwise uninterpreted options. */
		for (size_t i = 14; i < sizeof(peer); i += 2) { peer[i] = 0xfe; peer[i + 1] = 2; }
		peer[sizeof(peer) - 2] = 0xfd;
		peer[6] = (uint8_t)((sizeof(peer) - 4) >> 8); peer[7] = (uint8_t)(sizeof(peer) - 4);
		CHECK(host_frame(&p, local, sizeof(local), UINT32_MAX) &&
		      peer_frame(&p, peer, sizeof(peer)), "maximum peer request retained");
		local[4] = 2; CHECK(peer_frame(&p, local, sizeof(local)), "local request acknowledged");
		memcpy(ack, peer, sizeof(ack)); ack[4] = 2;
		ack[sizeof(ack) - 4] = 0xfd; ack[sizeof(ack) - 2] = 0xfe;
		CHECK(host_frame(&p, ack, sizeof(ack), UINT32_MAX) && !map_opened(&p) &&
		      p.host_accm == UINT32_MAX, "reordered ACK rejected");
		memcpy(ack, peer, sizeof(ack)); ack[4] = 2;
		CHECK(host_frame(&p, ack, sizeof(ack), UINT32_MAX) && map_opened(&p) && !p.host_accm,
		      "exact maximum ACK opens map");
	}
	/* FCS rejection must happen before LCP observation. */
	struct ds0_ppp p; CHECK(ds0_ppp_init(&p, NULL) == 0, "init");
	uint8_t body[14], async[64], wire[64]; size_t used;
	map_request(body, 17, 0); size_t n = async_frame(body, sizeof(body), async);
	async[1] ^= 1;
	CHECK(!ds0_ppp_encode(&p, async, n, wire, sizeof(wire), &used) && used == n &&
	      p.counters.async_tx_fcs_fail == 1 && !p.requests[1].length, "bad FCS cannot alter map state");
	/* LCP length excludes padding; no ACCM option means the default map. */
	const uint8_t padded[] = {0xff, 3, 0xc0, 0x21, 1, 17, 0, 4, 2, 6, 0};
	CHECK(host_frame(&p, padded, sizeof(padded), UINT32_MAX) && p.requests[1].length == 3 &&
	      p.requests[1].accm == UINT32_MAX, "padding excluded from request");
	return true;
}


static bool accm_negotiation_transitions(void)
{
	for (unsigned variant = 0; variant < 5; ++variant) {
		struct ds0_ppp p; CHECK(ds0_ppp_init(&p, NULL) == 0, "init");
		uint8_t local[14], peer[14]; map_request(local, 17, 0); map_request(peer, 29, 0);
		CHECK(host_frame(&p, local, sizeof(local), UINT32_MAX), "local request");
		local[4] = 2;
		if (variant == 2 || variant == 3) {
			CHECK(peer_frame(&p, local, sizeof(local)), "Ack-Rcvd setup");
		} else {
			CHECK(peer_frame(&p, peer, sizeof(peer)), "peer request");
			peer[4] = 2; CHECK(host_frame(&p, peer, sizeof(peer), UINT32_MAX), "Ack-Sent setup");
		}
		uint8_t control[] = {0xff, 3, 0xc0, 0x21, 6, 51, 0, 4, 0x7f, 1, 0, 4};
		if (!variant) control[4] = 5;
		if (variant == 3) { control[4] = 7; control[7] = 8; }
		if (variant == 4) {
			const uint8_t malformed[] = {0xff, 3, 0xc0, 0x21, 1, 51, 0, 6, 2, 1};
			CHECK(peer_frame(&p, malformed, sizeof(malformed)), "malformed peer request");
			control[4] = 4; CHECK(host_frame(&p, control, sizeof(control), UINT32_MAX), "host rejection");
		} else {
			CHECK(peer_frame(&p, control, sizeof(control)), "peer control transition");
			if (!variant) { control[4] = 6; CHECK(host_frame(&p, control, sizeof(control), UINT32_MAX), "terminate reply"); }
		}
		CHECK(peer_frame(&p, local, sizeof(local)), "outstanding or duplicate ACK");
		if (variant == 1) {
			CHECK(map_opened(&p) && !p.host_accm, "Terminate-Ack leaves Ack-Sent unchanged");
			continue;
		}
		CHECK(!map_opened(&p) && p.host_accm == UINT32_MAX, "no premature opening");
		peer[4] = 1; CHECK(peer_frame(&p, peer, sizeof(peer)), "new peer request");
		peer[4] = 2; CHECK(host_frame(&p, peer, sizeof(peer), UINT32_MAX), "ACK new request");
		if (variant == 2 || variant == 3) {
			CHECK(!map_opened(&p), "consumed ACK cannot be reused");
			local[4] = 1; CHECK(host_frame(&p, local, sizeof(local), UINT32_MAX), "host retransmits");
			local[4] = 2; CHECK(peer_frame(&p, local, sizeof(local)), "ACK retransmission");
		}
		CHECK(map_opened(&p) && !p.host_accm, "negotiation recovers");
	}
	struct ds0_ppp p; CHECK(ds0_ppp_init(&p, NULL) == 0, "init");
	uint8_t first[14], second[14]; map_request(first, 17, 0); map_request(second, 18, UINT32_MAX);
	ds0_ppp_receive_frame(&p, first, sizeof(first), DS0_RATE_64000);
	ds0_ppp_receive_frame(&p, second, sizeof(second), DS0_RATE_64000);
	CHECK(p.overflow && p.requests[0].bytes[0] == 17 && !p.requests[0].accm,
	      "failed queue cannot alter negotiation");
	return true;
}

int main(void)
{
	if (!partial_frames() || !invalid_and_overflow() || !short_frames() || !oversized_frame() || !assembly_without_idle() || !opening_flag_and_rate_change() || !accm_negotiation() || !accm_noise() || !accm_partial_restart() || !accm_lcp_bounds() || !accm_negotiation_transitions()) return 1;
	puts("DS0 PPP: partial I/O, both rates, escapes, FCS, negotiated ACCM and overflow passed");
	return 0;
}
