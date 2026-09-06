// SPDX-License-Identifier: BSD-3-Clause

#include "rtp_receiver.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

static bool timestamp_is_before(uint32_t left, uint32_t right)
{
	return (int32_t)(left - right) < 0;
}

static void clear_slots(struct rtp_receiver *receiver)
{
	memset(receiver->slots, 0,
	       receiver->slot_count * sizeof(receiver->slots[0]));
}

static void start_stream(struct rtp_receiver *receiver,
			 const struct rtp_packet_view *packet)
{
	clear_slots(receiver);
	receiver->stream_active = true;
	receiver->ssrc = packet->header.ssrc;
	receiver->sequence_valid = false;
	receiver->highest_sequence = 0;
	receiver->play_timestamp = packet->header.timestamp;
	receiver->prefill_remaining = receiver->config.prefill_samples;
	receiver->clock_timestamp = packet->header.timestamp;
	receiver->clock_pending_samples = 0;
}

static void note_sequence(struct rtp_receiver *receiver,
			  uint16_t sequence)
{
	uint16_t distance;

	if (!receiver->sequence_valid) {
		receiver->sequence_valid = true;
		receiver->highest_sequence = sequence;
		return;
	}

	distance = (uint16_t)(sequence - receiver->highest_sequence);
	if (distance == 0) {
		receiver->stats.sequence_duplicates++;
	} else if (distance < 0x8000) {
		if (distance > 1)
			receiver->stats.sequence_gaps += distance - 1;
		receiver->highest_sequence = sequence;
	} else {
		receiver->stats.sequence_out_of_order++;
	}
}

bool rtp_receiver_init(
	struct rtp_receiver *receiver,
	const struct rtp_receiver_config *config,
	struct rtp_jitter_slot *slots,
	size_t slot_count)
{
	if (receiver == NULL || config == NULL || slots == NULL ||
	    slot_count == 0 || slot_count > INT32_MAX ||
	    slot_count > SIZE_MAX / sizeof(slots[0]) ||
	    config->payload_type > RTP_MAX_PAYLOAD_TYPE)
		return false;

	memset(receiver, 0, sizeof(*receiver));
	receiver->slots = slots;
	receiver->slot_count = slot_count;
	receiver->config = *config;
	clear_slots(receiver);
	return true;
}

void rtp_receiver_restart(
	struct rtp_receiver *receiver)
{
	if (receiver == NULL || receiver->slots == NULL ||
	    receiver->slot_count == 0)
		return;

	clear_slots(receiver);
	receiver->stream_active = false;
	receiver->ssrc = 0;
	receiver->sequence_valid = false;
	receiver->highest_sequence = 0;
	receiver->play_timestamp = 0;
	receiver->prefill_remaining = 0;
	receiver->clock_timestamp = 0;
	receiver->clock_pending_samples = 0;
}

void rtp_receiver_reset(
	struct rtp_receiver *receiver)
{
	if (receiver == NULL)
		return;
	rtp_receiver_restart(receiver);
	memset(&receiver->stats, 0, sizeof(receiver->stats));
}

enum rtp_receive_result rtp_receiver_push(
	struct rtp_receiver *receiver,
	const struct rtp_packet_view *packet)
{
	bool buffered = false;
	bool duplicate = false;
	bool late = false;
	bool early = false;
	uint32_t buffered_end = 0;

	if (receiver == NULL || receiver->slots == NULL || packet == NULL ||
	    (packet->payload == NULL && packet->payload_size != 0) ||
	    packet->payload_size > UINT32_MAX)
		return RTP_RECEIVE_INVALID_ARGUMENT;

	receiver->stats.packets_seen++;
	if (packet->header.payload_type != receiver->config.payload_type) {
		receiver->stats.unexpected_payloads++;
		return RTP_RECEIVE_UNEXPECTED_PAYLOAD;
	}
	if (!receiver->stream_active)
		start_stream(receiver, packet);
	else if (receiver->ssrc != packet->header.ssrc) {
		receiver->stats.ssrc_restarts++;
		start_stream(receiver, packet);
	}
	note_sequence(receiver, packet->header.sequence);

	for (size_t index = 0; index < packet->payload_size; ++index) {
		uint32_t timestamp = packet->header.timestamp + (uint32_t)index;
		uint32_t lead;
		struct rtp_jitter_slot *slot;

		if (timestamp_is_before(timestamp, receiver->play_timestamp)) {
			receiver->stats.late_samples++;
			late = true;
			continue;
		}
		lead = timestamp - receiver->play_timestamp;
		if (lead >= receiver->slot_count) {
			receiver->stats.early_samples++;
			early = true;
			continue;
		}

		slot = &receiver->slots[timestamp % receiver->slot_count];
		if (slot->occupied && slot->timestamp == timestamp) {
			receiver->stats.duplicate_samples++;
			duplicate = true;
			continue;
		}
		slot->timestamp = timestamp;
		slot->octet = packet->payload[index];
		slot->occupied = true;
		receiver->stats.samples_buffered++;
		buffered = true;
		buffered_end = timestamp + 1;
	}
	if (buffered &&
	    timestamp_is_before(receiver->clock_timestamp, buffered_end)) {
		uint32_t elapsed = buffered_end - receiver->clock_timestamp;
		size_t available = receiver->slot_count -
				   receiver->clock_pending_samples;

		/* Only successfully buffered timestamps can supply clock credit.
		 * A lost packet costs receive audio, not elapsed transmit time. */
		receiver->clock_pending_samples +=
			elapsed < available ? elapsed : available;
		receiver->clock_timestamp = buffered_end;
	}

	if (packet->payload_size == 0 || buffered || duplicate) {
		receiver->stats.packets_accepted++;
		return RTP_RECEIVE_ACCEPTED;
	}
	if (late)
		return RTP_RECEIVE_LATE;
	if (early)
		return RTP_RECEIVE_TOO_EARLY;
	return RTP_RECEIVE_ACCEPTED;
}

