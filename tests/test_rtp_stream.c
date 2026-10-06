// SPDX-License-Identifier: BSD-3-Clause

#include <rtp_stream.h>

#include <g711.h>

#include "test.h"
#include "rtp_state_fixture.h"

#include <stdint.h>
#include <string.h>

enum {
	TEST_SLOTS = 16,
	TEST_WORKSPACE = 8,
	TEST_RECORDINGS = 8,
};

struct recording_capture {
	enum recording_stream streams[TEST_RECORDINGS];
	size_t counts[TEST_RECORDINGS];
	uint8_t data[TEST_RECORDINGS][16];
	size_t calls;
};

static void capture_recording(
	void *context,
	const struct recording_block *block)
{
	struct recording_capture *capture = context;
	size_t bytes = block->count;

	if (capture->calls >= ARRAY_LENGTH(capture->streams))
		return;
	if (block->format == RECORD_S16_NATIVE)
		bytes *= sizeof(int16_t);
	if (bytes > sizeof(capture->data[0]))
		bytes = sizeof(capture->data[0]);
	capture->streams[capture->calls] = block->stream;
	capture->counts[capture->calls] = block->count;
	memcpy(capture->data[capture->calls], block->data, bytes);
	capture->calls++;
}

static bool init_stream_codec(
	struct rtp_stream *stream, struct rtp_jitter_slot *slots,
	uint8_t *workspace, struct recording_capture *capture,
	size_t prefill_samples, enum media_codec codec)
{
	const struct rtp_stream_config config = {
		.codec = codec,
		.payload_type = (uint8_t)media_codec_default_payload_type(codec),
		.prefill_samples = prefill_samples,
		.transmit_sequence = 0x1234,
		.transmit_timestamp = 0x56789abc,
		.transmit_ssrc = 0xdef01234,
		.recorder = {
			.context = capture,
			.write = capture_recording,
		},
	};

	return rtp_stream_init(stream, &config, slots, TEST_SLOTS,
					  workspace, TEST_WORKSPACE);
}

static bool init_stream(struct rtp_stream *stream,
			struct rtp_jitter_slot *slots,
			uint8_t *workspace,
			struct recording_capture *capture,
			size_t prefill_samples)
{
	return init_stream_codec(
		stream, slots, workspace, capture,
		prefill_samples, MEDIA_CODEC_PCMU);
}

static bool stream_states_equal(
	const struct rtp_stream *left, const struct rtp_stream *right)
{
	return rtp_receiver_states_equal(&left->receiver, &right->receiver) &&
	       left->codec == right->codec &&
	       left->recorder.context == right->recorder.context &&
	       left->recorder.write == right->recorder.write &&
	       left->recorder.packet == right->recorder.packet &&
	       left->receive_workspace == right->receive_workspace &&
	       left->receive_workspace_size == right->receive_workspace_size &&
	       left->transmit_header.payload_type == right->transmit_header.payload_type &&
	       left->transmit_header.marker == right->transmit_header.marker &&
	       left->transmit_header.sequence == right->transmit_header.sequence &&
	       left->transmit_header.timestamp == right->transmit_header.timestamp &&
	       left->transmit_header.ssrc == right->transmit_header.ssrc &&
	       left->stats.transmit_packets == right->stats.transmit_packets &&
	       left->stats.transmit_samples == right->stats.transmit_samples;
}

