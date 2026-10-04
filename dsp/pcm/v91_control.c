// SPDX-License-Identifier: BSD-3-Clause
#include "v91_control.h"
#include "pcm_control_frame.h"
#include <string.h>

static uint16_t info_crc(const uint8_t *frame)
{
	uint16_t crc = CRC16_CCITT_FALSE_INITIAL_REMAINDER;
	for (unsigned bit = 12; bit < 42; ++bit)
		crc = crc16_ccitt_false_step(crc, packed_msb_bit(frame, bit));
	return crc;
}

bool v91_info_encode(const struct v91_info *p, uint8_t frame[V91_INFO_BYTES])
{
	uint8_t next[V91_INFO_BYTES] = { 0 };
	if (p == NULL || frame == NULL || !g711_law_is_valid(p->law) || p->maximum_power > 31 ||
	    (p->require_transparent && !p->transparent))
		return false;
	packed_msb_write_msb_field(next, 0, 12, 0xf72);
	packed_msb_set_bit(next, 26, p->custom_dil);
	packed_msb_set_bit(next, 27, p->control_channel);
	packed_msb_set_bit(next, 28, p->acknowledges_peer);
	packed_msb_write_lsb_field(next, 33, 5, p->maximum_power);
	packed_msb_set_bit(next, 38, p->receiver_power);
	packed_msb_set_bit(next, 39, p->law == G711_LAW_PCMA);
	packed_msb_set_bit(next, 40, p->transparent);
	packed_msb_set_bit(next, 41, p->require_transparent);
	packed_msb_write_msb_field(next, 42, 16, info_crc(next));
	packed_msb_write_msb_field(next, 58, 4, 15);
	memcpy(frame, next, sizeof(next));
	return true;
}

bool v91_info_decode(const uint8_t frame[V91_INFO_BYTES], struct v91_info *p)
{
	if (frame == NULL || p == NULL || packed_msb_read_msb_field(frame, 0, 12) != 0xf72 ||
	    packed_msb_read_msb_field(frame, 58, 4) != 15 ||
	    packed_msb_read_msb_field(frame, 42, 16) != info_crc(frame))
		return false;
	struct v91_info next = {
		.law = packed_msb_bit(frame, 39) ? G711_LAW_PCMA : G711_LAW_PCMU,
		.maximum_power = packed_msb_read_lsb_field(frame, 33, 5),
		.custom_dil = packed_msb_bit(frame, 26),
		.control_channel = packed_msb_bit(frame, 27),
		.acknowledges_peer = packed_msb_bit(frame, 28),
		.receiver_power = packed_msb_bit(frame, 38),
		.transparent = packed_msb_bit(frame, 40),
		.require_transparent = packed_msb_bit(frame, 41),
	};
	if (next.require_transparent && !next.transparent)
		return false;
	*p = next;
	return true;
}

static size_t cp_length(unsigned count)
{
	return ((136u * (1u + count) + 18u + 5u) / 6u) * 6u;
}

static bool cp_valid(const struct v91_cp *p)
{
	unsigned maximum = 0;
	uint64_t capacity = 1;
	if (p == NULL || p->count == 0 || p->count > 6 || p->rate > 28 ||
	    (p->transparent && p->rate != 28))
		return false;
	for (unsigned i = 0; i < 6; ++i) {
		unsigned row = p->frame_constellation[i];
		if (row >= p->count)
			return false;
		if (row > maximum)
			maximum = row;
		capacity *= pcm_constellation_count(&p->constellations[row]);
	}
	return maximum + 1 == p->count &&
	       (p->rate == 0 || capacity >= (UINT64_C(1) << (p->rate + 14u)));
}

size_t v91_cp_encode(const struct v91_cp *p, uint8_t frame[V91_CP_MAX_BYTES])
{
	uint8_t next[V91_CP_MAX_BYTES] = { 0 };
	if (frame == NULL || !cp_valid(p))
		return 0;
	pcm_control_frame_write_prefix(next);
	packed_msb_set_bit(next, 18, p->transparent);
	packed_msb_set_bit(next, 19, 1);
	packed_msb_write_lsb_field(next, 20, 5, p->rate);
	packed_msb_set_bit(next, 33, p->acknowledges_peer);
	pcm_control_frame_write_map(next, p->frame_constellation);
	pcm_control_frame_write_constellation_payload(next, p->constellations,
		NULL, p->count, false);
	size_t bits = cp_length(p->count);
	memcpy(frame, next, (bits + 7) / 8);
	return bits;
}

