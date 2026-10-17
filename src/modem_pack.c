
/*
 *
 *    Copyright (c) 2002, Smart Link Ltd.
 *    All rights reserved.
 *
 *    Redistribution and use in source and binary forms, with or without
 *    modification, are permitted provided that the following conditions
 *    are met:
 *
 *        1. Redistributions of source code must retain the above copyright
 *           notice, this list of conditions and the following disclaimer.
 *        2. Redistributions in binary form must reproduce the above
 *           copyright notice, this list of conditions and the following
 *           disclaimer in the documentation and/or other materials provided
 *           with the distribution.
 *        3. Neither the name of the Smart Link Ltd. nor the names of its
 *           contributors may be used to endorse or promote products derived
 *           from this software without specific prior written permission.
 *
 *    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *    "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *    A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *    OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/*
 *
 *    modem_pack.c  --  modem packer module.
 *
 *    Author: Sasha K (sashak@smlink.com)
 *
 *
 */

#include <limits.h>
#include <stdint.h>

#include <modem.h>
#include "modem_state.h"
#include "hdlc_fcs.h"
#include "hdlc_tx.h"
#include <modem_debug.h>

#define PACK_DBG(fmt,arg...) dprintf("%s: " fmt , m->name , ##arg)
#define PACK_ERR(fmt,arg...) eprintf("%s: " fmt , m->name , ##arg)

/* HDLC definitions */

#define HDLC_FLAG   0x7e
#define HDLC_ESCAPE (0x1f<<3)


/* bit ordering (reverse) table */
static const u8 _ordered[] = {
        0x0,0x8,0x4,0xc,0x2,0xa,0x6,0xe,
        0x1,0x9,0x5,0xd,0x3,0xb,0x7,0xf};
#define REVERSE_BITS(c) { (c) = _ordered[(c)&0xf]<<4 | _ordered[(c)>>4] ; }


/*
 *    Async packer
 *
 */

enum {
	ASYNC_DATA_BITS = 8,
	ASYNC_FRAME_BITS = 10,
	ASYNC_BREAK_BITS = 2 * ASYNC_FRAME_BITS + 3,
	V42_T400_MS = 750,
	V42_DETECTION_LIMIT_MS = 5 * V42_T400_MS,
};

enum async_receive_state {
	ASYNC_RECEIVE_HUNT_START,
	ASYNC_RECEIVE_DATA,
	ASYNC_RECEIVE_STOP,
	ASYNC_RECEIVE_HUNT_MARK,
};

enum async_detection_residue_state {
	ASYNC_DETECTION_RESIDUE_OFF,
	ASYNC_DETECTION_RESIDUE_WAIT,
	ASYNC_DETECTION_RESIDUE_HELD,
	ASYNC_DETECTION_RESIDUE_CONFIRMED,
};

/* A delayed detection run can continue after V.14 selection.  Hold one
 * matching character until its alternating peer proves that this is protocol
 * residue rather than raw DTE data. */

static void async_deliver_character(struct modem *m, u8 character)
{
	if (m->put_chars != NULL &&
	    m->put_chars(m, (const char *)&character, 1) != 1) {
		m->dte_overflow = true;
		modem_note_disconnect(m, true, "raw DTE receive overflow");
	}
}

static int async_is_detection_character(
	const struct async_state *async, u8 character)
{
	return character == async->detection_residue_pattern[0] ||
		character == async->detection_residue_pattern[1];
}

static void async_receive_character(struct modem *m, u8 character)
{
	struct async_state *async = &m->packer.async;

	switch (async->detection_residue_state) {
	case ASYNC_DETECTION_RESIDUE_WAIT:
		if (async_is_detection_character(async, character)) {
			async->detection_residue_character = character;
			async->detection_residue_state =
				ASYNC_DETECTION_RESIDUE_HELD;
			return;
		}
		async->detection_residue_state = ASYNC_DETECTION_RESIDUE_OFF;
		break;
	case ASYNC_DETECTION_RESIDUE_HELD:
		if (async_is_detection_character(async, character) &&
		    character != async->detection_residue_character) {
			async->detection_residue_state =
				ASYNC_DETECTION_RESIDUE_CONFIRMED;
			return;
		}
		async_deliver_character(m, async->detection_residue_character);
		if (async_is_detection_character(async, character)) {
			async->detection_residue_character = character;
			return;
		}
		async->detection_residue_state = ASYNC_DETECTION_RESIDUE_OFF;
		break;
	case ASYNC_DETECTION_RESIDUE_CONFIRMED:
		if (async_is_detection_character(async, character))
			return;
		async->detection_residue_state = ASYNC_DETECTION_RESIDUE_OFF;
		break;
	default:
		async->detection_residue_state = ASYNC_DETECTION_RESIDUE_OFF;
		break;
	}

	async_deliver_character(m, character);
}

