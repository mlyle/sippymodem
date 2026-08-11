// SPDX-License-Identifier: BSD-3-Clause

#include <rtp_stream.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

enum {
	TEST_SLOTS = 16,
	TEST_WORKSPACE = 8,
	TEST_RECORDINGS = 8,
};

struct recording_capture {
	enum sippymodem_recording_stream streams[TEST_RECORDINGS];
	size_t counts[TEST_RECORDINGS];
	uint8_t data[TEST_RECORDINGS][16];
	size_t calls;
};

static void capture_recording(
	void *context,
	const struct sippymodem_recording_block *block)
{
	struct recording_capture *capture = context;
	size_t bytes = block->count;

	if (capture->calls >= ARRAY_LENGTH(capture->streams))
		return;
	if (block->format == SIPPYMODEM_RECORD_S16_NATIVE)
		bytes *= sizeof(int16_t);
	if (bytes > sizeof(capture->data[0]))
		bytes = sizeof(capture->data[0]);
	capture->streams[capture->calls] = block->stream;
	capture->counts[capture->calls] = block->count;
	memcpy(capture->data[capture->calls], block->data, bytes);
	capture->calls++;
}

static bool init_stream(struct sippymodem_rtp_stream *stream,
			struct sippymodem_rtp_jitter_slot *slots,
			uint8_t *workspace,
			struct recording_capture *capture,
			size_t prefill_samples)
{
	const struct sippymodem_rtp_stream_config config = {
		.codec = SIPPYMODEM_CODEC_PCMU,
		.payload_type = 0,
		.prefill_samples = prefill_samples,
		.transmit_sequence = 0x1234,
		.transmit_timestamp = 0x56789abc,
		.transmit_ssrc = 0xdef01234,
		.recorder = {
			.context = capture,
			.write = capture_recording,
		},
	};

	return sippymodem_rtp_stream_init(stream, &config, slots, TEST_SLOTS,
					  workspace, TEST_WORKSPACE);
}

static bool test_receive_playout_and_recording(void)
{
	static const uint8_t payload[] = { 0xff, 0xfe, 0x7e, 0x80 };
	static const int16_t expected[] = { 0, 0, 0, 8 };
	struct sippymodem_rtp_jitter_slot slots[TEST_SLOTS];
	struct sippymodem_rtp_stream stream;
	struct recording_capture capture = { 0 };
	struct sippymodem_rtp_header header = {
		.payload_type = 0,
		.sequence = 10,
		.timestamp = 200,
		.ssrc = 9,
	};
	struct sippymodem_rtp_playout_report report;
	uint8_t workspace[TEST_WORKSPACE];
	uint8_t packet[32];
	int16_t samples[ARRAY_LENGTH(payload)];
	size_t packet_size;
	enum sippymodem_rtp_result packet_result;

	CHECK(init_stream(&stream, slots, workspace, &capture, 2),
	      "stream init failed");
	CHECK(sippymodem_rtp_packet_write(packet, sizeof(packet), &header,
					   payload, sizeof(payload),
					   &packet_size) == SIPPYMODEM_RTP_OK,
	      "failed to build receive packet");
	CHECK(sippymodem_rtp_stream_receive(&stream, packet, packet_size,
				     &packet_result) ==
		      SIPPYMODEM_RTP_RECEIVE_ACCEPTED &&
		      packet_result == SIPPYMODEM_RTP_OK,
	      "stream rejected receive packet");
	CHECK(capture.calls == 0, "arrival was recorded before playout");
	CHECK(sippymodem_rtp_stream_play(&stream, samples,
				  ARRAY_LENGTH(samples), &report) ==
		      SIPPYMODEM_MEDIA_OK,
	      "stream playout failed");
	CHECK(memcmp(samples, expected, sizeof(expected)) == 0 &&
		      report.prefill_samples == 2 &&
		      report.received_samples == 2,
	      "stream playout samples are wrong");
	CHECK(capture.calls == 2 &&
		      capture.streams[0] ==
			      SIPPYMODEM_RECORD_RX_RTP_PAYLOAD_PLAYOUT &&
		      capture.streams[1] == SIPPYMODEM_RECORD_RX_LINE_PCM &&
		      capture.counts[0] == ARRAY_LENGTH(payload) &&
		      memcmp(capture.data[0],
			     (uint8_t[]){ 0xff, 0xff, 0xff, 0xfe },
			     sizeof(payload)) == 0,
	      "receive recordings are not in playout order");
	return true;
}

