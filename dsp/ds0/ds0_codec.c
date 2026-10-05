// SPDX-License-Identifier: BSD-3-Clause
#include "ds0_codec.h"
#include "hdlc_fcs.h"
#include <limits.h>
#include <string.h>
#define DS0_MIN_FCS_FRAME_OCTETS 3u
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

static void ds0_rx_reset_frame(struct ds0_hdlc_rx *rx)
{
	rx->ones = 0;
	rx->bit_count = 0;
	rx->current_byte = 0;
	rx->frame_len = 0;
}

static void ds0_rx_reset_detector(struct ds0_hdlc_rx *rx,
				     enum ds0_rate rate)
{
	*rx = (struct ds0_hdlc_rx) {
		.rate = rate,
	};
}

static void ds0_rx_abort_frame(struct ds0_hdlc_rx *rx)
{
	rx->in_frame = 0;
	ds0_rx_reset_frame(rx);
}

static void ds0_count_fcs_fail(struct ds0 *ds0, enum ds0_rate rate)
{
	if (rate == DS0_RATE_56000)
		ds0->counters.rx56.fcs_fail++;
	else
		ds0->counters.rx64.fcs_fail++;
}

static void ds0_count_short(struct ds0 *ds0, enum ds0_rate rate)
{
	if (rate == DS0_RATE_56000)
		ds0->counters.rx56.short_frame++;
	else
		ds0->counters.rx64.short_frame++;
}

static void ds0_count_invalid(struct ds0 *ds0, enum ds0_rate rate)
{
	if (rate == DS0_RATE_56000)
		ds0->counters.rx56.invalid++;
	else
		ds0->counters.rx64.invalid++;
}

static void ds0_count_overflow(struct ds0 *ds0, enum ds0_rate rate)
{
	if (rate == DS0_RATE_56000)
		ds0->counters.rx56.overflow++;
	else
		ds0->counters.rx64.overflow++;
}

static void ds0_note_valid_frame(struct ds0 *ds0, enum ds0_rate rate)
{
	int was_locked = ds0->saw_valid_frame;

	if (rate == DS0_RATE_56000)
		ds0->counters.rx56.valid++;
	else
		ds0->counters.rx64.valid++;

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
	ds0->counters.auto_switches++;
}

static void ds0_rx_put_data_bit(struct ds0 *ds0,
				   struct ds0_hdlc_rx *rx,
				   unsigned bit)
{
	if (!rx->in_frame)
		return;

	if (bit)
		rx->current_byte = (uint8_t)(rx->current_byte |
					    (uint8_t)(1u << rx->bit_count));
	if (++rx->bit_count < 8)
		return;

	if (rx->frame_len < ds0_rx_storage_limit(ds0)) {
		rx->frame[rx->frame_len++] = rx->current_byte;
	} else {
		ds0_count_overflow(ds0, rx->rate);
		ds0_rx_abort_frame(rx);
		return;
	}
	rx->bit_count = 0;
	rx->current_byte = 0;
}

static void ds0_rx_destuff_bit(struct ds0 *ds0,
				  struct ds0_hdlc_rx *rx,
				  unsigned bit)
{
	if (!rx->in_frame)
		return;

	if (bit) {
		ds0_rx_put_data_bit(ds0, rx, 1);
		if (++rx->ones > 6) {
			ds0_count_invalid(ds0, rx->rate);
			ds0_rx_abort_frame(rx);
		}
		return;
	}

	if (rx->ones == 5) {
		rx->ones = 0;
		return;
	}
	if (rx->ones >= 6) {
		ds0_count_invalid(ds0, rx->rate);
		ds0_rx_abort_frame(rx);
		return;
	}

	ds0_rx_put_data_bit(ds0, rx, 0);
	rx->ones = 0;
}

static int ds0_raw_delay_is_flag(const struct ds0_hdlc_rx *rx)
{
	return memcmp(rx->raw_delay, ds0_hdlc_flag_bits,
		      sizeof(ds0_hdlc_flag_bits)) == 0;
}

