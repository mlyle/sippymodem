// SPDX-License-Identifier: BSD-3-Clause

#ifndef RTP_STATE_FIXTURE_H
#define RTP_STATE_FIXTURE_H

#include <rtp_receiver.h>

/* Snapshot comparisons check declared state, not unspecified struct padding. */
static inline bool rtp_receiver_states_equal(
	const struct rtp_receiver *left, const struct rtp_receiver *right)
{
	return left->slots == right->slots &&
	       left->slot_count == right->slot_count &&
	       left->config.payload_type == right->config.payload_type &&
	       left->config.silence_octet == right->config.silence_octet &&
	       left->config.prefill_samples == right->config.prefill_samples &&
	       left->stream_active == right->stream_active &&
	       left->ssrc == right->ssrc &&
	       left->sequence_valid == right->sequence_valid &&
	       left->highest_sequence == right->highest_sequence &&
	       left->play_timestamp == right->play_timestamp &&
	       left->prefill_remaining == right->prefill_remaining &&
	       left->clock_timestamp == right->clock_timestamp &&
	       left->buffered_timestamp == right->buffered_timestamp &&
	       left->clock_pending_samples == right->clock_pending_samples &&
	       left->stats.packets_seen == right->stats.packets_seen &&
	       left->stats.packets_accepted == right->stats.packets_accepted &&
	       left->stats.malformed_packets == right->stats.malformed_packets &&
	       left->stats.unexpected_payloads == right->stats.unexpected_payloads &&
	       left->stats.ssrc_restarts == right->stats.ssrc_restarts &&
	       left->stats.sequence_gaps == right->stats.sequence_gaps &&
	       left->stats.sequence_duplicates == right->stats.sequence_duplicates &&
	       left->stats.sequence_out_of_order == right->stats.sequence_out_of_order &&
	       left->stats.samples_buffered == right->stats.samples_buffered &&
	       left->stats.duplicate_samples == right->stats.duplicate_samples &&
	       left->stats.late_samples == right->stats.late_samples &&
	       left->stats.early_samples == right->stats.early_samples &&
	       left->stats.samples_played == right->stats.samples_played &&
	       left->stats.samples_concealed == right->stats.samples_concealed &&
	       left->stats.samples_prefilled == right->stats.samples_prefilled &&
	       left->stats.samples_idle == right->stats.samples_idle;
}

#endif
