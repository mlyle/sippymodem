// SPDX-License-Identifier: BSD-3-Clause
#include "rtp_transmit.h"
#include "test.h"
#include <stdio.h>
#include <string.h>

static bool test_bytes_and_bounds(void)
{
	struct rtp_transmit *queue = rtp_transmit_create(10000000, 30000000, 32);
	CHECK(queue != NULL, "queue allocation failed");
	uint8_t input[32];
	memset(input, 0x5a, sizeof(input));
	for (unsigned i = 0; i < RTP_TRANSMIT_CAPACITY; i++) {
		input[0] = (uint8_t)i;
		CHECK(rtp_transmit_queue(queue, input, sizeof(input), 1000000000) == RTP_TRANSMIT_OK,
		      "bounded queue rejected packet %u", i);
	}
	struct rtp_transmit_status before = rtp_transmit_get_status(queue);
	CHECK(rtp_transmit_queue(queue, input, sizeof(input), 1000000000) == RTP_TRANSMIT_FULL,
	      "full queue accepted extra input");
	struct rtp_transmit_status after = rtp_transmit_get_status(queue);
	CHECK(before.accepted == after.accepted && before.queued == after.queued &&
	      before.clock_epochs == after.clock_epochs, "rejected admission changed queue or clock");
	memset(input, 0, sizeof(input));
	uint64_t now = 1000000000;
	for (unsigned i = 0; i < RTP_TRANSMIT_CAPACITY; i++) {
		uint64_t wait = rtp_transmit_wait_ns(queue, now);
		CHECK(wait != UINT64_MAX, "queued packet lost its deadline");
		now += wait;
		size_t size;
		const uint8_t *packet = rtp_transmit_peek(queue, now, &size);
		CHECK(packet != NULL && size == 32 && packet[0] == i && packet[1] == 0x5a,
		      "copied packet %u lost bytes or FIFO order", i);
		CHECK(rtp_transmit_peek(queue, now, &size) == packet,
		      "observation consumed packet or credit");
		CHECK(rtp_transmit_sent(queue, now), "successful send was not retired");
	}
	after = rtp_transmit_get_status(queue);
	CHECK(rtp_transmit_wait_ns(queue, now) == UINT64_MAX &&
	      after.accepted == RTP_TRANSMIT_CAPACITY && after.sent == RTP_TRANSMIT_CAPACITY,
	      "empty queue retained work or lost accounting");
	CHECK(!rtp_transmit_sent(queue, now), "empty queue certified another send");
	rtp_transmit_destroy(queue);
	return true;
}

static bool test_epochs(void)
{
	struct rtp_transmit *queue = rtp_transmit_create(10000000, 30000000, 1);
	uint8_t byte = 1;
	CHECK(queue != NULL, "queue allocation failed");
	CHECK(rtp_transmit_queue(queue, &byte, 1, 1000000000) == RTP_TRANSMIT_OK &&
	      rtp_transmit_sent(queue, 1030000000), "first reserved output did not complete");
	CHECK(rtp_transmit_queue(queue, &byte, 1, 1039000000) == RTP_TRANSMIT_OK &&
	      rtp_transmit_get_status(queue).clock_epochs == 1 &&
	      rtp_transmit_sent(queue, 1040000000),
	      "empty queue incorrectly renewed reserve before a missed deadline");
	CHECK(rtp_transmit_queue(queue, &byte, 1, 1200000000) == RTP_TRANSMIT_OK &&
	      rtp_transmit_get_status(queue).clock_epochs == 2 &&
	      rtp_transmit_wait_ns(queue, 1200000000) == 30000000,
	      "genuine underrun did not establish fresh reserve");
	CHECK(rtp_transmit_clear(queue) == 1 && rtp_transmit_get_status(queue).canceled == 1,
	      "clear lost cancellation accounting");
	size_t size = 99;
	CHECK(rtp_transmit_peek(queue, UINT64_MAX, &size) == NULL && size == 0,
	      "cleared queue retained packet data");
	rtp_transmit_destroy(queue);
	return true;
}