int modem_async_get_bits(struct modem *m, int nbits, u8 *bit_buf, int bit_cnt)
{
	struct bits_state *s = &m->pack;
	struct async_state *async = &m->packer.async;
	int ret = 0;
	u8 ch;

	/*
	 * A peer performing V.42 detection discards data until T400 expires.
	 * Preserve queued raw DTE bytes by sending marks through that interval.
	 */
	while (bit_cnt > 0 && async->transmit_mark_bits > 0) {
		*bit_buf++ = (u8)((1u << nbits) - 1u);
		async->transmit_mark_bits -= nbits;
		bit_cnt--;
		ret++;
	}

	do {
		/* fill bits */
		while ( bit_cnt > 0 && s->bit >= nbits) {
			*bit_buf++ = (s->data>>(s->bit-nbits));
			s->bit -= nbits;
			ret ++;
			bit_cnt--;
		}
		if (bit_cnt <= 0)
			break;
		/* get char */
		if (!m->get_chars || m->get_chars(m, (char*)&ch, 1) != 1)
			break;
		REVERSE_BITS(ch);
		s->data <<= ASYNC_FRAME_BITS;
		s->data |= ((u32)ch << 1) | 1u;
		s->bit += ASYNC_FRAME_BITS;
	} while (1);
	if (s->bit && bit_cnt > 0) {
		/* left data + stops */
		*bit_buf++ = (s->data<<(nbits-s->bit))|((0x1<<(nbits-s->bit))-1);
		ret ++;
		bit_cnt--;
		s->bit = 0;
	}
	while (bit_cnt > 0) {
		/* fill stops */
		*bit_buf++ = (1<<nbits)-1;
		bit_cnt --;
		ret ++;
	}
	return ret;
}


int modem_async_put_bits(struct modem *m, int nbits, u8 *bit_buf, int bit_cnt)
{
	struct async_state *async = &m->packer.async;
	int ret = 0;
	while(bit_cnt > 0) {
		u8 bits = *bit_buf++ & ((0x1<<nbits)-1);

		bit_cnt--;
		ret++;

		for (int index = nbits - 1; index >= 0; --index) {
			unsigned int bit = (bits >> index) & 1u;

			/* V.14 7.3.2: distinguish deleted stops in NULs from break.
			 * At most two NULs can be pending before break is known.
			 * Keep the byte interface's existing break suppression. */
			if (bit == 0) {
				if (async->receive_zero_run < ASYNC_BREAK_BITS)
					async->receive_zero_run++;
				if (async->receive_zero_run == ASYNC_BREAK_BITS) {
					async->receive_pending_nuls = 0;
					async->receive_state = ASYNC_RECEIVE_HUNT_MARK;
					async->receive_bit_count = 0;
					async->receive_data = 0;
					continue;
				}
			} else {
				async->receive_zero_run = 0;
				while (async->receive_pending_nuls) {
					async_receive_character(m, 0);
					async->receive_pending_nuls--;
				}
			}

			switch (async->receive_state) {
			case ASYNC_RECEIVE_HUNT_MARK:
				if (bit != 0)
					async->receive_state =
						ASYNC_RECEIVE_HUNT_START;
				break;
			case ASYNC_RECEIVE_HUNT_START:
				if (bit == 0) {
					async->receive_state = ASYNC_RECEIVE_DATA;
					async->receive_bit_count = 0;
					async->receive_data = 0;
				}
				break;
			case ASYNC_RECEIVE_DATA:
				if (bit != 0)
					async->receive_data |= (u8)(
						1u << async->receive_bit_count);
				async->receive_bit_count++;
				if (async->receive_bit_count == ASYNC_DATA_BITS)
					async->receive_state = ASYNC_RECEIVE_STOP;
				break;
			case ASYNC_RECEIVE_STOP:
				/* V.14 7.1/7.2: a deleted stop is replaced by the
				 * next character's start, already consumed here. */
				if (bit || async->receive_data)
					async_receive_character(m, async->receive_data);
				else
					async->receive_pending_nuls++;
				async->receive_state = bit ? ASYNC_RECEIVE_HUNT_START :
					ASYNC_RECEIVE_DATA;
				async->receive_bit_count = 0;
				async->receive_data = 0;
				break;
			default:
				async->receive_state = ASYNC_RECEIVE_HUNT_START;
				async->receive_bit_count = 0;
				async->receive_data = 0;
				break;
			}
		}
	}
	return ret;
}

