// SPDX-License-Identifier: BSD-3-Clause
#include "ds0_codec.h"
#include "hdlc_fcs.h"
#include "hdlc_tx.h"
#include "integer_math.h"
#include <string.h>
#define DS0_HDLC_FLAG_BITS 8u
#define DS0_HDLC_FLAG_MASK 7u
static const uint8_t ds0_hdlc_flag_bits[8] = { 0,1,1,1,1,1,1,0 };

void ds0_config_default(struct ds0_config *cfg)
{
    if (cfg != NULL)
        *cfg = (struct ds0_config) { .rx_frame_limit = DS0_MAX_FRAME_OCTETS };
}

int ds0_init(struct ds0 *ds0, const struct ds0_config *cfg)
{
    if (ds0 == NULL) return -1;
    struct ds0_config effective;
    if (cfg != NULL) effective = *cfg;
    else ds0_config_default(&effective);
    if (effective.rate_policy < DS0_RATE_AUTO ||
        effective.rate_policy > DS0_RATE_FORCE_64000 ||
        effective.rx_frame_limit > DS0_MAX_FRAME_OCTETS) return -1;
    effective.robbed_bit_fill &= 1u;
    enum ds0_rate rate = effective.rate_policy == DS0_RATE_FORCE_56000 ?
        DS0_RATE_56000 : DS0_RATE_64000;
    *ds0 = (struct ds0) { .cfg = effective,
        .rx64 = { .rate = DS0_RATE_64000 }, .rx56 = { .rate = DS0_RATE_56000 },
        .tx_rate = rate, .selected_rate = rate };
    return 0;
}

static unsigned ds0_rate_bits_per_octet(enum ds0_rate rate)
{
	return rate == DS0_RATE_56000 ? 7u : 8u;
}

static int ds0_rate_valid(enum ds0_rate rate)
{
	return rate == DS0_RATE_56000 || rate == DS0_RATE_64000;
}

static size_t ds0_rx_storage_limit(const struct ds0 *ds0)
{
	size_t limit = ds0->cfg.rx_frame_limit;

	if (!limit || limit > DS0_MAX_FRAME_OCTETS)
		limit = DS0_MAX_FRAME_OCTETS;
	return limit + 2u;
}

static void ds0_rx_reset_detector(struct ds0_hdlc_rx *rx, enum ds0_rate rate)
{
	*rx = (struct ds0_hdlc_rx){ .rate = rate };
}

static struct ds0_rx_counters *ds0_rx_counts(struct ds0 *ds0, enum ds0_rate rate)
{
	return rate == DS0_RATE_56000 ? &ds0->counters.rx56 : &ds0->counters.rx64;
}

static void ds0_note_valid_frame(struct ds0 *ds0, enum ds0_rate rate)
{
	int was_locked = ds0->saw_valid_frame;

	ds0_rx_counts(ds0, rate)->valid++;

	ds0->saw_valid_frame = 1;
	ds0->selected_rate = rate;

	if (ds0->cfg.rate_policy != DS0_RATE_AUTO)
		return;
	if (!was_locked) {
		if (rate == DS0_RATE_56000)
			ds0_rx_reset_detector(&ds0->rx64,
						 DS0_RATE_64000);
		else
			ds0_rx_reset_detector(&ds0->rx56,
						 DS0_RATE_56000);
	}
	if (ds0->tx_rate == rate)
		return;

	ds0->tx_rate = rate;
	ds0->tx_flag_phase = 0;
	ds0->counters.auto_switches++;
}

static void ds0_rx_raw_bit(struct ds0 *ds0, struct ds0_hdlc_rx *rx,
	unsigned bit, ds0_rx_frame_cb frame_cb, void *user, int *delivered)
{
	if (rx->clean_bits && rx->clean_bits < UINT32_MAX) rx->clean_bits++;
	unsigned events = hdlc_rx_bit(&rx->framing, bit, rx->frame,
		ds0_rx_storage_limit(ds0));
	if (!events) return;
	struct ds0_rx_counters *counts = ds0_rx_counts(ds0, rx->rate);
	if (events & HDLC_RX_ABORT) counts->invalid++;
	if (events & HDLC_RX_OVERFLOW) counts->overflow++;
	if (events & (HDLC_RX_ABORT | HDLC_RX_OVERFLOW)) {
		rx->clean_bits = rx->verified_bits = 0;
		rx->clean_frame = false;
	}
	if (events & HDLC_RX_MARK_IDLE)
		/* The raw lookahead has not yet been classified as idle. */
		rx->verified_bits = rx->clean_bits > rx->framing.raw_len ?
			rx->clean_bits - rx->framing.raw_len : 0;
	if (!(events & HDLC_RX_FLAG)) return;
	if (events & HDLC_RX_FRAME) {
		rx->clean_frame = true;
		ds0_note_valid_frame(ds0, rx->rate);
		if (frame_cb) frame_cb(user, rx->frame, rx->framing.flag_octets - 2u, rx->rate);
		if (delivered) (*delivered)++;
	} else if (!(events & (HDLC_RX_IDLE | HDLC_RX_MARK_IDLE))) {
		if (events & HDLC_RX_BAD_FCS) counts->fcs_fail++;
		if (events & HDLC_RX_SHORT) counts->short_frame++;
		if ((events & HDLC_RX_PARTIAL) &&
		    rx->framing.flag_octets >= HDLC_FCS16_MIN_FRAME_OCTETS) counts->invalid++;
		rx->clean_bits = 8;
		rx->clean_frame = false;
	}
	rx->verified_bits = rx->clean_bits;
}