static bool test_bounds(void)
{
	CHECK(rtp_transmit_create(0, 30000000, 1) == NULL &&
	      rtp_transmit_create(10000000, 30000000, SIZE_MAX) == NULL,
	      "invalid geometry or allocation overflow accepted");
	struct rtp_transmit *queue = rtp_transmit_create(10000000, 30000000, 1);
	uint8_t byte = 0;
	CHECK(queue != NULL, "queue allocation failed");
	CHECK(rtp_transmit_queue(queue, &byte, 1, UINT64_MAX - 100) == RTP_TRANSMIT_INVALID &&
	      rtp_transmit_get_status(queue).accepted == 0 &&
	      rtp_transmit_get_status(queue).queued == 0, "unrepresentable deadline changed queue");
	CHECK(rtp_transmit_queue(queue, &byte, 1, 1000000000) == RTP_TRANSMIT_OK &&
	      rtp_transmit_queue(queue, &byte, 1, 999999999) == RTP_TRANSMIT_INVALID &&
	      rtp_transmit_get_status(queue).accepted == 1 &&
	      rtp_transmit_get_status(queue).queued == 1, "backward clock changed admitted packet");
	rtp_transmit_destroy(queue);
	return true;
}

enum production_kind { STEADY, HOLD, BATCH };

static uint64_t admission(unsigned n, uint64_t frame, unsigned numerator, unsigned denominator,
	enum production_kind kind)
{
	uint64_t time = UINT64_C(1000000000) + (uint64_t)n * frame * denominator / numerator;
	if (kind == BATCH)
		time = UINT64_C(1000000000) + (uint64_t)(n / 4) * 4 * frame;
	if (kind == HOLD && n >= 500 && n <= 502)
		time = UINT64_C(1000000000) + 502 * frame;
	return time;
}

static bool clock_case(uint64_t frame, unsigned numerator, unsigned denominator,
	enum production_kind kind)
{
	enum { COUNT = 1200 };
	struct rtp_transmit *queue = rtp_transmit_create(frame, 30000000, sizeof(uint64_t));
	CHECK(queue != NULL, "clock queue allocation");
	uint64_t now = 1000000000, sent_times[COUNT], largest_gap = 0;
	unsigned produced = 0, sent = 0;
	for (unsigned pass = 0; sent < COUNT && pass < COUNT * 5; pass++) {
		uint64_t wait = rtp_transmit_wait_ns(queue, now);
		uint64_t due = wait == UINT64_MAX ? UINT64_MAX : now + wait;
		uint64_t incoming = produced == COUNT ? UINT64_MAX : admission(produced, frame, numerator, denominator, kind);
		uint64_t next = incoming < due ? incoming : due;
		CHECK(next != UINT64_MAX && next >= now, "clock stalled or moved backward");
		now = next;
		while (produced < COUNT && admission(produced, frame, numerator, denominator, kind) <= now) {
			uint64_t value = produced++;
			CHECK(rtp_transmit_queue(queue, &value, sizeof(value), now) == RTP_TRANSMIT_OK,
			      "rate %u/%u overflowed", numerator, denominator);
		}
		for (unsigned budget = 0; budget < 4; budget++) {
			size_t size;
			const uint8_t *packet = rtp_transmit_peek(queue, now, &size);
			if (packet == NULL)
				break;
			uint64_t value;
			CHECK(size == sizeof(value), "queued byte count changed");
			memcpy(&value, packet, sizeof(value));
			CHECK(value == sent && rtp_transmit_sent(queue, now), "clock reordered or duplicated media");
			if (sent && now - sent_times[sent - 1] > largest_gap)
				largest_gap = now - sent_times[sent - 1];
			sent_times[sent++] = now;
		}
	}
	CHECK(produced == COUNT && sent == COUNT, "clock lost media");
	uint64_t source_span = admission(COUNT - 1, frame, numerator, denominator, kind) -
		admission(COUNT - 501, frame, numerator, denominator, kind);
	uint64_t wire_span = sent_times[COUNT - 1] - sent_times[COUNT - 501];
	uint64_t error = source_span > wire_span ? source_span - wire_span : wire_span - source_span;
	CHECK(error * UINT64_C(1000000) <= source_span * 10, "tail clock differs by more than10ppm");
	if (kind != STEADY) {
		CHECK(largest_gap <= 15000000, "hold or batching recreated an RTP gap");
	} else {
		uint64_t residence = 0;
		for (unsigned n = COUNT - 500; n < COUNT; n++)
			residence += sent_times[n] - admission(n, frame, numerator, denominator, kind);
		uint64_t mean = residence / 500;
		CHECK(mean >= 30000000 - frame / 10 && mean <= 30000000 + frame / 10,
		      "rate learning consumed or grew reserve");
	}
	struct rtp_transmit_status status = rtp_transmit_get_status(queue);
	CHECK(status.accepted == COUNT && status.sent == COUNT && status.queued == 0 && !status.credit_limited,
	      "autonomous production misclassified as credit limited");
	rtp_transmit_destroy(queue);
	return true;
}

