// SPDX-License-Identifier: BSD-3-Clause

#include "rtp_stream.h"

#include "g711.h"
#include "memory_region.h"
#include "rtp.h"

#include <stdint.h>
#include <string.h>

bool rtp_stream_init(
	struct rtp_stream *stream,
	const struct rtp_stream_config *config,
	struct rtp_jitter_slot *slots,
	size_t slot_count,
	uint8_t *receive_workspace,
	size_t receive_workspace_size)
{
	struct rtp_stream initialized = { 0 };
	struct rtp_receiver_config receiver_config;
	int silence_octet;

	if (stream == NULL || config == NULL || receive_workspace == NULL ||
	    receive_workspace_size == 0 ||
	    config->payload_type > RTP_MAX_PAYLOAD_TYPE)
		return false;
	silence_octet = media_codec_silence_octet(config->codec);
	if (silence_octet < 0)
		return false;

	receiver_config.payload_type = config->payload_type;
	receiver_config.silence_octet = (uint8_t)silence_octet;
	receiver_config.prefill_samples = config->prefill_samples;
	if (!rtp_receiver_init(&initialized.receiver,
					   &receiver_config, slots, slot_count))
		return false;

	initialized.codec = config->codec;
	initialized.recorder = config->recorder;
	initialized.receive_workspace = receive_workspace;
	initialized.receive_workspace_size = receive_workspace_size;
	initialized.transmit_header.payload_type = config->payload_type;
	initialized.transmit_header.sequence = config->transmit_sequence;
	initialized.transmit_header.timestamp = config->transmit_timestamp;
	initialized.transmit_header.ssrc = config->transmit_ssrc;
	*stream = initialized;
	return true;
}

enum rtp_receive_result rtp_stream_receive(
	struct rtp_stream *stream,
	const uint8_t *packet,
	size_t packet_size,
	enum rtp_result *packet_result)
{
	if (stream == NULL) {
		if (packet_result != NULL)
			*packet_result = RTP_INVALID_ARGUMENT;
		return RTP_RECEIVE_INVALID_ARGUMENT;
	}
	return rtp_receiver_receive(&stream->receiver, packet,
						packet_size, packet_result);
}

enum media_result rtp_stream_play(
	struct rtp_stream *stream,
	int16_t *samples,
	uint8_t *source_octets, bool *received_mask,
	size_t count,
	struct rtp_playout_report *report)
{
	enum media_result result;

	if (stream == NULL || samples == NULL || count == 0 ||
	    count > stream->receive_workspace_size || count > SIZE_MAX / sizeof(*samples))
		return MEDIA_INVALID_ARGUMENT;
	if (received_mask != NULL &&
	    (memory_regions_overlap(received_mask, count * sizeof(*received_mask),
		 samples, count * sizeof(*samples)) ||
	     memory_regions_overlap(received_mask, count * sizeof(*received_mask),
		 source_octets, source_octets == NULL ? 0u : count) ||
	     memory_regions_overlap(received_mask, count * sizeof(*received_mask),
		 stream, sizeof(*stream))))
		return MEDIA_INVALID_ARGUMENT;
	if (!rtp_receiver_play(&stream->receiver,
					   stream->receive_workspace, received_mask, count,
					   report))
		return MEDIA_INVALID_ARGUMENT;
	result = media_decode(stream->codec,
			      stream->receive_workspace, count, samples,
			      count, &stream->recorder);
	if (result == MEDIA_OK && source_octets != NULL)
		memcpy(source_octets, stream->receive_workspace, count);
	return result;
}

static enum media_result complete_write(
	struct rtp_stream *stream, size_t count,
	uint8_t *packet, size_t packet_capacity,
	size_t *packet_size)
{
	enum rtp_result packet_result;
	size_t written;

	packet_result = rtp_packet_write(
		packet, packet_capacity, &stream->transmit_header,
		packet + RTP_HEADER_SIZE, count, &written);
	if (packet_result != RTP_OK)
		return packet_result == RTP_BUFFER_TOO_SMALL ?
			       MEDIA_OUTPUT_TOO_SMALL :
			       MEDIA_INVALID_ARGUMENT;

	*packet_size = written;
	stream->transmit_header.sequence++;
	stream->transmit_header.timestamp += (uint32_t)count;
	stream->stats.transmit_packets++;
	stream->stats.transmit_samples += count;
	return MEDIA_OK;
}

static enum media_result validate_write(
	const struct rtp_stream *stream, size_t count,
	const uint8_t *packet, size_t packet_capacity,
	const size_t *packet_size)
{
	if (stream == NULL || count == 0 || packet == NULL ||
	    packet_size == NULL || count > UINT32_MAX ||
	    (stream->codec != MEDIA_CODEC_PCMU &&
	     stream->codec != MEDIA_CODEC_PCMA) ||
	    stream->transmit_header.payload_type > RTP_MAX_PAYLOAD_TYPE)
		return MEDIA_INVALID_ARGUMENT;
	if (packet_capacity < RTP_HEADER_SIZE ||
	    count > packet_capacity - RTP_HEADER_SIZE)
		return MEDIA_OUTPUT_TOO_SMALL;
	return MEDIA_OK;
}

enum media_result rtp_stream_write(
	struct rtp_stream *stream,
	const int16_t *samples,
	size_t count,
	uint8_t *packet,
	size_t packet_capacity,
	size_t *packet_size)
{
	enum media_result result = validate_write(
		stream, count, packet, packet_capacity, packet_size);

	if (samples == NULL)
		return MEDIA_INVALID_ARGUMENT;
	if (result != MEDIA_OK)
		return result;
	result = media_encode(
		stream->codec, samples, count, packet + RTP_HEADER_SIZE,
		packet_capacity - RTP_HEADER_SIZE, &stream->recorder);
	return result == MEDIA_OK ?
		complete_write(stream, count, packet, packet_capacity,
			       packet_size) :
		result;
}

static int16_t decode_codeword(enum media_codec codec, uint8_t codeword)
{
	return codec == MEDIA_CODEC_PCMA ?
		g711_decode_pcma(codeword) : g711_decode_pcmu(codeword);
}

enum media_result rtp_stream_write_g711(
	struct rtp_stream *stream,
	const uint8_t *codewords,
	const int16_t *decoded_samples,
	size_t count,
	uint8_t *packet,
	size_t packet_capacity,
	size_t *packet_size)
{
	enum media_result result = validate_write(
		stream, count, packet, packet_capacity, packet_size);
	uint8_t *payload;

	if (codewords == NULL || decoded_samples == NULL)
		return MEDIA_INVALID_ARGUMENT;
	if (result != MEDIA_OK)
		return result;
	for (size_t index = 0; index < count; ++index) {
		if (decoded_samples[index] !=
		    decode_codeword(stream->codec, codewords[index]))
			return MEDIA_INVALID_ARGUMENT;
	}

	payload = packet + RTP_HEADER_SIZE;
	recording_emit(
		&stream->recorder, RECORD_TX_LINE_PCM, RECORD_S16_NATIVE,
		MEDIA_SAMPLE_RATE, decoded_samples, count);
	memcpy(payload, codewords, count);
	recording_emit(
		&stream->recorder, RECORD_TX_RTP_PAYLOAD_PREPARED,
		RECORD_OCTETS, MEDIA_SAMPLE_RATE, payload, count);
	return complete_write(
		stream, count, packet, packet_capacity, packet_size);
}