static void modem_async_reset_receive(struct modem *m)
{
	m->packer.async.receive_state = ASYNC_RECEIVE_HUNT_START;
	m->packer.async.receive_bit_count = 0;
	m->packer.async.receive_data = 0;
	m->packer.async.receive_zero_run = 0;
	m->packer.async.receive_pending_nuls = 0;
	m->packer.async.detection_residue_state =
		ASYNC_DETECTION_RESIDUE_OFF;
}

void modem_async_retrain(struct modem *m)
{
	modem_async_reset_receive(m);
}

void modem_async_discard_transmit(struct modem *m)
{
	if (m->tx_completion == MODEM_TX_FINISHING)
		m->tx_completion = MODEM_TX_ABORTED;
	m->pack.bit = 0;
	m->pack.data = 0;
}

void modem_async_reframe(struct modem *m)
{
	modem_async_discard_transmit(m);
	modem_async_reset_receive(m);
}

void modem_async_start(struct modem *m)
{
	m->pack.bit  = m->unpack.bit  = 0;
	m->pack.data = m->unpack.data = 0;
	modem_async_reframe(m);
        m->bit_timer_func = NULL;
	m->bit_timer = 0;
	m->packer.async.transmit_mark_bits =
		!m->cfg.ec && !m->cfg.ec_detector &&
		modem_link_supports_v42(m) ?
			(int)(m->tx_rate * V42_T400_MS / 1000u) : 0;
}


/*
 *    Packer detector
 *
 */

/*
 *  Detection phase description.
 *  originator - transmits ODPs until T400 timer is expired
 *               or until ADP is received.
 *
 *  answer - transmits marks(1) until: detect phase termination,
 *           ODP receipt - send at least 10 ADPs,
 *           detection of start protocol phase (continuous flags).
 * 
 *
 */

/* ODP: 0100010001 11111111 0100010011 11111111 */
#define ODP0     0x88
#define ODP1     0x89 
/* ADP v42: 0101000101 11111111 0110000101 11111111 */
/* ADP v14: 0101000101 11111111 0000000001 11111111 */
#define ADP0     0xa2
#define ADP1_V14 0x00
#define ADP1_V42 0xc2

#define DETECTOR_MATCH_CHARACTERS (2 * V42_ORIGINATOR_MATCH_PATTERNS)
#define DETECTOR_FALLBACK_RATE 57600

_Static_assert(V42_ANSWER_REPLY_PATTERNS >=
		       V42_MINIMUM_ANSWER_PATTERNS,
	       "V.42 answer reply is shorter than the protocol minimum");

int modem_link_supports_v42(const struct modem *m)
{
	if (m == NULL || m->dp == NULL)
		return 0;

	switch (m->dp->id) {
	case DP_B103:
	case DP_V21:
	case DP_V23:
	case DP_BAUDOT:
		return 0;
	default:
		return 1;
	}
}

static void detector_set_pattern(u8 pattern[2], u8 first, u8 second)
{
	pattern[0] = first;
	pattern[1] = second;
}

static void detector_finish(struct modem *m);

static void detector_timeout(struct modem *m)
{
	/* Finish the final ODP pair and its mark interval before raw data. */
	if (m->caller) {
		struct detect_state *d = &m->packer.detector;

		d->tx_limit = d->tx_count & ~1;
		if (d->tx_limit == 0)
			d->tx_limit = 2;
		m->bit_timer = 0;
		m->bit_timer_func = NULL;
	} else {
		detector_finish(m);
	}
}

static uint64_t detector_rate(const struct modem *m)
{
	/* The shared bit timer advances on both transmitted and received bits. */
	uint64_t rate = (uint64_t)m->tx_rate + m->rx_rate;

	return rate != 0 ? rate : DETECTOR_FALLBACK_RATE;
}

static void detector_start_adp_reply(struct modem *m)
{
	m->packer.detector.tx_count = 1;
	m->packer.detector.ec_enable = m->cfg.ec != 0;
	m->bit_timer = 0;
	m->bit_timer_func = NULL;
}