size_t rtp_receiver_take_clock_frames(
	struct rtp_receiver *receiver, size_t frame_samples)
{
	size_t frames;

	if (receiver == NULL || frame_samples == 0)
		return 0;
	frames = receiver->clock_pending_samples / frame_samples;
	receiver->clock_pending_samples %= frame_samples;
	return frames;
}

enum rtp_receive_result rtp_receiver_receive(
	struct rtp_receiver *receiver,
	const uint8_t *packet,
	size_t packet_size,
	enum rtp_result *packet_result)
{
	struct rtp_packet_view view;
	enum rtp_result result;

	if (receiver == NULL || receiver->slots == NULL || packet == NULL) {
		if (packet_result != NULL)
			*packet_result = RTP_INVALID_ARGUMENT;
		return RTP_RECEIVE_INVALID_ARGUMENT;
	}

	result = rtp_packet_parse(packet, packet_size, &view);
	if (packet_result != NULL)
		*packet_result = result;
	if (result != RTP_OK) {
		receiver->stats.packets_seen++;
		receiver->stats.malformed_packets++;
		return RTP_RECEIVE_MALFORMED_PACKET;
	}
	return rtp_receiver_push(receiver, &view);
}

bool rtp_receiver_play(
	struct rtp_receiver *receiver,
	uint8_t *octets,
	size_t count,
	struct rtp_playout_report *report)
{
	struct rtp_playout_report played = { 0 };
	size_t index = 0;

	if (receiver == NULL || receiver->slots == NULL || octets == NULL ||
	    count == 0)
		return false;

	if (!receiver->stream_active) {
		memset(octets, receiver->config.silence_octet, count);
		receiver->stats.samples_idle += count;
		if (report != NULL)
			*report = played;
		return true;
	}

	played.stream_active = true;
	played.first_timestamp = receiver->play_timestamp;
	played.prefill_samples = receiver->prefill_remaining < count ?
				 receiver->prefill_remaining : count;
	if (played.prefill_samples != 0) {
		memset(octets, receiver->config.silence_octet,
		       played.prefill_samples);
		receiver->prefill_remaining -= played.prefill_samples;
		receiver->stats.samples_prefilled += played.prefill_samples;
		index = played.prefill_samples;
	}

	for (; index < count; ++index) {
		uint32_t timestamp = receiver->play_timestamp++;
		struct rtp_jitter_slot *slot =
			&receiver->slots[timestamp % receiver->slot_count];

		if (slot->occupied && slot->timestamp == timestamp) {
			octets[index] = slot->octet;
			slot->occupied = false;
			played.received_samples++;
			receiver->stats.samples_played++;
		} else {
			slot->occupied = false;
			octets[index] = receiver->config.silence_octet;
			played.concealed_samples++;
			receiver->stats.samples_concealed++;
		}
	}

	if (report != NULL)
		*report = played;
	return true;
}

const char *rtp_receive_result_name(
	enum rtp_receive_result result)
{
	switch (result) {
	case RTP_RECEIVE_ACCEPTED:
		return "accepted";
	case RTP_RECEIVE_INVALID_ARGUMENT:
		return "invalid argument";
	case RTP_RECEIVE_MALFORMED_PACKET:
		return "malformed RTP packet";
	case RTP_RECEIVE_UNEXPECTED_PAYLOAD:
		return "unexpected payload type";
	case RTP_RECEIVE_LATE:
		return "packet is late";
	case RTP_RECEIVE_TOO_EARLY:
		return "packet is outside the playout window";
	}
	return "unknown receive result";
}
