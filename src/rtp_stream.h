// SPDX-License-Identifier: BSD-3-Clause

#ifndef RTP_STREAM_H
#define RTP_STREAM_H

#include "media.h"
#include "rtp_receiver.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct rtp_stream_config {
	enum media_codec codec;
	uint8_t payload_type;
	size_t prefill_samples;
	uint16_t transmit_sequence;
	uint32_t transmit_timestamp;
	uint32_t transmit_ssrc;
	struct media_recorder recorder;
};

struct rtp_stream_stats {
	uint64_t transmit_packets;
	uint64_t transmit_samples;
};

struct rtp_stream {
	enum media_codec codec;
	struct media_recorder recorder;
	struct rtp_receiver receiver;
	uint8_t *receive_workspace;
	size_t receive_workspace_size;
	struct rtp_header transmit_header;
	struct rtp_stream_stats stats;
};

/* All working storage is supplied by the caller and must outlive the stream. */
bool rtp_stream_init(
	struct rtp_stream *stream,
	const struct rtp_stream_config *config,
	struct rtp_jitter_slot *slots,
	size_t slot_count,
	uint8_t *receive_workspace,
	size_t receive_workspace_size);

enum rtp_receive_result rtp_stream_receive(
	struct rtp_stream *stream,
	const uint8_t *packet,
	size_t packet_size,
	enum rtp_result *packet_result);

enum media_result rtp_stream_play(
	struct rtp_stream *stream,
	int16_t *samples,
	size_t count,
	struct rtp_playout_report *report);

enum media_result rtp_stream_write(
	struct rtp_stream *stream,
	const int16_t *samples,
	size_t count,
	uint8_t *packet,
	size_t packet_capacity,
	size_t *packet_size);

#endif