size_t v91_cp_length(const uint8_t *header, size_t bits)
{
	uint8_t map[6];
	size_t count;
	if (header == NULL || bits < 136 || !pcm_control_frame_prefix_is_valid(header) ||
	    (count = pcm_control_frame_read_map(header, map)) == 0)
		return 0;
	return cp_length(count);
}

bool v91_cp_decode(const uint8_t *frame, size_t bits, struct v91_cp *p)
{
	struct v91_cp next = { 0 };
	size_t count;
	if (frame == NULL || p == NULL || bits < cp_length(1) || bits > V91_CP_MAX_BITS ||
	    bits != v91_cp_length(frame, bits) ||
	    (count = pcm_control_frame_read_map(frame, next.frame_constellation)) == 0)
		return false;
	size_t payload = 136u * (1u + count);
	if (!pcm_control_frame_starts_are_zero(frame, 17, payload + 1) ||
	    !packed_msb_bits_are_zero(frame, payload + 17, bits - payload - 17) ||
	    !pcm_control_frame_read_constellation_payload(frame, next.constellations,
		NULL, count, false))
		return false;
	next.count = count;
	next.rate = packed_msb_read_lsb_field(frame, 20, 5);
	next.transparent = packed_msb_bit(frame, 18);
	next.acknowledges_peer = packed_msb_bit(frame, 33);
	if (!cp_valid(&next))
		return false;
	*p = next;
	return true;
}

static uint16_t control_crc(const uint8_t frame[12])
{
	uint16_t crc = CRC16_CCITT_FALSE_INITIAL_REMAINDER;
	for (unsigned bit = 16; bit < 78; ++bit)
		crc = crc16_ccitt_false_step(crc, packed_msb_bit(frame, bit));
	return crc;
}

bool v91_control_encode(uint8_t frame[12], bool retrain, bool cleardown)
{
	if (frame == NULL)
		return false;
	memset(frame, 0, 12);
	packed_msb_write_msb_field(frame, 0, 16, 0x77fa);
	packed_msb_set_bit(frame, 16, cleardown);
	packed_msb_set_bit(frame, 17, retrain);
	packed_msb_write_msb_field(frame, 78, 16, control_crc(frame));
	return true;
}

bool v91_control_decode(const uint8_t frame[12], bool *retrain, bool *cleardown)
{
	if (frame == NULL || retrain == NULL || cleardown == NULL ||
	    packed_msb_read_msb_field(frame, 0, 16) != 0x77fa ||
	    packed_msb_read_msb_field(frame, 78, 16) != control_crc(frame) ||
	    !packed_msb_bits_are_zero(frame, 94, 2))
		return false;
	*cleardown = packed_msb_bit(frame, 16);
	*retrain = packed_msb_bit(frame, 17);
	return true;
}

void v91_info_receiver_init(struct v91_info_receiver *receiver)
{
	if (receiver != NULL)
		memset(receiver, 0, sizeof(*receiver));
}

bool v91_info_receiver_sample(struct v91_info_receiver *receiver, bool positive,
	struct v91_info *info)
{
	if (receiver == NULL || info == NULL || receiver->position >= V91_INFO_BITS)
		return false;
	uint8_t bit = positive ^ receiver->previous_sign;
	receiver->previous_sign = positive;
	++receiver->clock;
	if (!receiver->position) {
		receiver->sync = ((receiver->sync << 1) | bit) & 0xfffu;
		if (receiver->sync == 0xf72) {
			memset(receiver->frame, 0, sizeof(receiver->frame));
			packed_msb_write_msb_field(receiver->frame, 0, 12, 0xf72);
			receiver->position = 12;
		}
		return false;
	}
	packed_msb_set_bit(receiver->frame, receiver->position++, bit);
	if (receiver->position < V91_INFO_BITS)
		return false;
	receiver->position = 0;
	if (!v91_info_decode(receiver->frame, info)) {
		receiver->consecutive = 0;
		return false;
	}
	bool adjacent = receiver->consecutive &&
		receiver->clock - receiver->last_frame == V91_INFO_BITS &&
		memcmp(receiver->frame, receiver->previous, V91_INFO_BYTES) == 0;
	receiver->consecutive = adjacent ? 2 : 1;
	receiver->last_frame = receiver->clock;
	memcpy(receiver->previous, receiver->frame, V91_INFO_BYTES);
	return true;
}