static int detector_ec_required(const struct modem *m)
{
	return m->sregs[SREG_EC] == 2 || m->sregs[SREG_EC] == 4 ||
		m->sregs[SREG_EC] == 5;
}

static void detector_finish(struct modem *m)
{
	struct bits_state transmit_tail = m->pack;
	struct bits_state receive_tail = m->unpack;
	u8 residue_pattern[2] = {
		m->packer.detector.rx_pattern[0],
		m->packer.detector.rx_pattern[1],
	};
	/* One received pair can precede a late ODP at the T400 boundary.
	 * It arms the residue filter; four characters still select V.42. */
	int discard_residue = !m->packer.detector.ec_enable &&
		m->packer.detector.rx_count >= 2;
	u8 text[sizeof(m->packer.detector.text.bytes)];
	unsigned int text_length = m->packer.detector.text.confirmed ?
		m->packer.detector.text.length : 0;

	if (text_length != 0)
		memcpy(text, m->packer.detector.text.bytes, text_length);

	PACK_DBG("detector finished.\n");
	m->packer.detector.tx_count = 0;
	m->bit_timer = 0;
	m->bit_timer_func = NULL;
	if (!m->packer.detector.ec_enable && detector_ec_required(m)) {
		modem_note_disconnect(m, true,
			"V.42 not detected while error control is required");
		modem_update_status(m, STATUS_ERROR);
		return;
	}
	m->cfg.ec = m->packer.detector.ec_enable;
	modem_update_status(m,STATUS_PACK_LINK);
	if (!m->cfg.ec && m->get_bits == modem_async_get_bits) {
		m->packer.async.transmit_mark_bits = 0;
		/* Preserve any marks already queued at the framing boundary. */
		m->pack = transmit_tail;
		if (discard_residue) {
			REVERSE_BITS(residue_pattern[0]);
			REVERSE_BITS(residue_pattern[1]);
			m->packer.async.detection_residue_pattern[0] =
				residue_pattern[0];
			m->packer.async.detection_residue_pattern[1] =
				residue_pattern[1];
			m->packer.async.detection_residue_state =
				ASYNC_DETECTION_RESIDUE_WAIT;
		}
		if (text_length != 0)
			for (unsigned int i = 0; i < text_length; ++i)
				async_deliver_character(m, text[i]);
		if (discard_residue || text_length != 0) {
			/* The detector has consumed every complete character. Seed
			 * the raw receiver with its partial next character so a
			 * fragment boundary cannot turn ADP/ODP residue into data. */
			for (int bit = receive_tail.bit - 1; bit >= 0; --bit) {
				u8 value = (receive_tail.data >> bit) & 1u;
				modem_async_put_bits(m, 1, &value, 1);
			}
		}
	}
}

static int detector_accept_pattern_character(struct modem *m,
					      struct detect_state *detector,
					      u8 character)
{
	unsigned int position = (unsigned int)detector->rx_count;

	if (position >= DETECTOR_MATCH_CHARACTERS)
		return 0;
	if ((position & 1u) == 0) {
		if (character == detector->rx_pattern[0]) {
			detector->rx_count++;
			return 0;
		}
	} else {
		if (m->caller && position == 1 &&
		    (character == ADP1_V42 || character == ADP1_V14))
			detector->rx_pattern[1] = character;
		if (character == detector->rx_pattern[1]) {
			detector->rx_count++;
			return detector->rx_count ==
				DETECTOR_MATCH_CHARACTERS;
		}
	}

	detector->rx_count =
		character == detector->rx_pattern[0] ? 1 : 0;
	return 0;
}

static int raw_text_observe(struct raw_text_probe *text, u8 character)
{
	bool newline = character == '\r' || character == '\n';

	if (text->adp_next != 0) {
		if (character == text->adp_next) {
			text->adp_next = character == 'E' ? 'C' : 'E';
			return 0;
		}
		text->adp_next = 0;
	}
	if (!text->confirmed && !newline && character != '\t' &&
	    (character < 0x20 || character > 0x7e)) {
		text->length = 0;
		return 0;
	}
	if (text->length == sizeof(text->bytes))
		return -1;
	text->bytes[text->length++] = character;
	if (text->confirmed)
		return 0;
	/* V.42 ADP decodes as printable E/C characters. Repeated answer
	 * patterns can outlast detection and must not become a raw prompt.
	 * Either pair alignment is possible when the HDLC probe begins. */
	if (text->length >= 4) {
		const u8 *tail = text->bytes + text->length - 4;
		if (!memcmp(tail, "ECEC", 4) || !memcmp(tail, "CECE", 4)) {
			text->adp_next = tail[2];
			text->length = 0;
			return 0;
		}
	}
	/* Require adjacent line endings, a short line, or a printable prompt. */
	if ((newline && (text->length >= 4 ||
	     (text->length >= 2 &&
	      (text->bytes[text->length - 2] == '\r' ||
	       text->bytes[text->length - 2] == '\n')))) || text->length == 32)
		text->confirmed = true;
	return 0;
}

