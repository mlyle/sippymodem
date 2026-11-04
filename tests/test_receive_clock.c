// SPDX-License-Identifier: BSD-3-Clause
#include "receive_clock.h"
#include "rtp_receiver.h"
#include "test.h"

#include <stdlib.h>
#include <string.h>

struct arrival { uint64_t time; uint32_t ordinal; };

static int compare_arrival(const void *left, const void *right)
{
	const struct arrival *a = left, *b = right;
	return a->time < b->time ? -1 : a->time > b->time ? 1 :
		a->ordinal < b->ordinal ? -1 : a->ordinal > b->ordinal;
}

/* One virtual hour through the real timestamp storage, with distinct bytes
 * at every sample. Arrival order is independent of source and playout time.
 * A source step reverses the ppm offset halfway through the hour. */
static bool stream_case(int ppm, bool jitter, bool step, bool loss, bool stall, size_t frame_samples, size_t packet_samples)
{
	CHECK(frame_samples && frame_samples <= 2048 && packet_samples && packet_samples <= 2048,
	      "test block exceeds bounds");
	const uint64_t end = UINT64_C(3600000000000);
	const uint32_t origin = UINT32_MAX - 159;
	const size_t reserve = 480 + frame_samples;
	struct rtp_jitter_slot slots[16480];
	struct rtp_receiver rx;
	const struct rtp_receiver_config config = {
		.payload_type = 0, .silence_octet = 255, .prefill_samples = reserve,
	};
	struct receive_clock clock;
	CHECK(rtp_receiver_init(&rx, &config, slots, ARRAY_LENGTH(slots)) &&
	      receive_clock_init(&clock, frame_samples, 8000, reserve, 4), "stream initialization failed");
	const size_t capacity = (size_t)(UINT64_C(38400000) / packet_samples + 100);
	const uint64_t numerator = packet_samples * UINT64_C(125000000000);
	struct arrival *arrivals = calloc(capacity, sizeof(*arrivals));
	CHECK(arrivals != NULL, "arrival allocation failed");
	size_t count = 0;
	uint64_t cut = UINT64_C(14400000) * (1000000 + ppm) / (packet_samples * UINT64_C(1000000));
	uint64_t cut_time = cut * numerator / (uint64_t)(1000000 + ppm);
	for (size_t index = 0; index < capacity; ++index) {
		uint64_t time = !step || index < cut ?
			index * numerator / (uint64_t)(1000000 + ppm) :
			cut_time + (index - cut) * numerator / (uint64_t)(1000000 - ppm);
		if (time >= end) break;
		if (jitter && index % 997 == 2) time += 20000000;
		if (jitter && index % 257 == 10) time += 30000000;
		if (loss && index == 12345) continue;
		arrivals[count++] = (struct arrival){ time, (uint32_t)index };
	}
	CHECK(count < capacity, "test source exceeds storage");
	qsort(arrivals, count, sizeof(*arrivals), compare_arrival);
	size_t next = 0;
	uint64_t play = 0, last_play = UINT64_MAX, same_time = 0;
	uint8_t payload[2048], output[2048];
	bool mask[2048];
	while (play < end || (next < count && arrivals[next].time < end)) {
		uint64_t now;
		if (next < count && arrivals[next].time <= play) {
			now = arrivals[next].time;
			struct rtp_packet_view packet = {
				.header = { .ssrc = 7, .sequence = (uint16_t)arrivals[next].ordinal,
					.timestamp = origin + (uint32_t)(arrivals[next].ordinal * packet_samples) },
				.payload = payload, .payload_size = packet_samples,
			};
			for (size_t i = 0; i < packet_samples; ++i)
				payload[i] = (uint8_t)((packet.header.timestamp + i) * 37 + 11);
			CHECK(rtp_receiver_push(&rx, &packet) == RTP_RECEIVE_ACCEPTED,
			      "ppm%d source rejected at %.3fs", ppm, now / 1e9);
			(void)rtp_receiver_take_clock_frames(&rx, frame_samples);
			next++;
			continue;
		}
		now = play;
		if (now >= end) break;
		if (stall && now >= UINT64_C(10000000000) && now < UINT64_C(10100000000)) {
			play = UINT64_C(10100000000);
			continue;
		}
		CHECK(receive_clock_take(&clock, now) == FRAME_CLOCK_OK,
		      "clock failed to service deadline at %.3fs", now / 1e9);
		same_time = now == last_play ? same_time + 1 : 1;
		CHECK(same_time <= 4, "repeated owner passes renewed burst credit");
		if (!stall && last_play != UINT64_MAX)
			CHECK(now - last_play <= (uint64_t)frame_samples * 1000000000 / 8000 * 4 / 3 + 2, "packet jitter became a TX pause");
		last_play = now;
		struct rtp_playout_report report;
		CHECK(rtp_receiver_play(&rx, output, mask, frame_samples, &report), "playout failed");
		for (size_t i = report.prefill_samples; i < frame_samples; ++i) {
			uint32_t timestamp = report.first_timestamp + (uint32_t)(i - report.prefill_samples);
			bool missing = loss && (uint32_t)(timestamp - (origin + 12345 * packet_samples)) < packet_samples;
			CHECK(mask[i] != missing, "ppm%d wrong provenance at %.3fs sample%u", ppm, now / 1e9, timestamp);
			if (mask[i])
				CHECK(output[i] == (uint8_t)(timestamp * 37 + 11), "received samples reordered or changed");
		}
		struct rtp_playout_status status = rtp_receiver_playout_status(&rx, frame_samples);
		CHECK(receive_clock_observe(&clock, &status), "clock update overflowed");
		uint64_t wait = receive_clock_wait_ns(&clock, now);
		CHECK(wait <= UINT64_MAX - now, "unbounded clock wait");
		play = now + wait;
	}
	free(arrivals);
	CHECK(rx.stats.samples_concealed == (loss ? packet_samples : 0u) &&
	      rx.stats.late_samples == 0 && rx.stats.early_samples == 0,
	      "ppm%d lost source samples: concealed%llu late%llu early%llu", ppm,
	      (unsigned long long)rx.stats.samples_concealed,
	      (unsigned long long)rx.stats.late_samples, (unsigned long long)rx.stats.early_samples);
	return true;
}

