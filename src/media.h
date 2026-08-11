// SPDX-License-Identifier: BSD-3-Clause

#ifndef MEDIA_H
#define MEDIA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	MEDIA_SAMPLE_RATE = 8000,
};

enum media_codec {
	MEDIA_CODEC_PCMU,
	MEDIA_CODEC_PCMA,
	MEDIA_CODEC_CLEARMODE,
};

enum recording_stream {
	RECORD_RX_RTP_PAYLOAD_ARRIVAL,
	RECORD_RX_RTP_PAYLOAD_PLAYOUT,
	RECORD_RX_LINE_PCM,
	RECORD_RX_MODEM_PCM,
	RECORD_TX_MODEM_PCM,
	RECORD_TX_LINE_PCM,
	RECORD_TX_RTP_PAYLOAD_PREPARED,
	RECORD_TX_RTP_PAYLOAD_SENT,
};

enum recording_format {
	RECORD_OCTETS,
	RECORD_S16_NATIVE,
};

struct recording_block {
	enum recording_stream stream;
	enum recording_format format;
	unsigned int sample_rate;
	const void *data;
	size_t count;
};

/* The callback is synchronous.  Both the block and its data are borrowed for
 * the duration of the call; a recorder that needs them later must copy them. */
struct media_recorder {
	void *context;
	void (*write)(void *context,
		      const struct recording_block *block);
};

bool recording_emit(
	const struct media_recorder *recorder,
	enum recording_stream stream,
	enum recording_format format,
	unsigned int sample_rate,
	const void *data,
	size_t count);
const char *recording_stream_name(
	enum recording_stream stream);

enum media_result {
	MEDIA_OK,
	MEDIA_INVALID_ARGUMENT,
	MEDIA_OUTPUT_TOO_SMALL,
	MEDIA_NO_LINEAR_MAPPING,
};

enum media_result media_decode(
	enum media_codec codec,
	const uint8_t *payload,
	size_t payload_count,
	int16_t *samples,
	size_t sample_capacity,
	const struct media_recorder *recorder);
enum media_result media_encode(
	enum media_codec codec,
	const int16_t *samples,
	size_t sample_count,
	uint8_t *payload,
	size_t payload_capacity,
	const struct media_recorder *recorder);

const char *media_codec_name(enum media_codec codec);
int media_codec_default_payload_type(
	enum media_codec codec);
int media_codec_silence_octet(enum media_codec codec);
const char *media_result_name(
	enum media_result result);

#endif