static bool test_clocks(void)
{
	const uint64_t frames[] = { 1000000, 10000000, 20000000 };
	const unsigned rates[][2] = { {1,1}, {10002,10000}, {9998,10000}, {6,5}, {4,3}, {4,5} };
	for (size_t f = 0; f < ARRAY_LENGTH(frames); f++)
		for (size_t r = 0; r < ARRAY_LENGTH(rates); r++)
			CHECK(clock_case(frames[f], rates[r][0], rates[r][1], STEADY), "steady clock matrix");
	CHECK(clock_case(10000000, 1, 1, HOLD), "availability hold");
	CHECK(clock_case(10000000, 1, 1, BATCH), "coalesced packet production");
	return true;
}

static bool test_feedback(void)
{
	struct rtp_transmit *queues[2] = {
		rtp_transmit_create(10000000, 30000000, sizeof(uint64_t)),
		rtp_transmit_create(10000000, 30000000, sizeof(uint64_t)),
	};
	CHECK(queues[0] && queues[1], "feedback allocation");
	unsigned produced[2] = {1,1}, sent[2] = {0,0}, pending[2] = {0,0};
	uint64_t zero = 0, now = 1000000000;
	for (unsigned side = 0; side < 2; side++)
		CHECK(rtp_transmit_queue(queues[side], &zero, sizeof(zero), now) == RTP_TRANSMIT_OK, "initial credit");
	for (unsigned pass = 0; pass < 10000 && now < 3000000000; pass++) {
		bool delivered = pending[0] || pending[1];
		for (unsigned side = 0; side < 2; side++)
			while (pending[side]) {
				uint64_t value = produced[side]++;
				pending[side]--;
				CHECK(rtp_transmit_queue(queues[side], &value, sizeof(value), now) == RTP_TRANSMIT_OK,
				      "feedback overflow");
			}
		uint64_t a = rtp_transmit_wait_ns(queues[0], now), b = rtp_transmit_wait_ns(queues[1], now);
		uint64_t wait = a < b ? a : b;
		CHECK(wait != UINT64_MAX, "both feedback clocks stopped");
		if (!delivered) now += wait;
		for (unsigned side = 0; side < 2; side++)
			for (unsigned budget = 0; budget < 4; budget++) {
				size_t size;
				const uint8_t *packet = rtp_transmit_peek(queues[side], now, &size);
				if (packet == NULL) break;
				uint64_t value;
				CHECK(size == sizeof(value), "feedback byte count");
				memcpy(&value, packet, sizeof(value));
				CHECK(value == sent[side]++ && rtp_transmit_sent(queues[side], now), "feedback order");
				pending[1-side]++;
			}
	}
	for (unsigned side = 0; side < 2; side++) {
		CHECK(sent[side] > 200 && rtp_transmit_get_status(queues[side]).credit_limited,
		      "reserve throttled a one-credit feedback source");
		rtp_transmit_destroy(queues[side]);
	}
	return true;
}

int main(void)
{
	if (!test_bytes_and_bounds() || !test_epochs() || !test_bounds() || !test_clocks() || !test_feedback())
		return 1;
	puts("RTP transmit byte ownership, bounds, epochs, clock matrix, feedback and cancellation passed");
	return 0;
}
