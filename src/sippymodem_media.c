// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_media.h"

#include "sippymodem_g711.h"

static bool recording_stream_is_valid(enum sippymodem_recording_stream stream)
{
	return stream >= SIPPYMODEM_RECORD_RX_RTP_PAYLOAD &&
	       stream <= SIPPYMODEM_RECORD_TX_RTP_PAYLOAD;
}

static bool recording_format_is_valid(enum sippymodem_recording_format format)
{
	return format == SIPPYMODEM_RECORD_OCTETS ||
	       format == SIPPYMODEM_RECORD_S16_NATIVE;
}

bool sippymodem_recording_emit(
	const struct sippymodem_recorder *recorder,
	enum sippymodem_recording_stream stream,
	enum sippymodem_recording_format format,
	unsigned int sample_rate,
	const void *data,
	size_t count)
{
	struct sippymodem_recording_block block;

	if (!recording_stream_is_valid(stream) ||
	    !recording_format_is_valid(format) || sample_rate == 0 ||
	    (data == NULL && count != 0))
		return false;
	if (recorder == NULL || recorder->write == NULL || count == 0)
		return true;

	block.stream = stream;
	block.format = format;
	block.sample_rate = sample_rate;
	block.data = data;
	block.count = count;
	recorder->write(recorder->context, &block);
	return true;
}

const char *sippymodem_recording_stream_name(
	enum sippymodem_recording_stream stream)
{
	switch (stream) {
	case SIPPYMODEM_RECORD_RX_RTP_PAYLOAD:
		return "rx.rtp_payload";
	case SIPPYMODEM_RECORD_RX_LINE_PCM:
		return "rx.line_pcm";
	case SIPPYMODEM_RECORD_RX_MODEM_PCM:
		return "rx.modem_pcm";
	case SIPPYMODEM_RECORD_TX_MODEM_PCM:
		return "tx.modem_pcm";
	case SIPPYMODEM_RECORD_TX_LINE_PCM:
		return "tx.line_pcm";
	case SIPPYMODEM_RECORD_TX_RTP_PAYLOAD:
		return "tx.rtp_payload";
	}
	return NULL;
}

static bool codec_has_linear_mapping(enum sippymodem_media_codec codec)
{
	return codec == SIPPYMODEM_CODEC_PCMU || codec == SIPPYMODEM_CODEC_PCMA;
}

enum sippymodem_media_result sippymodem_media_decode(
	enum sippymodem_media_codec codec,
	const uint8_t *payload,
	size_t payload_count,
	int16_t *samples,
	size_t sample_capacity,
	const struct sippymodem_recorder *recorder)
{
	if ((payload == NULL && payload_count != 0) ||
	    (samples == NULL && payload_count != 0))
		return SIPPYMODEM_MEDIA_INVALID_ARGUMENT;
	if (!codec_has_linear_mapping(codec))
		return SIPPYMODEM_MEDIA_NO_LINEAR_MAPPING;
	if (sample_capacity < payload_count)
		return SIPPYMODEM_MEDIA_OUTPUT_TOO_SMALL;

	for (size_t index = 0; index < payload_count; ++index) {
		if (codec == SIPPYMODEM_CODEC_PCMA)
			samples[index] =
				sippymodem_g711_decode_pcma(payload[index]);
		else
			samples[index] =
				sippymodem_g711_decode_pcmu(payload[index]);
	}
	sippymodem_recording_emit(recorder, SIPPYMODEM_RECORD_RX_RTP_PAYLOAD,
				   SIPPYMODEM_RECORD_OCTETS,
				   SIPPYMODEM_MEDIA_SAMPLE_RATE, payload,
				   payload_count);
	sippymodem_recording_emit(recorder, SIPPYMODEM_RECORD_RX_LINE_PCM,
				   SIPPYMODEM_RECORD_S16_NATIVE,
				   SIPPYMODEM_MEDIA_SAMPLE_RATE, samples,
				   payload_count);
	return SIPPYMODEM_MEDIA_OK;
}

enum sippymodem_media_result sippymodem_media_encode(
	enum sippymodem_media_codec codec,
	const int16_t *samples,
	size_t sample_count,
	uint8_t *payload,
	size_t payload_capacity,
	const struct sippymodem_recorder *recorder)
{
	if ((samples == NULL && sample_count != 0) ||
	    (payload == NULL && sample_count != 0))
		return SIPPYMODEM_MEDIA_INVALID_ARGUMENT;
	if (!codec_has_linear_mapping(codec))
		return SIPPYMODEM_MEDIA_NO_LINEAR_MAPPING;
	if (payload_capacity < sample_count)
		return SIPPYMODEM_MEDIA_OUTPUT_TOO_SMALL;

	sippymodem_recording_emit(recorder, SIPPYMODEM_RECORD_TX_LINE_PCM,
				   SIPPYMODEM_RECORD_S16_NATIVE,
				   SIPPYMODEM_MEDIA_SAMPLE_RATE, samples,
				   sample_count);
	for (size_t index = 0; index < sample_count; ++index) {
		if (codec == SIPPYMODEM_CODEC_PCMA)
			payload[index] =
				sippymodem_g711_encode_pcma(samples[index]);
		else
			payload[index] =
				sippymodem_g711_encode_pcmu(samples[index]);
	}
	sippymodem_recording_emit(recorder, SIPPYMODEM_RECORD_TX_RTP_PAYLOAD,
				   SIPPYMODEM_RECORD_OCTETS,
				   SIPPYMODEM_MEDIA_SAMPLE_RATE, payload,
				   sample_count);
	return SIPPYMODEM_MEDIA_OK;
}

const char *sippymodem_codec_name(enum sippymodem_media_codec codec)
{
	switch (codec) {
	case SIPPYMODEM_CODEC_PCMU:
		return "PCMU/8000/1";
	case SIPPYMODEM_CODEC_PCMA:
		return "PCMA/8000/1";
	case SIPPYMODEM_CODEC_CLEARMODE:
		return "CLEARMODE/8000/1";
	}
	return NULL;
}

int sippymodem_codec_default_payload_type(enum sippymodem_media_codec codec)
{
	switch (codec) {
	case SIPPYMODEM_CODEC_PCMU:
		return 0;
	case SIPPYMODEM_CODEC_PCMA:
		return 8;
	case SIPPYMODEM_CODEC_CLEARMODE:
		return 96;
	}
	return -1;
}

int sippymodem_codec_silence_octet(enum sippymodem_media_codec codec)
{
	switch (codec) {
	case SIPPYMODEM_CODEC_PCMU:
		return sippymodem_g711_encode_pcmu(0);
	case SIPPYMODEM_CODEC_PCMA:
		return sippymodem_g711_encode_pcma(0);
	case SIPPYMODEM_CODEC_CLEARMODE:
		return -1;
	}
	return -1;
}

const char *sippymodem_media_result_name(enum sippymodem_media_result result)
{
	switch (result) {
	case SIPPYMODEM_MEDIA_OK:
		return "ok";
	case SIPPYMODEM_MEDIA_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_MEDIA_OUTPUT_TOO_SMALL:
		return "output buffer too small";
	case SIPPYMODEM_MEDIA_NO_LINEAR_MAPPING:
		return "codec has no linear PCM mapping";
	}
	return "unknown media result";
}