static void ds0_rx_bits(struct ds0 *ds0, struct ds0_hdlc_rx *rx,
	unsigned bits, unsigned nbits, ds0_rx_frame_cb frame_cb, void *user, int *delivered)
{
	if (hdlc_rx_bits(&rx->framing, bits, nbits, rx->frame, ds0_rx_storage_limit(ds0))) {
		if (rx->clean_bits) {
			uint32_t room = UINT32_MAX - rx->clean_bits;
			rx->clean_bits += nbits < room ? nbits : room;
		}
		return;
	}
	for (int bit = (int)nbits - 1; bit >= 0; --bit)
		ds0_rx_raw_bit(ds0, rx, (bits >> bit) & 1u, frame_cb, user, delivered);
}

static void ds0_maybe_rate_timeout(struct ds0 *ds0)
{
	if (ds0->cfg.rate_policy != DS0_RATE_AUTO)
		return;
	if (ds0->saw_valid_frame || ds0->rate_timeout_done)
		return;
	if (ds0->samples_since_start < DS0_RATE_TIMEOUT_SAMPLES)
		return;

	ds0->tx_rate = DS0_RATE_56000;
	ds0->tx_flag_phase = 0;
	ds0->selected_rate = DS0_RATE_56000;
	ds0->rate_timeout_done = 1;
	ds0->counters.rate_timeout_fallbacks++;
}

void ds0_advance_samples(struct ds0 *ds0, size_t sample_count)
{
	if (!ds0)
		return;

	ds0->samples_since_start = add_u64_saturating(
		ds0->samples_since_start, (uint64_t)sample_count);
	ds0_maybe_rate_timeout(ds0);
}

static int ds0_detector_active(const struct ds0 *ds0,
				  enum ds0_rate rate)
{
	switch (ds0->cfg.rate_policy) {
	case DS0_RATE_FORCE_56000:
		return rate == DS0_RATE_56000;
	case DS0_RATE_FORCE_64000:
		return rate == DS0_RATE_64000;
	case DS0_RATE_AUTO:
	default:
		return !ds0->saw_valid_frame || rate == ds0->selected_rate;
	}
}

int ds0_rx_codec(struct ds0 *ds0, const uint8_t *codec_octets,
		    size_t codec_count, ds0_rx_frame_cb frame_cb,
		    void *user)
{
	int delivered = 0;

	if (!ds0 || (!codec_octets && codec_count))
		return -1;

	for (size_t i = 0; i < codec_count; ++i) {
		uint8_t octet = codec_octets[i];

		if (ds0_detector_active(ds0, DS0_RATE_64000)) {
			ds0_rx_bits(ds0, &ds0->rx64, octet, 8, frame_cb, user, &delivered);
		}
		if (ds0_detector_active(ds0, DS0_RATE_56000)) {
			ds0_rx_bits(ds0, &ds0->rx56, octet >> 1, 7, frame_cb, user, &delivered);
		}
	}

	ds0_advance_samples(ds0, codec_count);
	return delivered;
}

static size_t ds0_tx_frame_bound(size_t frame_len, enum ds0_rate rate)
{
	unsigned bits_per_octet;
	size_t data_octets;
	size_t data_bits;
	size_t stuffed_bits;
	size_t total_bits;

	if (!ds0_rate_valid(rate))
		return 0;
	if (frame_len < HDLC_FCS16_MIN_FRAME_OCTETS - 2u ||
	    frame_len > DS0_MAX_FRAME_OCTETS)
		return 0;

	bits_per_octet = ds0_rate_bits_per_octet(rate);
	data_octets = frame_len + 2u;
	data_bits = data_octets * 8u;
	stuffed_bits = data_bits + data_bits / 5u;
	if (stuffed_bits > SIZE_MAX - 16u - (bits_per_octet - 1u))
		return 0;
	total_bits = 16u + stuffed_bits + (bits_per_octet - 1u);
	return (total_bits + bits_per_octet - 1u) / bits_per_octet;
}

struct ds0_tx_writer {
	uint8_t *out;
	size_t capacity;
	size_t len;
	enum ds0_rate rate;
	uint8_t fill_bit;
	uint8_t octet;
	unsigned used_bits;
	int ones;
};