static bool test_receive_playout_and_recording(void)
{
	static const uint8_t payload[] = { 0x7f, 0xfe, 0x7e, 0x80 };
	static const uint8_t expected_octets[] = { 0xff, 0xff, 0x7f, 0xfe };
	static const int16_t expected[] = { 0, 0, 0, 8 };
	struct rtp_jitter_slot slots[TEST_SLOTS];
	struct rtp_stream stream;
	struct recording_capture capture = { 0 };
	struct rtp_header header = {
		.payload_type = 0,
		.sequence = 10,
		.timestamp = 200,
		.ssrc = 9,
	};
	struct rtp_playout_report report;
	uint8_t workspace[TEST_WORKSPACE];
	uint8_t packet[32];
	uint8_t source_octets[ARRAY_LENGTH(payload)];
	bool received_mask[ARRAY_LENGTH(payload)];
	int16_t samples[ARRAY_LENGTH(payload)];
	size_t packet_size;
	enum rtp_result packet_result;

	CHECK(init_stream(&stream, slots, workspace, &capture, 2),
	      "stream init failed");
	CHECK(rtp_packet_write(packet, sizeof(packet), &header,
					   payload, sizeof(payload),
					   &packet_size) == RTP_OK,
	      "failed to build receive packet");
	CHECK(rtp_stream_receive(&stream, packet, packet_size,
				     &packet_result) ==
		      RTP_RECEIVE_ACCEPTED &&
		      packet_result == RTP_OK,
	      "stream rejected receive packet");
	CHECK(capture.calls == 0, "arrival was recorded before playout");
	struct rtp_stream before = stream;
	CHECK(rtp_stream_play(&stream, samples, source_octets, (bool *)samples,
		ARRAY_LENGTH(samples), &report) == MEDIA_INVALID_ARGUMENT &&
	      rtp_stream_play(&stream, samples, source_octets, (bool *)source_octets,
		ARRAY_LENGTH(samples), &report) == MEDIA_INVALID_ARGUMENT &&
	      stream_states_equal(&stream, &before) && capture.calls == 0,
	      "overlapping provenance output consumed playout");
	CHECK(rtp_stream_play(&stream, samples, source_octets, received_mask,
				  ARRAY_LENGTH(samples), &report) ==
		      MEDIA_OK,
	      "stream playout failed");
	CHECK(memcmp(samples, expected, sizeof(expected)) == 0 &&
		      memcmp(source_octets, expected_octets,
			     sizeof(expected_octets)) == 0 &&
		      report.prefill_samples == 2 &&
		      report.received_samples == 2 &&
		      memcmp(received_mask, (bool[]){ false, false, true, true }, sizeof(received_mask)) == 0,
	      "stream playout samples or codewords are wrong");
	CHECK(capture.calls == 2 &&
		      capture.streams[0] ==
			      RECORD_RX_RTP_PAYLOAD_PLAYOUT &&
		      capture.streams[1] == RECORD_RX_LINE_PCM &&
		      capture.counts[0] == ARRAY_LENGTH(payload) &&
		      memcmp(capture.data[0],
			     expected_octets,
			     sizeof(payload)) == 0,
	      "receive recordings are not in playout order");
	return true;
}

static bool test_transmit_timeline_and_recording(void)
{
	static const int16_t samples[] = { 0, 1000, -1000, 0 };
	static const uint8_t expected[] = { 0xff, 0xce, 0x4e, 0xff };
	struct rtp_jitter_slot slots[TEST_SLOTS];
	struct rtp_stream stream;
	struct recording_capture capture = { 0 };
	struct rtp_packet_view view;
	uint8_t workspace[TEST_WORKSPACE];
	uint8_t packet[32];
	size_t packet_size;

	CHECK(init_stream(&stream, slots, workspace, &capture, 0),
	      "stream init failed");
	CHECK(rtp_stream_write(&stream, samples,
				   ARRAY_LENGTH(samples), packet,
				   sizeof(packet), &packet_size) ==
		      MEDIA_OK,
	      "stream transmit failed");
	CHECK(rtp_packet_parse(packet, packet_size, &view) ==
		      RTP_OK,
	      "transmitted packet did not parse");
	CHECK(view.header.sequence == 0x1234 &&
		      view.header.timestamp == 0x56789abc &&
		      view.header.ssrc == 0xdef01234 &&
		      view.payload_size == sizeof(expected) &&
		      memcmp(view.payload, expected, sizeof(expected)) == 0,
	      "transmitted RTP contents are wrong");
	CHECK(capture.calls == 2 &&
		      capture.streams[0] == RECORD_TX_LINE_PCM &&
		      capture.streams[1] ==
			      RECORD_TX_RTP_PAYLOAD_PREPARED &&
		      memcmp(capture.data[1], expected, sizeof(expected)) == 0,
	      "transmit recording stages are wrong");

	CHECK(rtp_stream_write(&stream, samples, 2, packet,
				   sizeof(packet), &packet_size) ==
		      MEDIA_OK &&
		      rtp_packet_parse(packet, packet_size, &view) ==
			      RTP_OK,
	      "second transmit failed");
	CHECK(view.header.sequence == 0x1235 &&
		      view.header.timestamp == 0x56789ac0 &&
		      stream.stats.transmit_packets == 2 &&
		      stream.stats.transmit_samples == 6,
	      "transmit timeline did not advance exactly");
	return true;
}