static bool boundaries(void)
{
	struct receive_clock clock, before;
	CHECK(receive_clock_init(&clock, 80, 8000, 480, 4), "clock initialization failed");
	before = clock;
	CHECK(!receive_clock_init(&clock, 0, 8000, 480, 4) &&
	      !receive_clock_init(&clock, 80, 8000, 0, 4) &&
	      !receive_clock_init(&clock, 1, 1000000000, 480, 4) &&
	      memcmp(&clock, &before, sizeof(clock)) == 0, "invalid initialization changed owner");
	CHECK(receive_clock_wait_ns(&clock, 0) == 0 && receive_clock_take(&clock, 0) == FRAME_CLOCK_OK,
	      "clock failed to seed at nominal rate");
	struct rtp_playout_status status = { .stream_active = true, .ssrc = 1, .source_timestamp = 80,
		.clock_lead_samples = 480 };
	CHECK(receive_clock_observe(&clock, &status), "initial observation failed");
	CHECK(receive_clock_take(&clock, 10000000) == FRAME_CLOCK_OK, "second frame failed");
	status.source_timestamp += 80;
	status.clock_lead_samples = 880;
	CHECK(receive_clock_observe(&clock, &status), "frequency observation failed");
	uint64_t interval = clock.frames.interval_ns;
	status.clock_lead_samples = 0;
	for (unsigned i = 0; i < 100; ++i) {
		uint64_t now = clock.frames.next_deadline_ns;
		CHECK(receive_clock_take(&clock, now) == FRAME_CLOCK_OK &&
		      receive_clock_observe(&clock, &status) && clock.frames.interval_ns == interval,
		      "missing input changed mean clock or froze TX");
	}
	uint64_t now = clock.frames.next_deadline_ns + 100000000;
	unsigned taken = 0;
	while (receive_clock_take(&clock, now) == FRAME_CLOCK_OK) taken++;
	CHECK(taken == 4 && receive_clock_wait_ns(&clock, now) > 0 &&
	      receive_clock_take(&clock, 0) == FRAME_CLOCK_NOT_DUE, "backlog or rollback renewed credit");
	status.ssrc++;
	CHECK(receive_clock_observe(&clock, &status) && clock.frames.interval_ns == 10000000 &&
	      receive_clock_take(&clock, now) == FRAME_CLOCK_NOT_DUE, "SSRC restart renewed burst allowance");
	receive_clock_reset(&clock);
	CHECK(receive_clock_take(&clock, UINT64_MAX) == FRAME_CLOCK_OVERRUN &&
	      receive_clock_wait_ns(&clock, UINT64_MAX) == UINT64_MAX &&
	      receive_clock_take(&clock, 0) == FRAME_CLOCK_OVERRUN, "overflow did not latch");
	receive_clock_reset(&clock);
	CHECK(receive_clock_take(&clock, 0) == FRAME_CLOCK_OK && !clock.source_valid,
	      "new call inherited the retired epoch");
	return true;
}

