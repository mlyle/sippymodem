// SPDX-License-Identifier: BSD-3-Clause
#include "v91_call.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>

static uint8_t random_bit(uint32_t *state)
{
	*state ^= *state << 13;
	*state ^= *state >> 17;
	*state ^= *state << 5;
	return *state & 1;
}
static void source(void *context, uint8_t *bits, size_t count)
{
	for (size_t i = 0; i < count; ++i)
		bits[i] = random_bit(context);
}
static uint8_t transform(uint8_t octet, unsigned impairment, enum g711_law law, unsigned n)
{
	if (impairment == 1 && n % 6 == 2)
		return (octet & ~1u) | ((n / 6) & 1u);
	if (impairment == 2)
		return g711_encode(law, g711_decode(law, octet) / 2);
	return octet;
}
static bool test_duplex(enum g711_law a_law, enum g711_law b_law,
	unsigned a_impairment, unsigned b_impairment, unsigned delay)
{
	struct v91_call *a = calloc(1, sizeof(*a)), *b = calloc(1, sizeof(*b));
	uint8_t ab[2000], ba[2000];
	uint32_t a_tx = 1234567, b_rx = a_tx, b_tx = 7654321, a_rx = b_tx;
	unsigned received[2] = { 0 };
	CHECK(a && b && delay < sizeof(ab), "allocation/argument failure");
	CHECK(v91_call_init(a, a_law) && v91_call_init(b, b_law), "initialization failed");
	memset(ab, 0xff, sizeof(ab));
	memset(ba, 0xff, sizeof(ba));
	for (unsigned n = 0; n < 100000; ++n) {
		unsigned slot = n % (delay + 1);
		uint8_t atx, btx;
		struct v91_event ae, be;
		bool ar = v91_call_process(a, ba[slot], true, &atx, source, &a_tx, &ae);
		bool br = v91_call_process(b, ab[slot], true, &btx, source, &b_tx, &be);
		CHECK(ar && br, "sample=%u failure=%u/%u stages=%u,%u/%u,%u counters=%u/%u",
		      n, ae.failure, be.failure, a->tx_stage, a->rx_stage, b->tx_stage,
		      b->rx_stage, a->rx_count, b->rx_count);
		ab[slot] = transform(atx, a_impairment, a_law, n);
		ba[slot] = transform(btx, b_impairment, b_law, n);
		for (unsigned i = 0; i < ae.count; ++i)
			CHECK(ae.bits[i] == random_bit(&a_rx), "A payload differs at %u", n);
		for (unsigned i = 0; i < be.count; ++i)
			CHECK(be.bits[i] == random_bit(&b_rx), "B payload differs at %u", n);
		received[0] += ae.count;
		received[1] += be.count;
		if (received[0] > 200000 && received[1] > 200000)
			break;
	}
	CHECK(received[0] > 200000 && received[1] > 200000, "did not deliver enough data");
	printf("duplex laws=%u/%u impairments=%u/%u delay=%u bits=%u/%u frame_bits=%u/%u\n",
		a_law, b_law, a_impairment, b_impairment, delay, received[0], received[1],
		a->encoder.mapping.frame_bits, b->encoder.mapping.frame_bits);
	free(a); free(b);
	return true;
}
static void alternating(void *context, uint8_t *bits, size_t count)
{
	(void)context;
	for (size_t i = 0; i < count; ++i)
		bits[i] = i & 1;
}
static bool test_recovery(unsigned fault)
{
	struct v91_call *a = calloc(1, sizeof(*a)), *b = calloc(1, sizeof(*b));
	uint8_t ab[80], ba[80];
	CHECK(a && b && v91_call_init(a, G711_LAW_PCMU) &&
	      v91_call_init(b, G711_LAW_PCMU), "recovery initialization failed");
	memset(ab, 0xff, sizeof(ab));
	memset(ba, 0xff, sizeof(ba));
	unsigned recovered_bits = 0;
	for (unsigned n = 0; n < 150000; ++n) {
		struct v91_event ae, be;
		uint8_t atx, btx;
		unsigned slot = n % 80;
		if ((n == 30000 && fault == 0) || (n == 32000 && fault == 2))
			CHECK(v91_call_retrain(a), "explicit retrain failed");
		uint8_t received = ba[(slot + (n >= 30000 && fault == 3)) % 80];
		if (fault == 4 && n == 1000) {
			CHECK(a->rx_stage == V91_RX_J, "DIL descriptor fault missed J");
			received ^= 0x80;
		}
		bool ar = v91_call_process(a, received,
			!(n == 30000 && fault == 1),
			&atx, alternating, NULL, &ae);
		bool br = v91_call_process(b, ab[slot], true, &btx, alternating, NULL, &be);
		CHECK(ar && br, "recovery fault=%u sample=%u failed %u/%u retrains=%u/%u stage=%u,%u/%u,%u",
			fault, n, ae.failure, be.failure, a->retrains, b->retrains,
			a->tx_stage, a->rx_stage, b->tx_stage, b->rx_stage);
		ab[slot] = atx;
		ba[slot] = n >= 30000 && fault == 2 ?
			(btx & ~1u) | ((n / 6) & 1u) : btx;
		if (n > 30000 && a->retrains && b->retrains && ae.online && be.online) {
			for (unsigned i = 0; i < ae.count; ++i)
				CHECK(ae.bits[i] == (i & 1), "recovered A payload corrupt at %u", n);
			for (unsigned i = 0; i < be.count; ++i)
				CHECK(be.bits[i] == (i & 1), "recovered B payload corrupt at %u", n);
			recovered_bits += ae.count + be.count;
			if (recovered_bits >= 100000)
				break;
		}
	}
	CHECK(recovered_bits >= 100000, "fault=%u recovery stalled, retrains=%u/%u",
		fault, a->retrains, b->retrains);
	printf("recovery fault=%u passed retrains=%u/%u frame_bits=%u/%u\n", fault,
		a->retrains, b->retrains, a->encoder.mapping.frame_bits, b->encoder.mapping.frame_bits);
	free(a); free(b);
	return true;
}