static bool test_transmit_timeline_and_recording(void)
{
	static const int16_t samples[] = { 0, 1000, -1000, 0 };
	static const uint8_t expected[] = { 0xff, 0xce, 0x4e, 0xff };
	struct sippymodem_rtp_jitter_slot slots[TEST_SLOTS];
	struct sippymodem_rtp_stream stream;
	struct recording_capture capture = { 0 };
	struct sippymodem_rtp_packet_view view;
	uint8_t workspace[TEST_WORKSPACE];
	uint8_t packet[32];
	size_t packet_size;

	CHECK(init_stream(&stream, slots, workspace, &capture, 0),
	      "stream init failed");
	CHECK(sippymodem_rtp_stream_write(&stream, samples,
				   ARRAY_LENGTH(samples), packet,
				   sizeof(packet), &packet_size) ==
		      SIPPYMODEM_MEDIA_OK,
	      "stream transmit failed");
	CHECK(sippymodem_rtp_packet_parse(packet, packet_size, &view) ==
		      SIPPYMODEM_RTP_OK,
	      "transmitted packet did not parse");
	CHECK(view.header.sequence == 0x1234 &&
		      view.header.timestamp == 0x56789abc &&
		      view.header.ssrc == 0xdef01234 &&
		      view.payload_size == sizeof(expected) &&
		      memcmp(view.payload, expected, sizeof(expected)) == 0,
	      "transmitted RTP contents are wrong");
	CHECK(capture.calls == 2 &&
		      capture.streams[0] == SIPPYMODEM_RECORD_TX_LINE_PCM &&
		      capture.streams[1] ==
			      SIPPYMODEM_RECORD_TX_RTP_PAYLOAD_PREPARED &&
		      memcmp(capture.data[1], expected, sizeof(expected)) == 0,
	      "transmit recording stages are wrong");

	CHECK(sippymodem_rtp_stream_write(&stream, samples, 2, packet,
				   sizeof(packet), &packet_size) ==
		      SIPPYMODEM_MEDIA_OK &&
		      sippymodem_rtp_packet_parse(packet, packet_size, &view) ==
			      SIPPYMODEM_RTP_OK,
	      "second transmit failed");
	CHECK(view.header.sequence == 0x1235 &&
		      view.header.timestamp == 0x56789ac0 &&
		      stream.stats.transmit_packets == 2 &&
		      stream.stats.transmit_samples == 6,
	      "transmit timeline did not advance exactly");
	return true;
}

static bool test_validation_is_transactional(void)
{
	struct sippymodem_rtp_jitter_slot slots[TEST_SLOTS];
	struct sippymodem_rtp_stream stream;
	struct recording_capture capture = { 0 };
	struct sippymodem_rtp_stream_config invalid = {
		.codec = SIPPYMODEM_CODEC_CLEARMODE,
		.payload_type = 96,
	};
	uint8_t workspace[TEST_WORKSPACE];
	uint8_t packet[16] = { 0 };
	const int16_t samples[] = { 0, 1, 2, 3, 4 };
	int16_t playout[TEST_WORKSPACE + 1];
	size_t packet_size = 999;
	uint16_t sequence;

	CHECK(!sippymodem_rtp_stream_init(&stream, &invalid, slots, TEST_SLOTS,
					  workspace, sizeof(workspace)),
	      "stream accepted CLEARMODE as linear audio");
	CHECK(init_stream(&stream, slots, workspace, &capture, 0),
	      "stream init failed");
	sequence = stream.transmit_header.sequence;
	CHECK(sippymodem_rtp_stream_write(&stream, samples,
				   ARRAY_LENGTH(samples), packet,
				   sizeof(packet), &packet_size) ==
		      SIPPYMODEM_MEDIA_OUTPUT_TOO_SMALL,
	      "stream accepted a short packet buffer");
	CHECK(packet_size == 999 && stream.transmit_header.sequence == sequence &&
		      capture.calls == 0,
	      "failed transmit changed state or emitted a recording");
	CHECK(sippymodem_rtp_stream_play(&stream, playout,
				  TEST_WORKSPACE + 1, NULL) ==
		      SIPPYMODEM_MEDIA_INVALID_ARGUMENT,
	      "stream accepted playout beyond its workspace");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "receive playout and recording",
		  test_receive_playout_and_recording },
		{ "transmit timeline and recording",
		  test_transmit_timeline_and_recording },
		{ "validation is transactional",
		  test_validation_is_transactional },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		printf("[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			printf("[       OK ] %s\n", tests[index].name);
		else {
			printf("[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0 ? 0 : 1;
}