static int detector_observe_text(struct modem *m, u8 character)
{
	struct detect_state *d = &m->packer.detector;
	bool confirmed = d->text.confirmed;

	/* Detection patterns use the opposite bit order to DTE characters. */
	REVERSE_BITS(character);
	if (raw_text_observe(&d->text, character) != 0)
		return -1;
	if (!confirmed && d->text.confirmed) {
		d->rx_count = 0;
		if (m->caller)
			detector_timeout(m);
	}
	return 0;
}

int modem_detector_get_bits(struct modem *m, int nbits, u8 *bit_buf, int bit_cnt)
{
	struct bits_state *s = &m->pack;
	struct detect_state *d = &m->packer.detector;
	int ret = 0;

	while (bit_cnt > 0) {
		/* fill bits */
		while (bit_cnt > 0 && s->bit >= nbits) {
			*bit_buf++ = (u8)(s->data >> (s->bit - nbits)) &
				(u8)(((u32)1 << nbits) - 1u);
			s->bit -= nbits;
			ret++;
			bit_cnt--;
		}
		if (bit_cnt <= 0)
			break;
		if (d->tx_limit != 0 && d->tx_count > d->tx_limit) {
			/* Finish the selected output group with marks.  The detector
			 * changes ownership only after every required ADP bit is on the
			 * wire, independent of receive-side activity. */
			int marks = d->tx_count == d->tx_limit + 1 ?
				9 : nbits - s->bit;

			d->tx_count = d->tx_limit + 2;

			s->data = (s->data << marks) |
				(((u32)1 << marks) - 1u);
			s->bit += marks;
			continue;
		}
		/* Insert the mark interval preceding each pattern character. */
		s->data = (s->data << 9) | 0x1ffu;
		s->bit += 9;
		if (d->tx_count > 0) {
			unsigned int pattern_index =
				((unsigned int)d->tx_count - 1u) & 1u;

			s->data = (s->data << 9) |
				d->tx_pattern[pattern_index];
			d->tx_count++;
			s->bit += 9;
		}
	}
	if (d->tx_limit != 0 &&
	    d->tx_count > d->tx_limit + 1 && s->bit == 0) {
		if (m->bit_timer_func == detector_finish)
			return ret;
		/* A reply that begins while the final ODP drains still belongs
		 * to detection. Allow at most two received pattern intervals
		 * to complete it; the timer callback prevents repeated renewal. */
		if (m->caller && !d->text.confirmed &&
		    d->rx_count > 0 && d->rx_count < DETECTOR_MATCH_CHARACTERS) {
			uint64_t receive_rate = m->rx_rate ? m->rx_rate :
				DETECTOR_FALLBACK_RATE / 2;
			uint64_t ticks = detector_rate(m) *
				V42_DETECTION_MAX_PATTERN_BITS *
				V42_ORIGINATOR_MATCH_PATTERNS;
			ticks = (ticks + receive_rate - 1) / receive_rate;
			m->bit_timer = ticks > INT_MAX ? INT_MAX :
				ticks != 0 ? (int)ticks : 1;
			m->bit_timer_func = detector_finish;
		} else
			detector_finish(m);
	}
	return ret;
}


