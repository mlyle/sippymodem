// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_RTP_STREAM_H
#define SIPPYMODEM_RTP_STREAM_H

#include "media.h"
#include "rtp_receiver.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct sippymodem_rtp_stream_config {
	enum sippymodem_media_codec codec;
	uint8_t payload_type;
	size_t prefill_samples;
	uint16_t transmit_sequence;
	uint32_t transmit_timestamp;
	uint32_t transmit_ssrc;
	struct sippymodem_recorder recorder;
};

struct sippymodem_rtp_stream_stats {
	uint64_t transmit_packets;
	uint64_t transmit_samples;
};

struct sippymodem_rtp_stream {
	enum sippymodem_media_codec codec;
	struct sippymodem_recorder recorder;
	struct sippymodem_rtp_receiver receiver;
	uint8_t *receive_workspace;
	size_t receive_workspace_size;
	struct sippymodem_rtp_header transmit_header;
	struct sippymodem_rtp_stream_stats stats;
};

/* All working storage is supplied by the caller and must outlive the stream. */
bool sippymodem_rtp_stream_init(
	struct sippymodem_rtp_stream *stream,
	const struct sippymodem_rtp_stream_config *config,
	struct sippymodem_rtp_jitter_slot *slots,
	size_t slot_count,
	uint8_t *receive_workspace,
	size_t receive_workspace_size);

enum sippymodem_rtp_receive_result sippymodem_rtp_stream_receive(
	struct sippymodem_rtp_stream *stream,
	const uint8_t *packet,
	size_t packet_size,
	enum sippymodem_rtp_result *packet_result);

enum sippymodem_media_result sippymodem_rtp_stream_play(
	struct sippymodem_rtp_stream *stream,
	int16_t *samples,
	size_t count,
	struct sippymodem_rtp_playout_report *report);

enum sippymodem_media_result sippymodem_rtp_stream_write(
	struct sippymodem_rtp_stream *stream,
	const int16_t *samples,
	size_t count,
	uint8_t *packet,
	size_t packet_capacity,
	size_t *packet_size);

#endif
