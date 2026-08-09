// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_MEDIA_H
#define SIPPYMODEM_MEDIA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	SIPPYMODEM_MEDIA_SAMPLE_RATE = 8000,
};

enum sippymodem_media_codec {
	SIPPYMODEM_CODEC_PCMU,
	SIPPYMODEM_CODEC_PCMA,
	SIPPYMODEM_CODEC_CLEARMODE,
};

enum sippymodem_recording_stream {
	SIPPYMODEM_RECORD_RX_RTP_PAYLOAD_PLAYOUT,
	SIPPYMODEM_RECORD_RX_LINE_PCM,
	SIPPYMODEM_RECORD_RX_MODEM_PCM,
	SIPPYMODEM_RECORD_TX_MODEM_PCM,
	SIPPYMODEM_RECORD_TX_LINE_PCM,
	SIPPYMODEM_RECORD_TX_RTP_PAYLOAD_PREPARED,
};

enum sippymodem_recording_format {
	SIPPYMODEM_RECORD_OCTETS,
	SIPPYMODEM_RECORD_S16_NATIVE,
};

struct sippymodem_recording_block {
	enum sippymodem_recording_stream stream;
	enum sippymodem_recording_format format;
	unsigned int sample_rate;
	const void *data;
	size_t count;
};

/* The callback is synchronous.  Both the block and its data are borrowed for
 * the duration of the call; a recorder that needs them later must copy them. */
struct sippymodem_recorder {
	void *context;
	void (*write)(void *context,
		      const struct sippymodem_recording_block *block);
};

bool sippymodem_recording_emit(
	const struct sippymodem_recorder *recorder,
	enum sippymodem_recording_stream stream,
	enum sippymodem_recording_format format,
	unsigned int sample_rate,
	const void *data,
	size_t count);
const char *sippymodem_recording_stream_name(
	enum sippymodem_recording_stream stream);

enum sippymodem_media_result {
	SIPPYMODEM_MEDIA_OK,
	SIPPYMODEM_MEDIA_INVALID_ARGUMENT,
	SIPPYMODEM_MEDIA_OUTPUT_TOO_SMALL,
	SIPPYMODEM_MEDIA_NO_LINEAR_MAPPING,
};

enum sippymodem_media_result sippymodem_media_decode(
	enum sippymodem_media_codec codec,
	const uint8_t *payload,
	size_t payload_count,
	int16_t *samples,
	size_t sample_capacity,
	const struct sippymodem_recorder *recorder);
enum sippymodem_media_result sippymodem_media_encode(
	enum sippymodem_media_codec codec,
	const int16_t *samples,
	size_t sample_count,
	uint8_t *payload,
	size_t payload_capacity,
	const struct sippymodem_recorder *recorder);

const char *sippymodem_codec_name(enum sippymodem_media_codec codec);
int sippymodem_codec_default_payload_type(
	enum sippymodem_media_codec codec);
int sippymodem_codec_silence_octet(enum sippymodem_media_codec codec);
const char *sippymodem_media_result_name(
	enum sippymodem_media_result result);

#endif
