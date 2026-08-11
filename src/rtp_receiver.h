// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_RTP_RECEIVER_H
#define SIPPYMODEM_RTP_RECEIVER_H

#include "rtp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct sippymodem_rtp_jitter_slot {
	uint32_t timestamp;
	uint8_t octet;
	bool occupied;
};

struct sippymodem_rtp_receiver_config {
	uint8_t payload_type;
	uint8_t silence_octet;
	size_t prefill_samples;
};

struct sippymodem_rtp_receiver_stats {
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

struct sippymodem_rtp_receiver {
	struct sippymodem_rtp_jitter_slot *slots;
	size_t slot_count;
	struct sippymodem_rtp_receiver_config config;
	struct sippymodem_rtp_receiver_stats stats;
	bool stream_active;
	uint32_t ssrc;
	bool sequence_valid;
	uint16_t highest_sequence;
	uint32_t play_timestamp;
	size_t prefill_remaining;
};

enum sippymodem_rtp_receive_result {
	SIPPYMODEM_RTP_RECEIVE_ACCEPTED,
	SIPPYMODEM_RTP_RECEIVE_INVALID_ARGUMENT,
	SIPPYMODEM_RTP_RECEIVE_MALFORMED_PACKET,
	SIPPYMODEM_RTP_RECEIVE_UNEXPECTED_PAYLOAD,
	SIPPYMODEM_RTP_RECEIVE_LATE,
	SIPPYMODEM_RTP_RECEIVE_TOO_EARLY,
};

struct sippymodem_rtp_playout_report {
	bool stream_active;
	uint32_t first_timestamp;
	size_t prefill_samples;
	size_t received_samples;
	size_t concealed_samples;
};

/* Storage is supplied and owned by the caller.  One slot holds one RTP clock
 * tick, so slot_count is also the maximum accepted lead in samples. */
bool sippymodem_rtp_receiver_init(
	struct sippymodem_rtp_receiver *receiver,
	const struct sippymodem_rtp_receiver_config *config,
	struct sippymodem_rtp_jitter_slot *slots,
	size_t slot_count);
void sippymodem_rtp_receiver_reset(
	struct sippymodem_rtp_receiver *receiver);
void sippymodem_rtp_receiver_restart(
	struct sippymodem_rtp_receiver *receiver);

enum sippymodem_rtp_receive_result sippymodem_rtp_receiver_push(
	struct sippymodem_rtp_receiver *receiver,
	const struct sippymodem_rtp_packet_view *packet);
/* Parse and push one wire packet.  packet_result optionally retains the more
 * specific packet-parser result when receive reports malformed input. */
enum sippymodem_rtp_receive_result sippymodem_rtp_receiver_receive(
	struct sippymodem_rtp_receiver *receiver,
	const uint8_t *packet,
	size_t packet_size,
	enum sippymodem_rtp_result *packet_result);

/* Playout always fills count octets on success.  Missing media and time before
 * the first stream are represented by the configured silence octet. */
bool sippymodem_rtp_receiver_play(
	struct sippymodem_rtp_receiver *receiver,
	uint8_t *octets,
	size_t count,
	struct sippymodem_rtp_playout_report *report);

const char *sippymodem_rtp_receive_result_name(
	enum sippymodem_rtp_receive_result result);

#endif