int modem_detector_put_bits(struct modem *m, int nbits, u8 *bit_buf, int bit_cnt)
{
	struct bits_state *s = &m->unpack;
	struct detect_state *d = &m->packer.detector;
	int ret = 0;
	int finish = 0;
	u8 ch;
	while(bit_cnt > 0) {
		s->data<<= nbits;
		s->data |= *bit_buf++ & ((0x1<<nbits)-1);
		s->bit += nbits;
		bit_cnt --;
		ret++;
		/* Flags are synchronous eight-bit patterns, not async characters.
		 * Require adjacent flags and inspect every newly received bit end. */
		for (int bit = nbits - 1; bit >= 0; --bit) {
			if (((s->data >> bit) & 0xffffu) != 0x7e7eu ||
			    d->ec_enable || d->text.confirmed ||
			    (!m->caller && d->tx_count != 0))
				continue;
			if (!m->caller)
				detector_start_adp_reply(m);
			else {
				d->ec_enable = m->cfg.ec != 0;
				finish = 1;
			}
		}
		while(s->bit && s->data&(0x1<<(s->bit-1))) /* skip marks */
			s->bit--;
		while (s->bit >= 10 ) {
			ch = s->data >> (s->bit-9);
			s->bit-= 10;
			/* V.42 7.2.1 requires correctly received adjacent pattern
			 * characters. A bad stop breaks an unfinished match. */
			if (!(s->data & ((u32)1 << s->bit))) {
				if (!d->text.confirmed)
					d->text = (struct raw_text_probe){0};
				if (d->rx_count < DETECTOR_MATCH_CHARACTERS)
					d->rx_count = 0;
				continue;
			}
			PACK_DBG("rx pattern: 0x%02x.\n",ch);
			if (!d->ec_enable && !finish &&
			    (m->caller || d->tx_count == 0) &&
			    detector_observe_text(m, ch) != 0) {
				modem_update_status(m, STATUS_ERROR);
				return ret;
			}
			if (d->text.confirmed)
				continue;
			if (!detector_accept_pattern_character(m, d, ch))
				continue;

			if (!m->caller) {
				/* Reply with the locally selected V.42 or V.14 mode. */
				detector_start_adp_reply(m);
			} else {
				d->ec_enable = d->rx_pattern[1] == ADP1_V42;
				if (d->ec_enable)
					m->cfg.ec = 1;
				finish = 1;
			}
		}
	}
	/* An answerer's text fallback must wait until this input group is
	 * consumed before replacing the detector union with async framing. */
	if (finish || (!m->caller && d->text.confirmed)) {
		if (m->caller && !d->ec_enable)
			detector_timeout(m);
		else
			detector_finish(m);
	}
	return ret;
}

void modem_detector_start(struct modem *m)
{
	struct detect_state *d = &m->packer.detector;
	unsigned int duration = m->caller || m->cfg.ec ?
		V42_DETECTION_LIMIT_MS : V42_T400_MS;
	uint64_t timeout = detector_rate(m) * duration / 1000;
	d->tx_count = 0; /* mute transmitter */
	d->rx_count = 0;
	d->tx_limit = m->caller ? 0 : 2 * V42_ANSWER_REPLY_PATTERNS;
	d->ec_enable = 0;
	d->text = (struct raw_text_probe){0};
	if (m->caller) {
		detector_set_pattern(d->tx_pattern, ODP0, ODP1);
		detector_set_pattern(d->rx_pattern, ADP0, ADP1_V42);
		d->tx_count = 1;
	}
	else {
		detector_set_pattern(
			d->tx_pattern, ADP0,
			m->cfg.ec ? ADP1_V42 : ADP1_V14);
		detector_set_pattern(d->rx_pattern, ODP0, ODP1);
	}
	/* V.42 7.2.1: start detection immediately. When seeking error control,
	 * allow 3.75 seconds of active detection for delayed ODP/ADP or
	 * protocol flags. A peer can finish training after our carrier is ready,
	 * including after a retrain has consumed most of the initial allowance.
	 * Explicit raw answer mode retains the standard default. This bounded
	 * interoperability allowance never renews on incoming data; a valid
	 * reply completes early. */
	m->bit_timer_func = detector_timeout;
	m->bit_timer = timeout > INT_MAX ? INT_MAX : timeout > 0 ? (int)timeout : 1;
	m->pack.bit  = m->unpack.bit  = 0;
	m->pack.data = 0;
	m->unpack.data = 0xffffu; /* No flag history before this carrier epoch. */
}

void modem_detector_reframe(struct modem *m)
{
	/* Retraining interrupts adjacency and physical bit alignment, not the
	 * selected reply or the suspended detection timer (V.42 7.10/7.11). */
	m->packer.detector.rx_count = 0;
	if (!m->packer.detector.text.confirmed)
		m->packer.detector.text = (struct raw_text_probe){0};
	m->pack.bit = m->unpack.bit = 0;
	m->pack.data = 0;
	m->unpack.data = 0xffffu; /* No flag history before this carrier epoch. */
}


/*
 *    HDLC packer
 *
 */


