// SPDX-License-Identifier: BSD-3-Clause

#include "media.h"

#include "g711.h"

static bool recording_stream_is_valid(enum recording_stream stream)
{
	return stream >= RECORD_RX_RTP_PAYLOAD_ARRIVAL &&
	       stream <= RECORD_TX_RTP_PAYLOAD_SENT;
}

static bool recording_format_is_valid(enum recording_format format)
{
	return format == RECORD_OCTETS ||
	       format == RECORD_S16_NATIVE;
}

bool recording_emit(
	const struct media_recorder *recorder,
	enum recording_stream stream,
	enum recording_format format,
	unsigned int sample_rate,
	const void *data,
	size_t count)
{
	struct recording_block block;

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

const char *recording_stream_name(
	enum recording_stream stream)
{
	switch (stream) {
	case RECORD_RX_RTP_PAYLOAD_ARRIVAL:
		return "rx.rtp_payload.arrival";
	case RECORD_RX_RTP_PAYLOAD_PLAYOUT:
		return "rx.rtp_payload.playout";
	case RECORD_RX_LINE_PCM:
		return "rx.line_pcm";
	case RECORD_RX_MODEM_PCM:
		return "rx.modem_pcm";
	case RECORD_TX_MODEM_PCM:
		return "tx.modem_pcm";
	case RECORD_TX_LINE_PCM:
		return "tx.line_pcm";
	case RECORD_TX_RTP_PAYLOAD_PREPARED:
		return "tx.rtp_payload.prepared";
	case RECORD_TX_RTP_PAYLOAD_SENT:
		return "tx.rtp_payload.sent";
	case RECORD_RTP_PACKETS:
		return "rtp.packets";
	}
	return NULL;
}

static bool codec_has_linear_mapping(enum media_codec codec)
{
	return codec == MEDIA_CODEC_PCMU || codec == MEDIA_CODEC_PCMA;
}

enum media_result media_decode(
	enum media_codec codec,
	const uint8_t *payload,
	size_t payload_count,
	int16_t *samples,
	size_t sample_capacity,
	const struct media_recorder *recorder)
{
	if ((payload == NULL && payload_count != 0) ||
	    (samples == NULL && payload_count != 0))
		return MEDIA_INVALID_ARGUMENT;
	if (!codec_has_linear_mapping(codec))
		return MEDIA_NO_LINEAR_MAPPING;
	if (sample_capacity < payload_count)
		return MEDIA_OUTPUT_TOO_SMALL;

	for (size_t index = 0; index < payload_count; ++index) {
		if (codec == MEDIA_CODEC_PCMA)
			samples[index] =
				g711_decode_pcma(payload[index]);
		else
			samples[index] =
				g711_decode_pcmu(payload[index]);
	}
	recording_emit(
		recorder, RECORD_RX_RTP_PAYLOAD_PLAYOUT,
		RECORD_OCTETS, MEDIA_SAMPLE_RATE, payload,
		payload_count);
	recording_emit(recorder, RECORD_RX_LINE_PCM,
				   RECORD_S16_NATIVE,
				   MEDIA_SAMPLE_RATE, samples,
				   payload_count);
	return MEDIA_OK;
}

enum media_result media_encode(
	enum media_codec codec,
	const int16_t *samples,
	size_t sample_count,
	uint8_t *payload,
	size_t payload_capacity,
	const struct media_recorder *recorder)
{
	if ((samples == NULL && sample_count != 0) ||
	    (payload == NULL && sample_count != 0))
		return MEDIA_INVALID_ARGUMENT;
	if (!codec_has_linear_mapping(codec))
		return MEDIA_NO_LINEAR_MAPPING;
	if (payload_capacity < sample_count)
		return MEDIA_OUTPUT_TOO_SMALL;

	recording_emit(recorder, RECORD_TX_LINE_PCM,
				   RECORD_S16_NATIVE,
				   MEDIA_SAMPLE_RATE, samples,
				   sample_count);
	for (size_t index = 0; index < sample_count; ++index) {
		if (codec == MEDIA_CODEC_PCMA)
			payload[index] =
				g711_encode_pcma(samples[index]);
		else
			payload[index] =
				g711_encode_pcmu(samples[index]);
	}
	recording_emit(
		recorder, RECORD_TX_RTP_PAYLOAD_PREPARED,
		RECORD_OCTETS, MEDIA_SAMPLE_RATE, payload,
		sample_count);
	return MEDIA_OK;
}

const char *media_codec_name(enum media_codec codec)
{
	switch (codec) {
	case MEDIA_CODEC_PCMU:
		return "PCMU/8000/1";
	case MEDIA_CODEC_PCMA:
		return "PCMA/8000/1";
	case MEDIA_CODEC_CLEARMODE:
		return "CLEARMODE/8000/1";
	}
	return NULL;
}

int media_codec_default_payload_type(enum media_codec codec)
{
	switch (codec) {
	case MEDIA_CODEC_PCMU:
		return 0;
	case MEDIA_CODEC_PCMA:
		return 8;
	case MEDIA_CODEC_CLEARMODE:
		return 96;
	}
	return -1;
}

int media_codec_silence_octet(enum media_codec codec)
{
	switch (codec) {
	case MEDIA_CODEC_PCMU:
		return g711_encode_pcmu(0);
	case MEDIA_CODEC_PCMA:
		return g711_encode_pcma(0);
	case MEDIA_CODEC_CLEARMODE:
		return -1;
	}
	return -1;
}

const char *media_result_name(enum media_result result)
{
	switch (result) {
	case MEDIA_OK:
		return "ok";
	case MEDIA_INVALID_ARGUMENT:
		return "invalid argument";
	case MEDIA_OUTPUT_TOO_SMALL:
		return "output buffer too small";
	case MEDIA_NO_LINEAR_MAPPING:
		return "codec has no linear PCM mapping";
	}
	return "unknown media result";
}