static bool test_hangup(unsigned when, bool control)
{
	struct v91_call *a = calloc(1, sizeof(*a)), *b = calloc(1, sizeof(*b));
	uint8_t ab = 0xff, ba = 0xff;
	CHECK(a && b && v91_call_init(a, G711_LAW_PCMU) &&
	      v91_call_init(b, G711_LAW_PCMU), "hangup initialization failed");
	a->local_info.control_channel = b->local_info.control_channel = control;
	for (unsigned n = 0; n < when + 24000; ++n) {
		struct v91_event ae, be;
		uint8_t atx = 0xff, btx = 0xff;
		if (n == when)
			CHECK(v91_call_hangup(a), "hangup request failed");
		if (!a->failure)
			v91_call_process(a, ba, true, &atx, NULL, NULL, &ae);
		if (!b->failure)
			v91_call_process(b, ab, true, &btx, NULL, NULL, &be);
		ab = atx; ba = btx;
		if (a->failure && b->failure)
			break;
	}
	CHECK(a->failure == V91_FAILURE_LOCAL_CLEARDOWN &&
	      b->failure == V91_FAILURE_PEER_CLEARDOWN,
	      "hangup at %u control=%u failed %u/%u stages=%u/%u", when, control,
	      a->failure, b->failure, a->tx_stage, b->rx_stage);
	free(a); free(b);
	return true;
}

static bool test_deadlines(void)
{
	struct v91_call *call = calloc(1, sizeof(*call));
	CHECK(call && v91_call_init(call, G711_LAW_PCMU), "deadline initialization failed");
	struct v91_event event;
	uint8_t output;
	for (unsigned n = 0; n < 14000 && !call->failure; ++n)
		v91_call_process(call, 0xff, true, &output, NULL, NULL, &event);
	CHECK(call->failure == V91_FAILURE_INFO_TIMEOUT, "silent peer did not time out");
	free(call);
	return true;
}

static bool test_recovery_budget(bool rearm, bool corrupt)
{
	struct v91_call *a = calloc(1, sizeof(*a)), *b = calloc(1, sizeof(*b));
	uint8_t ab = 0xff, ba = 0xff;
	CHECK(a && b && v91_call_init(a, G711_LAW_PCMU) &&
	      v91_call_init(b, G711_LAW_PCMU), "budget initialization failed");
	unsigned third = rearm ? 160000 : 60000;
	for (unsigned n = 0; n < third + 15000; ++n) {
		struct v91_event ae, be;
		uint8_t atx = 0xff, btx = 0xff;
		bool available = n != 30000 && n != 45000 && n != third;
		if (corrupt && n == third) {
			memset(a->channel.known, 0, sizeof(a->channel.known));
			available = true;
		}
		bool ar = v91_call_process(a, ba, available, &atx, NULL, NULL, &ae);
		bool br = v91_call_process(b, ab, true, &btx, NULL, NULL, &be);
		if (!rearm && n == third) {
			CHECK(!ar && ae.failure == V91_FAILURE_RECOVERY_EXHAUSTED,
			      "repeated brief recovery reset the retry budget");
			break;
		}
		CHECK(ar && br, "budget rearm=%u sample=%u failed %u/%u", rearm, n, ae.failure, be.failure);
		ab = atx; ba = btx;
	}
	if (rearm)
		CHECK(a->retrains == 3 && a->tx_stage == V91_TX_DATA && a->rx_stage == V91_RX_DATA,
		      "sustained healthy data did not rearm automatic recovery");
	free(a); free(b);
	return true;
}

int main(void)
{
	if (!test_deadlines() || !test_recovery_budget(false, false) || !test_recovery_budget(false, true) ||
	    !test_recovery_budget(true, false)) return 1;
	for (unsigned control = 0; control < 2; ++control)
		if (!test_hangup(4000, control) || !test_hangup(30000, control)) return 1;
	for (unsigned fault = 0; fault < 5; ++fault)
		if (!test_recovery(fault)) return 1;
	for (unsigned law = 0; law < 2; ++law)
		for (unsigned impairment = 0; impairment < 3; ++impairment)
			if (!test_duplex(law, !law, impairment, (impairment + 1) % 3, 79))
				return 1;
	return test_duplex(G711_LAW_PCMU, G711_LAW_PCMU, 0, 0, 0) &&
	       test_duplex(G711_LAW_PCMA, G711_LAW_PCMA, 1, 1, 977) ? 0 : 1;
}
