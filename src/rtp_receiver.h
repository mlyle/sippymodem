// SPDX-License-Identifier: BSD-3-Clause

#ifndef RTP_RECEIVER_H
#define RTP_RECEIVER_H

#include "rtp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct rtp_jitter_slot {
	uint32_t timestamp;
	uint8_t octet;
	bool occupied;
};

struct rtp_receiver_config {
	uint8_t payload_type;
	uint8_t silence_octet;
	size_t prefill_samples;
};

struct rtp_receiver_stats {
	uint64_t packets_seen;
	uint64_t packets_accepted;
	uint64_t malformed_packets;
	uint64_t unexpected_payloads;
	uint64_t ssrc_restarts;
	uint64_t sequence_gaps;
	uint64_t sequence_duplicates;
	uint64_t sequence_out_of_order;
	uint64_t samples_buffered;
	uint64_t duplicate_samples;
	uint64_t late_samples;
	uint64_t early_samples;
	uint64_t samples_played;
	uint64_t samples_concealed;
	uint64_t samples_prefilled;
	uint64_t samples_idle;
};

struct rtp_receiver {
	struct rtp_jitter_slot *slots;
	size_t slot_count;
	struct rtp_receiver_config config;
	struct rtp_receiver_stats stats;
	bool stream_active;
	uint32_t ssrc;
	bool sequence_valid;
	uint16_t highest_sequence;
	uint32_t play_timestamp;
	size_t prefill_remaining;
	uint32_t clock_timestamp;
	size_t clock_pending_samples;
};

enum rtp_receive_result {
	RTP_RECEIVE_ACCEPTED,
	RTP_RECEIVE_INVALID_ARGUMENT,
	RTP_RECEIVE_MALFORMED_PACKET,
	RTP_RECEIVE_UNEXPECTED_PAYLOAD,
	RTP_RECEIVE_LATE,
	RTP_RECEIVE_TOO_EARLY,
};

struct rtp_playout_report {
	bool stream_active;
	uint32_t first_timestamp;
	size_t prefill_samples;
	size_t received_samples;
	size_t concealed_samples;
};

/* Storage is supplied and owned by the caller.  One slot holds one RTP clock
 * tick, so slot_count is also the maximum accepted lead in samples. */
bool rtp_receiver_init(
	struct rtp_receiver *receiver,
	const struct rtp_receiver_config *config,
	struct rtp_jitter_slot *slots,
	size_t slot_count);
void rtp_receiver_reset(
	struct rtp_receiver *receiver);
void rtp_receiver_restart(
	struct rtp_receiver *receiver);

enum rtp_receive_result rtp_receiver_push(
	struct rtp_receiver *receiver,
	const struct rtp_packet_view *packet);
/* Parse and push one wire packet.  packet_result optionally retains the more
 * specific packet-parser result when receive reports malformed input. */
enum rtp_receive_result rtp_receiver_receive(
	struct rtp_receiver *receiver,
	const uint8_t *packet,
	size_t packet_size,
	enum rtp_result *packet_result);

/* Take complete frames of newly observed RTP sample time, including gaps.
 * Reordered/duplicate payload does not advance time. Partial frames remain
 * owned by the stream and are discarded on restart. Pending clock credit is
 * bounded by the receive window; callers must consume it each drain. */
size_t rtp_receiver_take_clock_frames(
	struct rtp_receiver *receiver, size_t frame_samples);

/* Playout always fills count octets on success.  Missing media and time before
 * the first stream are represented by the configured silence octet. */
bool rtp_receiver_play(
	struct rtp_receiver *receiver,
	uint8_t *octets,
	size_t count,
	struct rtp_playout_report *report);

const char *rtp_receive_result_name(
	enum rtp_receive_result result);

#endif