static int ds0_tx_emit_bits(struct ds0_tx_writer *wr, uint32_t bits, unsigned count)
{
	unsigned bits_per_octet = ds0_rate_bits_per_octet(wr->rate);

	while (count) {
		unsigned room = bits_per_octet - wr->used_bits;
		unsigned take = count < room ? count : room;

		if (!wr->used_bits)
			wr->octet = wr->rate == DS0_RATE_56000 ? wr->fill_bit & 1u : 0;
		count -= take;
		wr->octet |= (uint8_t)(((bits >> count) & ((1u << take) - 1u)) <<
			(8u - wr->used_bits - take));
		wr->used_bits += take;
		if (wr->used_bits == bits_per_octet) {
			if (wr->len >= wr->capacity)
				return -1;
			wr->out[wr->len++] = wr->octet;
			wr->used_bits = 0;
		}
	}
	return 0;
}

static int ds0_tx_emit_bit(struct ds0_tx_writer *wr, unsigned bit)
{
	return ds0_tx_emit_bits(wr, bit, 1);
}

static int ds0_tx_emit_flag(struct ds0_tx_writer *wr)
{
	wr->ones = 0;
	return ds0_tx_emit_bits(wr, 0x7eu, DS0_HDLC_FLAG_BITS);
}

static int ds0_tx_emit_byte(struct ds0_tx_writer *wr, uint8_t byte)
{
	uint32_t packed = 0;
	unsigned count = hdlc_tx_append_octet(&packed, &wr->ones, byte);

	return ds0_tx_emit_bits(wr, packed, count);
}

size_t ds0_tx_frame(struct ds0 *ds0, const uint8_t *frame,
		       size_t frame_len, uint8_t *codec_octets,
		       size_t codec_capacity)
{
	struct ds0_tx_writer wr;
	enum ds0_rate rate;
	uint16_t fcs;
	size_t bound;
	unsigned flag_phase = ds0 != NULL ? ds0->tx_flag_phase : 0;

	if (!ds0 || (!frame && frame_len) || !codec_octets)
		return 0;

	rate = ds0->tx_rate;
	bound = ds0_tx_frame_bound(frame_len, rate);
	if (bound && ds0->tx_flag_phase)
		bound++;
	if (!bound || codec_capacity < bound)
		return 0;

	memset(&wr, 0, sizeof(wr));
	wr.out = codec_octets;
	wr.capacity = codec_capacity;
	wr.rate = rate;
	wr.fill_bit = ds0->cfg.robbed_bit_fill;

	while (flag_phase) {
		if (ds0_tx_emit_bit(&wr, ds0_hdlc_flag_bits[flag_phase]) < 0)
			return 0;
		flag_phase = (flag_phase + 1u) & DS0_HDLC_FLAG_MASK;
	}
	if (ds0_tx_emit_flag(&wr) < 0)
		return 0;

	fcs = hdlc_fcs16(frame, frame_len);
	for (size_t i = 0; i < frame_len; ++i) {
		if (ds0_tx_emit_byte(&wr, frame[i]) < 0)
			return 0;
	}

	fcs = (uint16_t)(fcs ^ 0xffffu);
	if (ds0_tx_emit_byte(&wr, (uint8_t)(fcs & 0xffu)) < 0 ||
	    ds0_tx_emit_byte(&wr, (uint8_t)(fcs >> 8)) < 0)
		return 0;
	if (ds0_tx_emit_flag(&wr) < 0)
		return 0;

	while (wr.used_bits) {
		if (ds0_tx_emit_bit(&wr,
				       ds0_hdlc_flag_bits[flag_phase]) < 0)
			return 0;
		flag_phase = (flag_phase + 1u) & DS0_HDLC_FLAG_MASK;
	}

	ds0->tx_flag_phase = (uint8_t)flag_phase;
	return wr.len;
}

size_t ds0_tx_idle(struct ds0 *ds0, uint8_t *codec_octets,
		      size_t codec_count)
{
	struct ds0_tx_writer wr;
	unsigned flag_phase = ds0 != NULL ? ds0->tx_flag_phase : 0;

	if (!ds0 || (!codec_octets && codec_count))
		return 0;

	memset(&wr, 0, sizeof(wr));
	wr.out = codec_octets;
	wr.capacity = codec_count;
	wr.rate = ds0->tx_rate;
	wr.fill_bit = ds0->cfg.robbed_bit_fill;

	while (wr.len < codec_count) {
		if (ds0_tx_emit_bit(&wr,
				       ds0_hdlc_flag_bits[flag_phase]) < 0)
			return wr.len;
		flag_phase = (flag_phase + 1u) & DS0_HDLC_FLAG_MASK;
	}

	ds0->tx_flag_phase = (uint8_t)flag_phase;
	return wr.len;
}

void ds0_rx_gap(struct ds0 *ds0)
{
    if (ds0 == NULL) return;
    ds0_rx_reset_detector(&ds0->rx64, DS0_RATE_64000);
    ds0_rx_reset_detector(&ds0->rx56, DS0_RATE_56000);
    ds0->counters.media_gaps++;
}
