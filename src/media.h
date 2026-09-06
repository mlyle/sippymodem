// SPDX-License-Identifier: BSD-3-Clause

#ifndef MEDIA_H
#define MEDIA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	MEDIA_SAMPLE_RATE = 8000,
	MEDIA_MAX_CATCHUP_FRAMES = 4,
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
	/* Structured packet metadata uses the recorder's packet callback. */
	RECORD_RTP_PACKETS,
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

enum recording_packet_event {
	RECORD_PACKET_SENT,
	RECORD_PACKET_SEND_FAILED,
	RECORD_PACKET_ACCEPTED,
	RECORD_PACKET_LATE,
	RECORD_PACKET_EARLY,
	RECORD_PACKET_REJECTED,
};

/* Application socket-observation time, not kernel arrival or remote playout.
 * A zero time means the monotonic clock was unavailable. No payload or network
 * address is retained. Playout counters identify the local receive timeline. */
struct recording_packet {
	enum recording_packet_event event;
	uint64_t monotonic_ns;
	uint64_t playout_samples;
	uint64_t concealed_samples;
	uint32_t timestamp;
	uint32_t ssrc;
	uint32_t payload_octets;
	uint16_t sequence;
	uint8_t payload_type;
	bool marker;
};

/* The callback is synchronous.  Both the block and its data are borrowed for
 * the duration of the call; a recorder that needs them later must copy them.
 * The optional packet callback has the same ownership and lifetime contract. */
struct media_recorder {
	void *context;
	void (*write)(void *context,
		      const struct recording_block *block);
	void (*packet)(void *context, const struct recording_packet *packet);
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