/* static data */

/* Shared PPP/LAPM reflected FCS, RFC 1662. */
#define INIT_FCS HDLC_FCS16_INITIAL
#define GET_FCS(fcs, c) hdlc_fcs16_update((fcs), (c))

/* hdlc bit generators */



int modem_hdlc_get_bits(struct modem *m, int nbits,
			u8 *bit_buf, int bit_cnt)
{
	struct hdlc_state *h = &m->packer.hdlc;
	register struct bits_state *s = &m->pack;
	register int ret = 0;
	register u8 in, mask = ((1<<nbits) - 1);

	do {
		/* fill bits */
		while ( bit_cnt > 0 && s->bit >= nbits) {
			*bit_buf++ = (s->data>>(s->bit-nbits))&mask;
			s->bit -= nbits;
			ret ++;
			bit_cnt--;
		}
		if (bit_cnt <= 0)
			break;
		if (!h->tx_frame) {
			s->data <<= 8;
			s->data |= HDLC_FLAG;
			s->bit  += 8;
			if (h->tx_preamble_flags > 0 &&
			    --h->tx_preamble_flags == 0) {
				void (*complete)(struct modem *m) =
					h->tx_preamble_complete;

				h->tx_preamble_complete = NULL;
				if (complete != NULL)
					complete(m);
			}
			if (h->get_tx_frame &&
			    (h->tx_frame = h->get_tx_frame(h->framer))) {
				h->tx_frame->fcs = INIT_FCS;
				h->tx_frame->count = 0;
			}
		}
		else if(h->tx_frame->count == h->tx_frame->size) {
			/* end frame */
			/* put fcs */
			h->tx_frame->fcs ^= 0xffff;
			in = h->tx_frame->fcs&0xff;
			s->bit += (int)hdlc_tx_append_octet(&s->data, &h->tx_ones, in);
			in = (h->tx_frame->fcs>>8)&0xff;
			s->bit += (int)hdlc_tx_append_octet(&s->data, &h->tx_ones, in);
			h->tx_ones = 0;
			/* frame complete callback */
			if (h->tx_complete) 
				h->tx_complete(h->framer, h->tx_frame);
			/* reset tx frame */
			h->tx_frame = NULL;
			/* put flags will done automatically */
		}
		else {
			in = h->tx_frame->buf[h->tx_frame->count];
			h->tx_frame->count++;
			/* fixme: in order to speed up
			   fcs calc: copy it to local var */
			h->tx_frame->fcs = GET_FCS(h->tx_frame->fcs,in);
			s->bit += (int)hdlc_tx_append_octet(&s->data, &h->tx_ones, in);
		}
	} while (1);
	return ret;
}


/* Only unestablished, optional LAPM can yield to a peer's raw fallback.
 * A valid HDLC frame permanently ends this probe for the current call. */
static bool hdlc_can_probe_text(const struct modem *m)
{
	return m->state == STATE_EC_ESTAB && !m->call_report.connected &&
	       m->cfg.ec && m->cfg.ec_detector && !detector_ec_required(m) &&
	       !m->packer.hdlc.raw_fallback_disabled;
}

static bool hdlc_observe_raw_text(struct modem *m, unsigned bit)
{
	struct hdlc_state *h = &m->packer.hdlc;
	struct bits_state *bits = &h->raw_bits;

	/* Never interpret a possible HDLC frame as asynchronous characters. */
	if (h->rx.in_frame) {
		*bits = (struct bits_state){0};
		h->raw_text = (struct raw_text_probe){0};
		return false;
	}
	if (bits->bit == 0 && bit)
		return false;
	bits->data = (bits->data << 1) | bit;
	if (++bits->bit != ASYNC_FRAME_BITS)
		return false;
	bits->bit = 0;
	if (!bit) {
		h->raw_text = (struct raw_text_probe){0};
		return false;
	}
	u8 character = (u8)(bits->data >> 1);
	REVERSE_BITS(character);
	if (raw_text_observe(&h->raw_text, character) != 0 ||
	    !h->raw_text.confirmed)
		return false;
	struct raw_text_probe text = h->raw_text;

	m->cfg.ec = 0;
	m->cfg.comp = 0;
	modem_update_status(m, STATUS_PACK_LINK);
	for (unsigned i = 0; i < text.length; ++i)
		async_deliver_character(m, text.bytes[i]);
	return true;
}