static void ds0_rx_finish_frame(struct ds0 *ds0,
				   struct ds0_hdlc_rx *rx,
				   ds0_rx_frame_cb frame_cb,
				   void *user, int *delivered)
{
	if (!rx->in_frame) {
		rx->in_frame = 1;
		ds0_rx_reset_frame(rx);
		return;
	}

	if ((rx->frame_len || rx->bit_count)) {
		if (rx->bit_count) {
			if (rx->frame_len >= DS0_MIN_FCS_FRAME_OCTETS)
				ds0_count_invalid(ds0, rx->rate);
		} else if (rx->frame_len < DS0_MIN_FCS_FRAME_OCTETS) {
			ds0_count_short(ds0, rx->rate);
		} else if (hdlc_fcs16(rx->frame, rx->frame_len) !=
			   HDLC_FCS16_GOOD) {
			ds0_count_fcs_fail(ds0, rx->rate);
		} else {
			size_t body_len = rx->frame_len - 2u;

			ds0_note_valid_frame(ds0, rx->rate);
			if (frame_cb)
				frame_cb(user, rx->frame, body_len, rx->rate);
			if (delivered)
				(*delivered)++;
		}
	}

	rx->in_frame = 1;
	ds0_rx_reset_frame(rx);
}

static void ds0_rx_raw_bit(struct ds0 *ds0,
			      struct ds0_hdlc_rx *rx,
			      unsigned bit, ds0_rx_frame_cb frame_cb,
			      void *user, int *delivered)
{
	uint8_t oldest = 0;
	int have_oldest = 0;

	if (rx->raw_len < DS0_HDLC_FLAG_BITS) {
		rx->raw_delay[rx->raw_len++] = (uint8_t)(bit != 0);
	} else {
		oldest = rx->raw_delay[0];
		memmove(&rx->raw_delay[0], &rx->raw_delay[1],
			sizeof(rx->raw_delay) - 1u);
		rx->raw_delay[DS0_HDLC_FLAG_MASK] = (uint8_t)(bit != 0);
		have_oldest = 1;
	}

	if (rx->raw_len == DS0_HDLC_FLAG_BITS &&
	    ds0_raw_delay_is_flag(rx)) {
		if (have_oldest) {
			if (rx->raw_skip)
				rx->raw_skip--;
			else
				ds0_rx_destuff_bit(ds0, rx, oldest);
		}
		ds0_rx_finish_frame(ds0, rx, frame_cb, user, delivered);
		rx->raw_delay[0] = 0;
		rx->raw_len = 1;
		rx->raw_skip = 1;
		return;
	}

	if (have_oldest) {
		if (rx->raw_skip)
			rx->raw_skip--;
		else
			ds0_rx_destuff_bit(ds0, rx, oldest);
	}
}

static void ds0_maybe_answerer_timeout(struct ds0 *ds0)
{
	if (ds0->cfg.rate_policy != DS0_RATE_AUTO || !ds0->cfg.answerer)
		return;
	if (ds0->saw_valid_frame || ds0->answerer_timeout_done)
		return;
	if (ds0->samples_since_start < DS0_ANSWER_TIMEOUT_SAMPLES)
		return;

	ds0->tx_rate = DS0_RATE_56000;
	ds0->selected_rate = DS0_RATE_56000;
	ds0->answerer_timeout_done = 1;
	ds0->counters.answerer_timeout_fallbacks++;
}

void ds0_advance_samples(struct ds0 *ds0, size_t sample_count)
{
	uint64_t samples;

	if (!ds0)
		return;

	samples = ds0->samples_since_start + (uint64_t)sample_count;
	if (samples < ds0->samples_since_start)
		samples = UINT64_MAX;
	ds0->samples_since_start = samples;
	ds0_maybe_answerer_timeout(ds0);
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
			for (int bit = 7; bit >= 0; --bit) {
				ds0_rx_raw_bit(ds0, &ds0->rx64,
						  (octet >> bit) & 1u,
						  frame_cb, user,
						  &delivered);
			}
		}
		if (ds0_detector_active(ds0, DS0_RATE_56000)) {
			for (int bit = 7; bit >= 1; --bit) {
				ds0_rx_raw_bit(ds0, &ds0->rx56,
						  (octet >> bit) & 1u,
						  frame_cb, user,
						  &delivered);
			}
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
	if (frame_len > DS0_MAX_FRAME_OCTETS)
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
	unsigned ones;
};