static bool accepted_source_time(void)
{
	struct rtp_jitter_slot slots[16];
	struct rtp_receiver rx;
	const struct rtp_receiver_config config = { .prefill_samples = 2 };
	uint8_t bytes[4] = { 1, 2, 3, 4 }, output[4];
	struct rtp_packet_view packet = {
		.header = { .ssrc = 1, .timestamp = UINT32_MAX - 3 }, .payload = bytes, .payload_size = 4,
	};
	CHECK(rtp_receiver_init(&rx, &config, slots, ARRAY_LENGTH(slots)) &&
	      rtp_receiver_push(&rx, &packet) == RTP_RECEIVE_ACCEPTED,
	      "source-time fixture failed");
	struct rtp_playout_status status = rtp_receiver_playout_status(&rx, 4);
	CHECK(status.source_timestamp == 0 && status.clock_lead_samples == 6,
	      "wrapped source time or prefill lead changed");
	CHECK(rtp_receiver_play(&rx, output, NULL, 2, NULL) &&
	      rtp_receiver_play(&rx, output, NULL, 4, NULL) && memcmp(output, bytes, 4) == 0 &&
	      rtp_receiver_play(&rx, output, NULL, 4, NULL), "source-time playout failed");
	status = rtp_receiver_playout_status(&rx, 4);
	CHECK(status.source_timestamp == 0 && status.clock_lead_samples == 0,
	      "concealment fabricated source-clock progress");
	CHECK(rtp_receiver_push(&rx, &packet) == RTP_RECEIVE_LATE, "old packet was not late");
	packet.header.timestamp = 1000;
	CHECK(rtp_receiver_push(&rx, &packet) == RTP_RECEIVE_TOO_EARLY &&
	      rtp_receiver_playout_status(&rx, 4).source_timestamp == 0,
	      "rejected media changed accepted source time");
	packet.header.ssrc++;
	packet.header.timestamp = 200;
	CHECK(rtp_receiver_push(&rx, &packet) == RTP_RECEIVE_ACCEPTED &&
	      rtp_receiver_playout_status(&rx, 4).source_timestamp == 204 &&
	      rtp_receiver_playout_status(&rx, 4).clock_lead_samples == 6,
	      "new stream inherited the old source cursor");
	return true;
}

int main(void)
{
	if (!boundaries() || !accepted_source_time()) return 1;
	puts("[       OK ] receive clock bounds and epochs");
	const int offsets[] = { 0, -50, 50, -100, 100, -500, 500, -1000, 1000 };
	for (size_t i = 0; i < ARRAY_LENGTH(offsets); ++i) {
		if (!stream_case(offsets[i], false, false, false, false, 80, 80) ||
		    !stream_case(offsets[i], true, false, false, false, 80, 80) ||
		    !stream_case(offsets[i], true, true, false, false, 80, 80)) return 1;
		printf("[       OK ] receive clock one-hour drift/reorder/step %dppm\n", offsets[i]);
	}
	const int wide[] = { 333333, 100000, -100000, -250000 };
	for (size_t i = 0; i < ARRAY_LENGTH(wide); ++i) {
		if (!stream_case(wide[i], false, false, false, false, 80, 80)) return 1;
		printf("[       OK ] receive clock one-hour wide pull %dppm\n", wide[i]);
	}
	if (!stream_case(0, true, false, true, false, 80, 80) || !stream_case(0, true, false, false, true, 80, 80)) return 1;
	puts("[       OK ] receive clock exact loss and bounded backlog");
	const size_t frames[] = { 8, 160, 320, 960, 1600, 2032 };
	for (size_t i = 0; i < ARRAY_LENGTH(frames); ++i) {
		if (!stream_case(-1000, true, false, false, false, frames[i], 80) ||
		    !stream_case(1000, true, false, false, false, frames[i], 80)) return 1;
		printf("[       OK ] receive clock packetization %zu samples\n", frames[i]);
	}
	const size_t packets[][2] = { {80, 160}, {8, 960}, {160, 8} };
	for (size_t i = 0; i < ARRAY_LENGTH(packets); ++i) {
		if (!stream_case(-1000, true, false, false, false, packets[i][0], packets[i][1]) ||
		    !stream_case(1000, true, false, false, false, packets[i][0], packets[i][1])) return 1;
		printf("[       OK ] receive clock local/remote blocks %zu/%zu\n", packets[i][0], packets[i][1]);
	}
	return 0;
}
