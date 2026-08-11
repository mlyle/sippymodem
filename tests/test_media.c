// SPDX-License-Identifier: BSD-3-Clause

#include <media.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

enum {
	CAPTURE_BLOCKS = 4,
	CAPTURE_BYTES = 64,
};

struct captured_block {
	enum recording_stream stream;
	enum recording_format format;
	unsigned int sample_rate;
	size_t count;
	uint8_t data[CAPTURE_BYTES];
};

struct recording_capture {
	struct captured_block blocks[CAPTURE_BLOCKS];
	size_t count;
};

static void capture_recording(
	void *context,
	const struct recording_block *block)
{
	struct recording_capture *capture = context;
	struct captured_block *saved;
	size_t bytes = block->count;

	if (capture->count >= ARRAY_LENGTH(capture->blocks))
		return;
	saved = &capture->blocks[capture->count++];
	saved->stream = block->stream;
	saved->format = block->format;
	saved->sample_rate = block->sample_rate;
	saved->count = block->count;
	if (block->format == RECORD_S16_NATIVE)
		bytes *= sizeof(int16_t);
	if (bytes > sizeof(saved->data))
		bytes = sizeof(saved->data);
	memcpy(saved->data, block->data, bytes);
}

static bool check_recording(const struct captured_block *block,
			    enum recording_stream stream,
			    enum recording_format format,
			    const void *data, size_t count)
{
	size_t bytes = count;

	if (format == RECORD_S16_NATIVE)
		bytes *= sizeof(int16_t);
	CHECK(block->stream == stream, "wrong recording stream");
	CHECK(block->format == format, "wrong recording format");
	CHECK(block->sample_rate == MEDIA_SAMPLE_RATE,
	      "wrong recording sample rate");
	CHECK(block->count == count, "wrong recording element count");
	CHECK(bytes <= sizeof(block->data) &&
		      memcmp(block->data, data, bytes) == 0,
	      "wrong recorded data");
	return true;
}

static bool test_codec_metadata(void)
{
	CHECK(strcmp(media_codec_name(MEDIA_CODEC_PCMU),
		     "PCMU/8000/1") == 0,
	      "wrong PCMU name");
	CHECK(strcmp(media_codec_name(MEDIA_CODEC_PCMA),
		     "PCMA/8000/1") == 0,
	      "wrong PCMA name");
	CHECK(media_codec_default_payload_type(MEDIA_CODEC_PCMU) == 0 &&
		      media_codec_default_payload_type(
			      MEDIA_CODEC_PCMA) == 8 &&
		      media_codec_default_payload_type(
			      MEDIA_CODEC_CLEARMODE) == 96,
	      "wrong default RTP payload type");
	CHECK(media_codec_silence_octet(MEDIA_CODEC_PCMU) == 0xff &&
		      media_codec_silence_octet(
			      MEDIA_CODEC_PCMA) == 0xd5 &&
		      media_codec_silence_octet(
			      MEDIA_CODEC_CLEARMODE) == -1,
	      "wrong codec silence octet");
	CHECK(media_codec_name((enum media_codec)99) == NULL &&
		      media_codec_default_payload_type(
			      (enum media_codec)99) == -1,
	      "accepted an unknown codec");
	CHECK(strcmp(recording_stream_name(
			     RECORD_RX_RTP_PAYLOAD_ARRIVAL),
		     "rx.rtp_payload.arrival") == 0 &&
		      strcmp(recording_stream_name(
			     RECORD_RX_RTP_PAYLOAD_PLAYOUT),
		     "rx.rtp_payload.playout") == 0 &&
		      strcmp(recording_stream_name(
			     RECORD_TX_MODEM_PCM),
			     "tx.modem_pcm") == 0 &&
		      strcmp(recording_stream_name(
			     RECORD_TX_RTP_PAYLOAD_SENT),
			     "tx.rtp_payload.sent") == 0 &&
		      recording_stream_name(
			      (enum recording_stream)99) == NULL,
	      "recording stream names are not stable");
	return true;
}