int modem_hdlc_put_bits(struct modem *m, int nbits, u8 *bit_buf, int bit_cnt)
{
	if (m == NULL || nbits < 1 || nbits > 8 || bit_cnt < 0 ||
	    (bit_buf == NULL && bit_cnt != 0)) return -1;
	struct hdlc_state *h = &m->packer.hdlc;
	if (h->rx_frame == NULL || h->rx_frame->buf == NULL || h->rx_frame->size <= 0)
		return -1;
	for (int group = 0; group < bit_cnt; ++group) {
		if (!hdlc_can_probe_text(m) &&
		    hdlc_rx_bits(&h->rx, bit_buf[group] & ((1u << nbits) - 1u),
		                 nbits, h->rx_frame->buf, h->rx_frame->size))
			continue;
		for (int bit = nbits - 1; bit >= 0; --bit) {
			unsigned events = hdlc_rx_bit(&h->rx,
				(bit_buf[group] >> bit) & 1u, h->rx_frame->buf, h->rx_frame->size);
			if (events & HDLC_RX_FRAME)
				h->raw_fallback_disabled = true;
			if (hdlc_can_probe_text(m) && hdlc_observe_raw_text(
				    m, (bit_buf[group] >> bit) & 1u)) {
				/* The packer union was replaced. Finish this group in raw
				 * mode, then hand over every remaining input group. */
				for (int tail = bit - 1; tail >= 0; --tail) {
					u8 value = (bit_buf[group] >> tail) & 1u;
					modem_async_put_bits(m, 1, &value, 1);
				}
				return group + 1 + modem_async_put_bits(m, nbits,
					bit_buf + group + 1, bit_cnt - group - 1);
			}
			if (!events) continue;
			if (events & (HDLC_RX_PARTIAL | HDLC_RX_SHORT | HDLC_RX_BAD_FCS |
				HDLC_RX_ABORT | HDLC_RX_OVERFLOW)) {
				PACK_DBG("HDLC rejected: flags=%x\n", events);
			}
			/* Adjacent idle flags finish ADP waiting, while preserving
			 * the required sixteen transmitted flags before XID. */
			if ((events & HDLC_RX_IDLE) && h->tx_preamble_complete != NULL &&
			    h->tx_preamble_flags > V42_PROTOCOL_PREAMBLE_FLAGS)
				h->tx_preamble_flags = V42_PROTOCOL_PREAMBLE_FLAGS;
			if ((events & HDLC_RX_FRAME) && h->rx_complete != NULL) {
				h->rx_frame->count = (int)h->rx.flag_octets - 2;
				h->rx_frame->fcs = HDLC_FCS16_GOOD;
				h->rx_complete(h->framer, h->rx_frame);
			}
		}
	}
	h->rx_frame->count = (int)h->rx.frame_len;
	h->rx_frame->fcs = h->rx.fcs;
	return bit_cnt;
}

static void hdlc_reset_physical_framing(
	struct modem *m, struct hdlc_state *h)
{
	h->tx_frame = NULL;
	h->tx_ones = 0;
	hdlc_rx_reset(&h->rx);
	h->raw_text = (struct raw_text_probe){0};
	h->raw_bits = (struct bits_state){0};
	h->rx_frame->count = 0;
	h->rx_frame->fcs = INIT_FCS;
	m->pack.bit = m->unpack.bit = 0;
	m->pack.data = m->unpack.data = 0;
}

void modem_hdlc_start(struct modem *m)
{
	struct hdlc_state *h = &m->packer.hdlc;
	PACK_DBG("hdlc_start...\n");
	memset(h,0,sizeof(*h));
	/* rx frame */
	h->rx_frame = &h->_rx_frame;
	h->rx_frame->buf  = h->_rx_buf;
	h->rx_frame->size = sizeof(h->_rx_buf);
	hdlc_reset_physical_framing(m, h);
        m->bit_timer_func = NULL;
	m->bit_timer = 0;
}

int modem_hdlc_reframe(struct modem *m)
{
	struct hdlc_state *h;

	if (m == NULL)
		return -1;
	h = &m->packer.hdlc;
	if (h->rx_frame == NULL || h->rx_frame->buf == NULL ||
	    h->rx_frame->size <= 0 ||
	    (h->tx_frame != NULL && h->tx_complete == NULL))
		return -1;
	if (h->tx_frame != NULL) {
		h->tx_complete(h->framer, h->tx_frame);
	}
	hdlc_reset_physical_framing(m, h);
	return 0;
}
