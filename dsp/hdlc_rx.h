// SPDX-License-Identifier: BSD-3-Clause
#ifndef HDLC_RX_H
#define HDLC_RX_H

#include "hdlc_fcs.h"
#include <stdbool.h>

/* Wire-order input, LSB-first octets, RFC 1662 section 5. The owner keeps the
 * same bounded frame storage until a flag or reset; no pointer is retained. */
struct hdlc_rx {
	bool in_frame;
	uint8_t raw_bits, raw_len, raw_skip, ones, bit_count, current_byte;
	size_t frame_len, flag_octets;
	uint16_t fcs;
};

enum hdlc_rx_flags {
	HDLC_RX_FLAG = 1u,
	HDLC_RX_ACQUIRE = 2u,
	HDLC_RX_IDLE = 4u,
	HDLC_RX_FRAME = 8u,
	HDLC_RX_PARTIAL = 16u,
	HDLC_RX_SHORT = 32u,
	HDLC_RX_BAD_FCS = 64u,
	HDLC_RX_ABORT = 128u,
	HDLC_RX_OVERFLOW = 256u,
};

/* Events may coincide when the last delayed bit overflows storage before a
 * flag. On FLAG, flag_octets includes FCS; on FRAME, the owner can read that
 * many octets from its storage until the next step changes it. */

static inline void hdlc_rx_clear_frame(struct hdlc_rx *rx)
{
	rx->ones = rx->bit_count = rx->current_byte = 0;
	rx->frame_len = 0;
	rx->fcs = HDLC_FCS16_INITIAL;
}

static inline void hdlc_rx_reset(struct hdlc_rx *rx)
{
	*rx = (struct hdlc_rx){ .fcs = HDLC_FCS16_INITIAL };
}

static inline unsigned hdlc_rx_abort(struct hdlc_rx *rx, unsigned reason)
{
	rx->in_frame = false;
	hdlc_rx_clear_frame(rx);
	return reason;
}

static inline unsigned hdlc_rx_append_bit(struct hdlc_rx *rx, unsigned bit,
	uint8_t *frame, size_t capacity)
{
	if (bit) rx->current_byte |= (uint8_t)(1u << rx->bit_count);
	if (++rx->bit_count < 8) return 0;
	if (rx->frame_len >= capacity)
		return hdlc_rx_abort(rx, HDLC_RX_OVERFLOW);
	frame[rx->frame_len++] = rx->current_byte;
	rx->fcs = hdlc_fcs16_update(rx->fcs, rx->current_byte);
	rx->bit_count = rx->current_byte = 0;
	return 0;
}

static inline unsigned hdlc_rx_destuff_bit(struct hdlc_rx *rx, unsigned bit,
	uint8_t *frame, size_t capacity)
{
	if (!rx->in_frame) return 0;
	if (bit) {
		unsigned events = hdlc_rx_append_bit(rx, 1, frame, capacity);
		if (++rx->ones > 6) events |= hdlc_rx_abort(rx, HDLC_RX_ABORT);
		return events;
	}
	if (rx->ones == 5) { rx->ones = 0; return 0; }
	if (rx->ones >= 6) return hdlc_rx_abort(rx, HDLC_RX_ABORT);
	unsigned events = hdlc_rx_append_bit(rx, 0, frame, capacity);
	rx->ones = 0;
	return events;
}

static inline unsigned hdlc_rx_finish(struct hdlc_rx *rx)
{
	unsigned events = HDLC_RX_FLAG;
	rx->flag_octets = rx->frame_len;
	if (!rx->in_frame) events |= HDLC_RX_ACQUIRE;
	else if (rx->bit_count) events |= HDLC_RX_PARTIAL;
	else if (!rx->frame_len) events |= HDLC_RX_IDLE;
	else if (rx->frame_len < HDLC_FCS16_MIN_FRAME_OCTETS) events |= HDLC_RX_SHORT;
	else if (rx->fcs != HDLC_FCS16_GOOD) events |= HDLC_RX_BAD_FCS;
	else events |= HDLC_RX_FRAME;
	rx->in_frame = true;
	hdlc_rx_clear_frame(rx);
	return events;
}

static inline unsigned hdlc_rx_bit(struct hdlc_rx *rx,
	unsigned bit, uint8_t *frame, size_t capacity)
{
	unsigned oldest = rx->raw_bits >> 7;
	bool have_oldest = rx->raw_len == 8;
	if (!have_oldest) rx->raw_len++;
	rx->raw_bits = (uint8_t)((rx->raw_bits << 1) | (bit != 0));
	unsigned events = 0;
	if (have_oldest) {
		if (rx->raw_skip) rx->raw_skip--;
		else events = hdlc_rx_destuff_bit(rx, oldest, frame, capacity);
	}
	if (rx->raw_len == 8 && rx->raw_bits == 0x7e) {
		events |= hdlc_rx_finish(rx);
		/* Keep the closing zero for peers sharing a flag boundary. Do not
		 * feed that zero to the next frame's destuffer. */
		rx->raw_bits = 0;
		rx->raw_len = rx->raw_skip = 1;
	}
	return events;
}

/* Consume 1..8 wire bits (MSB first) together only when neither a flag nor
 * stuffing nor overflow is possible. Otherwise leave all state untouched. */
static inline bool hdlc_rx_bits(struct hdlc_rx *rx, unsigned bits, unsigned nbits,
	uint8_t *frame, size_t capacity)
{
	if (!rx->in_frame || rx->raw_len != 8 || rx->raw_skip ||
	    rx->frame_len >= capacity) return false;
	unsigned wire = ((unsigned)rx->raw_bits << nbits) | bits;
	unsigned six = wire & (wire >> 1) & (wire >> 2) & (wire >> 3) &
		(wire >> 4) & (wire >> 5);
	/* A six-one run bounded by zeros is a flag ending in this group. */
	if (six & ~(wire >> 6) & ~(wire << 1) & ((2u << nbits) - 2u)) return false;
	unsigned delayed = (rx->raw_bits >> (8 - nbits)) |
		(((1u << rx->ones) - 1u) << nbits);
	if (delayed & (delayed >> 1) & (delayed >> 2) &
	    (delayed >> 3) & (delayed >> 4)) return false;
	unsigned data = rx->raw_bits;
	data = ((data & 0x55u) << 1) | ((data >> 1) & 0x55u);
	data = ((data & 0x33u) << 2) | ((data >> 2) & 0x33u);
	data = ((data & 0x0fu) << 4) | ((data >> 4) & 0x0fu);
	data = ((data & ((1u << nbits) - 1u)) << rx->bit_count) | rx->current_byte;
	rx->bit_count += nbits;
	if (rx->bit_count >= 8) {
		frame[rx->frame_len++] = (uint8_t)data;
		rx->fcs = hdlc_fcs16_update(rx->fcs, (uint8_t)data);
		rx->bit_count -= 8;
		data >>= 8;
	}
	rx->current_byte = (uint8_t)data;
	rx->ones = 0;
	while (delayed & 1u) { rx->ones++; delayed >>= 1; }
	rx->raw_bits = (uint8_t)wire;
	return true;
}

#endif