static bool test_decode_with_recording(void)
{
	static const uint8_t pcmu[] = { 0xff, 0xfe, 0x7e, 0x80 };
	static const int16_t expected[] = { 0, 8, -8, 32124 };
	struct recording_capture capture = { 0 };
	struct media_recorder recorder = {
		.context = &capture,
		.write = capture_recording,
	};
	int16_t samples[ARRAY_LENGTH(pcmu)];

	CHECK(media_decode(MEDIA_CODEC_PCMU, pcmu,
				       ARRAY_LENGTH(pcmu), samples,
				       ARRAY_LENGTH(samples), &recorder) ==
		      MEDIA_OK,
	      "PCMU decode failed");
	CHECK(memcmp(samples, expected, sizeof(expected)) == 0,
	      "PCMU samples are wrong");
	CHECK(capture.count == 2, "decode emitted the wrong block count");
	return check_recording(&capture.blocks[0],
			       RECORD_RX_RTP_PAYLOAD_PLAYOUT,
			       RECORD_OCTETS, pcmu,
			       ARRAY_LENGTH(pcmu)) &&
	       check_recording(&capture.blocks[1],
			       RECORD_RX_LINE_PCM,
			       RECORD_S16_NATIVE, expected,
			       ARRAY_LENGTH(expected));
}

static bool test_encode_with_recording(void)
{
	static const int16_t samples[] = { 8, -8, 1000, -1000 };
	static const uint8_t expected[] = { 0xd5, 0x55, 0xfa, 0x7a };
	struct recording_capture capture = { 0 };
	struct media_recorder recorder = {
		.context = &capture,
		.write = capture_recording,
	};
	uint8_t payload[ARRAY_LENGTH(samples)];

	CHECK(media_encode(MEDIA_CODEC_PCMA, samples,
				       ARRAY_LENGTH(samples), payload,
				       ARRAY_LENGTH(payload), &recorder) ==
		      MEDIA_OK,
	      "PCMA encode failed");
	CHECK(memcmp(payload, expected, sizeof(expected)) == 0,
	      "PCMA payload is wrong");
	CHECK(capture.count == 2, "encode emitted the wrong block count");
	return check_recording(&capture.blocks[0],
			       RECORD_TX_LINE_PCM,
			       RECORD_S16_NATIVE, samples,
			       ARRAY_LENGTH(samples)) &&
	       check_recording(&capture.blocks[1],
			       RECORD_TX_RTP_PAYLOAD_PREPARED,
			       RECORD_OCTETS, expected,
			       ARRAY_LENGTH(expected));
}

static bool test_validation(void)
{
	const uint8_t input[] = { 0xff, 0xfe };
	int16_t output[] = { 123, 456 };
	int16_t original[ARRAY_LENGTH(output)];
	struct recording_capture capture = { 0 };
	struct media_recorder recorder = {
		.context = &capture,
		.write = capture_recording,
	};

	memcpy(original, output, sizeof(original));
	CHECK(media_decode(MEDIA_CODEC_PCMU, input, 2,
				       output, 1, &recorder) ==
		      MEDIA_OUTPUT_TOO_SMALL,
	      "decode accepted a short output buffer");
	CHECK(memcmp(output, original, sizeof(output)) == 0 &&
		      capture.count == 0,
	      "failed decode produced samples or recordings");
	CHECK(media_decode(MEDIA_CODEC_CLEARMODE, input, 2,
				       output, 2, &recorder) ==
		      MEDIA_NO_LINEAR_MAPPING,
	      "CLEARMODE was treated as linear PCM");
	CHECK(media_decode(MEDIA_CODEC_PCMU, NULL, 1,
				       output, 2, NULL) ==
		      MEDIA_INVALID_ARGUMENT,
	      "decode accepted a null input");
	CHECK(!recording_emit(&recorder,
					 RECORD_RX_LINE_PCM,
					 RECORD_S16_NATIVE, 0,
					 output, 2),
	      "recorder accepted a zero sample rate");
	CHECK(recording_emit(NULL,
					RECORD_RX_LINE_PCM,
					RECORD_S16_NATIVE, 8000,
					output, 2),
	      "disabled recorder rejected valid data");
	CHECK(recording_emit(&recorder,
					RECORD_RX_LINE_PCM,
					RECORD_S16_NATIVE, 8000,
					NULL, 0) && capture.count == 0,
	      "empty recording emitted a callback");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "codec metadata", test_codec_metadata },
		{ "decode with recording", test_decode_with_recording },
		{ "encode with recording", test_encode_with_recording },
		{ "validation", test_validation },
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