static bool test_exact_g711_transmit(void)
{
	static const uint8_t codewords[] = { 0x7f, 0xff, 0x00, 0x80 };
	static const uint8_t alaw_codewords[] = { 0x55, 0xd5, 0x00, 0x80 };
	const int16_t decoded[] = {
		g711_decode_pcmu(codewords[0]),
		g711_decode_pcmu(codewords[1]),
		g711_decode_pcmu(codewords[2]),
		g711_decode_pcmu(codewords[3]),
	};
	int16_t mismatched[ARRAY_LENGTH(decoded)];
	int16_t alaw_decoded[ARRAY_LENGTH(alaw_codewords)];
	struct rtp_jitter_slot slots[TEST_SLOTS];
	struct rtp_stream stream;
	struct recording_capture capture = { 0 };
	struct rtp_packet_view view;
	uint8_t workspace[TEST_WORKSPACE];
	uint8_t packet[32];
	size_t packet_size = 0;
	uint16_t sequence;
	uint32_t timestamp;

	CHECK(init_stream(&stream, slots, workspace, &capture, 0),
	      "exact G.711 stream init failed");
	CHECK(rtp_stream_write_g711(
		      &stream, codewords, decoded, ARRAY_LENGTH(codewords),
		      packet, sizeof(packet), &packet_size) == MEDIA_OK &&
		      rtp_packet_parse(packet, packet_size, &view) == RTP_OK &&
		      view.payload_size == sizeof(codewords) &&
		      memcmp(view.payload, codewords, sizeof(codewords)) == 0,
	      "exact G.711 codewords were canonicalized or changed");
	CHECK(capture.calls == 2 &&
		      capture.streams[0] == RECORD_TX_LINE_PCM &&
		      capture.streams[1] == RECORD_TX_RTP_PAYLOAD_PREPARED &&
		      memcmp(capture.data[0], decoded, sizeof(decoded)) == 0 &&
		      memcmp(capture.data[1], codewords, sizeof(codewords)) == 0,
	      "exact G.711 recording evidence disagrees with the packet");

	memcpy(mismatched, decoded, sizeof(mismatched));
	mismatched[0]++;
	sequence = stream.transmit_header.sequence;
	timestamp = stream.transmit_header.timestamp;
	packet_size = 999;
	CHECK(rtp_stream_write_g711(
		      &stream, codewords, mismatched, ARRAY_LENGTH(codewords),
		      packet, sizeof(packet), &packet_size) ==
			      MEDIA_INVALID_ARGUMENT &&
		      packet_size == 999 && capture.calls == 2 &&
		      stream.transmit_header.sequence == sequence &&
		      stream.transmit_header.timestamp == timestamp &&
		      stream.stats.transmit_packets == 1 &&
		      stream.stats.transmit_samples == ARRAY_LENGTH(codewords),
	      "mismatched G.711 evidence changed transmit state");

	for (size_t index = 0; index < ARRAY_LENGTH(alaw_codewords); ++index)
		alaw_decoded[index] = g711_decode_pcma(alaw_codewords[index]);
	capture = (struct recording_capture){ 0 };
	CHECK(init_stream_codec(
		      &stream, slots, workspace, &capture, 0,
		      MEDIA_CODEC_PCMA) &&
		      rtp_stream_write_g711(
			      &stream, alaw_codewords, alaw_decoded,
			      ARRAY_LENGTH(alaw_codewords), packet, sizeof(packet),
			      &packet_size) == MEDIA_OK &&
		      rtp_packet_parse(packet, packet_size, &view) == RTP_OK &&
		      view.header.payload_type == 8 &&
		      view.payload_size == sizeof(alaw_codewords) &&
		      memcmp(view.payload, alaw_codewords,
			     sizeof(alaw_codewords)) == 0 &&
		      capture.calls == 2 &&
		      memcmp(capture.data[0], alaw_decoded,
			     sizeof(alaw_decoded)) == 0,
	      "exact PCMA codewords or their evidence changed");
	return true;
}

static bool test_validation_is_transactional(void)
{
	struct rtp_jitter_slot slots[TEST_SLOTS];
	struct rtp_stream stream;
	struct recording_capture capture = { 0 };
	struct rtp_stream_config invalid = {
		.codec = MEDIA_CODEC_CLEARMODE,
		.payload_type = 96,
	};
	uint8_t workspace[TEST_WORKSPACE];
	uint8_t packet[16] = { 0 };
	const int16_t samples[] = { 0, 1, 2, 3, 4 };
	int16_t playout[TEST_WORKSPACE + 1];
	size_t packet_size = 999;
	uint16_t sequence;

	CHECK(!rtp_stream_init(&stream, &invalid, slots, TEST_SLOTS,
					  workspace, sizeof(workspace)),
	      "stream accepted CLEARMODE as linear audio");
	CHECK(init_stream(&stream, slots, workspace, &capture, 0),
	      "stream init failed");
	sequence = stream.transmit_header.sequence;
	CHECK(rtp_stream_write(&stream, samples,
				   ARRAY_LENGTH(samples), packet,
				   sizeof(packet), &packet_size) ==
		      MEDIA_OUTPUT_TOO_SMALL,
	      "stream accepted a short packet buffer");
	CHECK(packet_size == 999 && stream.transmit_header.sequence == sequence &&
		      capture.calls == 0,
	      "failed transmit changed state or emitted a recording");
	CHECK(rtp_stream_play(&stream, playout, NULL, NULL,
				  TEST_WORKSPACE + 1, NULL) ==
		      MEDIA_INVALID_ARGUMENT,
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
		{ "exact G.711 transmit", test_exact_g711_transmit },
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