static int ds0_tx_emit_bit(struct ds0_tx_writer *wr, unsigned bit)
{
	unsigned bits_per_octet = ds0_rate_bits_per_octet(wr->rate);

	if (!wr->used_bits)
		wr->octet = wr->rate == DS0_RATE_56000 ?
				    (uint8_t)(wr->fill_bit & 1u) :
				    0;

	if (bit)
		wr->octet = (uint8_t)(wr->octet |
				      (uint8_t)(1u << (7u - wr->used_bits)));
	if (++wr->used_bits < bits_per_octet)
		return 0;

	if (wr->len >= wr->capacity)
		return -1;
	wr->out[wr->len++] = wr->octet;
	wr->used_bits = 0;
	wr->octet = 0;
	return 0;
}

static int ds0_tx_emit_flag(struct ds0_tx_writer *wr,
			       unsigned *flag_phase)
{
	for (unsigned i = 0; i < DS0_HDLC_FLAG_BITS; ++i) {
		unsigned phase = flag_phase ? *flag_phase : i;

		if (ds0_tx_emit_bit(wr, ds0_hdlc_flag_bits[phase]) < 0)
			return -1;
		if (flag_phase)
			*flag_phase = (*flag_phase + 1u) &
				      DS0_HDLC_FLAG_MASK;
	}
	wr->ones = 0;
	return 0;
}

static int ds0_tx_emit_data_bit(struct ds0_tx_writer *wr, unsigned bit)
{
	if (ds0_tx_emit_bit(wr, bit) < 0)
		return -1;

	if (bit) {
		if (++wr->ones == 5) {
			if (ds0_tx_emit_bit(wr, 0) < 0)
				return -1;
			wr->ones = 0;
		}
	} else {
		wr->ones = 0;
	}
	return 0;
}

static int ds0_tx_emit_byte(struct ds0_tx_writer *wr, uint8_t byte)
{
	for (unsigned bit = 0; bit < 8; ++bit) {
		if (ds0_tx_emit_data_bit(wr, (byte >> bit) & 1u) < 0)
			return -1;
	}
	return 0;
}

size_t ds0_tx_frame(struct ds0 *ds0, const uint8_t *frame,
		       size_t frame_len, uint8_t *codec_octets,
		       size_t codec_capacity)
{
	struct ds0_tx_writer wr;
	enum ds0_rate rate;
	uint16_t fcs;
	size_t bound;
	unsigned flag_phase = 0;

	if (!ds0 || (!frame && frame_len) || !codec_octets)
		return 0;

	rate = ds0->tx_rate;
	bound = ds0_tx_frame_bound(frame_len, rate);
	if (!bound || codec_capacity < bound)
		return 0;

	memset(&wr, 0, sizeof(wr));
	wr.out = codec_octets;
	wr.capacity = codec_capacity;
	wr.rate = rate;
	wr.fill_bit = ds0->cfg.robbed_bit_fill;

	if (ds0_tx_emit_flag(&wr, NULL) < 0)
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
	if (ds0_tx_emit_flag(&wr, &flag_phase) < 0)
		return 0;

	while (wr.used_bits) {
		if (ds0_tx_emit_bit(&wr,
				       ds0_hdlc_flag_bits[flag_phase]) < 0)
			return 0;
		flag_phase = (flag_phase + 1u) & DS0_HDLC_FLAG_MASK;
	}

	return wr.len;
}

size_t ds0_tx_idle(struct ds0 *ds0, uint8_t *codec_octets,
		      size_t codec_count)
{
	struct ds0_tx_writer wr;
	unsigned flag_phase = 0;

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

	return wr.len;
}

void ds0_rx_gap(struct ds0 *ds0)
{
    if (ds0 == NULL) return;
    ds0_rx_reset_detector(&ds0->rx64, DS0_RATE_64000);
    ds0_rx_reset_detector(&ds0->rx56, DS0_RATE_56000);
    ds0->counters.media_gaps++;
}
