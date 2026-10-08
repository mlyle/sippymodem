// SPDX-License-Identifier: BSD-3-Clause
#include "ds0_ppp.h"
#include "hdlc_fcs.h"
#include <string.h>
#define DS0_ASYNC_FLAG 0x7eu
#define DS0_ASYNC_ESCAPE 0x7du
#define DS0_ASYNC_ESCAPE_XOR 0x20u

int ds0_ppp_init(struct ds0_ppp *ppp, const struct ds0_config *cfg)
{
    if (ppp == NULL) return -1;
    memset(ppp, 0, sizeof(*ppp));
    ppp->host_accm = UINT32_MAX;
    return ds0_init(&ppp->codec, cfg);
}

static bool lcp_request_map(const uint8_t *options, size_t length,
			    uint32_t *map)
{
	bool found = false;
	*map = UINT32_MAX;
	while (length) {
		if (length < 2u || options[1] < 2u || options[1] > length)
			return false;
		if (options[0] == 2u) {
			if (found || options[1] != 6u)
				return false;
			*map = (uint32_t)options[2] << 24 |
			       (uint32_t)options[3] << 16 |
			       (uint32_t)options[4] << 8 | options[5];
			found = true;
		}
		length -= options[1];
		options += options[1];
	}
	return true;
}

/* RFC 1662 sections 6/7.1: the converter receives the local host's send
 * map, requested by the synchronous peer. RFC 1661 requires exact Acks
 * in both directions before opening. Observe only validated, queued frames;
 * LCP always has its uncompressed address/control and protocol fields. */
static void observe_lcp(struct ds0_ppp *ppp, const uint8_t *frame,
			size_t length, bool from_host)
{
	struct ds0_lcp_request *request = &ppp->requests[from_host];
	const uint8_t *packet;
	size_t packet_length;
	uint32_t map;
	bool opened = ppp->requests[0].acknowledged &&
		      ppp->requests[1].acknowledged;

	if (length < 8u || frame[0] != 0xffu || frame[1] != 3u ||
	    frame[2] != 0xc0u || frame[3] != 0x21u)
		return;
	packet = frame + 4u;
	packet_length = (size_t)packet[2] << 8 | packet[3];
	if (packet_length < 4u || packet_length > length - 4u)
		return;

	if (packet[0] == 1u) {
		if (!lcp_request_map(packet + 4u, packet_length - 4u, &map))
			return;
		if (opened) {
			ppp->requests[0].acknowledged = false;
			ppp->requests[1].acknowledged = false;
		}
		/* A received restart cannot change bytes already queued by the
		 * host. Its own Configure-Request shows the host has restarted. */
		if (from_host)
			ppp->host_accm = UINT32_MAX;
		request->length = packet_length - 1u;
		memcpy(request->bytes, packet + 1u, request->length);
		request->accm = map;
		request->acknowledged = false;
		request->replied = false;
	} else if (packet[0] == 2u) {
		request = &ppp->requests[!from_host];
		if (request->replied || request->length != packet_length - 1u ||
		    memcmp(request->bytes, packet + 1u, request->length))
			return;
		request->replied = true;
		request->acknowledged = true;
		if (ppp->requests[0].acknowledged &&
		    ppp->requests[1].acknowledged)
			ppp->host_accm = ppp->requests[0].accm;
	} else if (packet[0] == 5u || packet[0] == 6u) {
		if (packet[0] == 6u && !from_host && !opened &&
		    ppp->requests[0].acknowledged)
			return; /* Terminate-Ack leaves Ack-Sent unchanged. */
		/* During negotiation a Terminate only resets the Ack states;
		 * the outstanding request and consumed-reply marker survive. */
		for (size_t i = 0; i < 2u; ++i)
			ppp->requests[i].acknowledged = false;
		if (from_host)
			ppp->host_accm = UINT32_MAX;
	} else if (from_host && !opened &&
		   (packet[0] == 3u || packet[0] == 4u)) {
		/* The host rejected a peer request, possibly malformed enough
		 * that this observer did not retain it. */
		ppp->requests[0].acknowledged = false;
	} else if (packet[0] == 7u && packet_length >= 8u && !from_host &&
		   !opened && !ppp->requests[0].acknowledged) {
		/* Code-Reject moves Ack-Rcvd back to Req-Sent; an already
		 * consumed Ack remains a duplicate until the host retransmits. */
		ppp->requests[1].acknowledged = false;
	}
}

static size_t ds0_drain_pending(uint8_t *pending, size_t *pos, size_t *len,
				   uint8_t *out, size_t capacity)
{
	size_t remaining;
	size_t count;

	if (!pending || !pos || !len || !*len || !capacity)
		return 0;

	remaining = *len - *pos;
	count = remaining < capacity ? remaining : capacity;
	memcpy(out, pending + *pos, count);
	*pos += count;
	if (*pos == *len) {
		*pos = 0;
		*len = 0;
	}
	return count;
}

static int ds0_async_emit_octet(uint8_t octet, uint8_t *out,
				 size_t capacity, size_t *len)
{
	/* Escaping all controls is valid for every negotiated ACCM. */
	int escaped = octet < 0x20u || octet == DS0_ASYNC_FLAG ||
		      octet == DS0_ASYNC_ESCAPE;

	if (*len + (escaped ? 2u : 1u) > capacity)
		return -1;

	if (escaped) {
		out[(*len)++] = DS0_ASYNC_ESCAPE;
		out[(*len)++] = (uint8_t)(octet ^ DS0_ASYNC_ESCAPE_XOR);
	} else {
		out[(*len)++] = octet;
	}
	return 0;
}

static size_t ds0_async_encode_frame(const uint8_t *frame,
				     size_t frame_len, uint8_t *out,
				     size_t capacity)
{
	uint16_t fcs;
	size_t len = 0;

	if ((!frame && frame_len) || frame_len > DS0_MAX_FRAME_OCTETS || !out || capacity < 2u)
		return 0;

	out[len++] = DS0_ASYNC_FLAG;
	fcs = (uint16_t)(hdlc_fcs16(frame, frame_len) ^ 0xffffu);
	for (size_t i = 0; i < frame_len + 2u; ++i) {
		uint8_t octet = i < frame_len ? frame[i] :
			(uint8_t)(i == frame_len ? fcs : fcs >> 8);
		if (ds0_async_emit_octet(octet, out, capacity, &len) < 0)
			return 0;
	}

	if (len >= capacity)
		return 0;
	out[len++] = DS0_ASYNC_FLAG;
	return len;
}

static void ds0_async_host_reset(struct ds0_async_host_rx *rx)
{
	rx->escaped = 0;
	rx->frame_error = 0;
	rx->frame_len = 0;
}

static void ds0_async_host_queue_sync_frame(struct ds0_ppp *ds0,
					       const uint8_t *frame,
					       size_t frame_len)
{
	size_t codec_len;

	if (ds0->tx_codec_pending_len) {
		ds0->counters.async_tx_overflow++;
		ds0->overflow = true;
		return;
	}

	codec_len = ds0_tx_frame(&ds0->codec, frame, frame_len,
				    ds0->tx_codec_pending,
				    sizeof(ds0->tx_codec_pending));
	if (!codec_len) {
		ds0->counters.async_tx_overflow++;
		ds0->overflow = true;
		return;
	}

	memcpy(ds0->tx_body, frame, frame_len);
	ds0->tx_body_len = frame_len;
	ds0->tx_encoding_rate = ds0->codec.tx_rate;
	ds0->tx_codec_pending_pos = 0;
	ds0->tx_codec_pending_len = codec_len;
	ds0->counters.async_tx_valid_frames++;
	observe_lcp(ds0, frame, frame_len, true);
}

static void ds0_async_host_finish_frame(struct ds0_ppp *ds0)
{
	struct ds0_async_host_rx *rx = &ds0->async_host_rx;

	if (rx->escaped && !rx->frame_error) {
		ds0->counters.async_tx_invalid_escape++;
		rx->frame_error = 1;
	}

	if (!rx->frame_error && rx->frame_len) {
		if (rx->frame_len < HDLC_FCS16_MIN_FRAME_OCTETS) {
			ds0->counters.async_tx_short++;
		} else if (hdlc_fcs16(rx->frame, rx->frame_len) !=
			   HDLC_FCS16_GOOD) {
			ds0->counters.async_tx_fcs_fail++;
		} else {
			ds0_async_host_queue_sync_frame(
				ds0, rx->frame, rx->frame_len - 2u);
		}
	}

	ds0_async_host_reset(rx);
}

static void ds0_async_host_append(struct ds0_ppp *ds0, uint8_t octet)
{
	struct ds0_async_host_rx *rx = &ds0->async_host_rx;
	size_t limit = DS0_MAX_FRAME_OCTETS + 2u;

	if (rx->frame_error)
		return;
	if (rx->frame_len >= limit) {
		ds0->counters.async_tx_overflow++;
		ds0->overflow = true;
		rx->frame_error = 1;
		rx->frame_len = 0;
		return;
	}

	rx->frame[rx->frame_len++] = octet;
}

static void ds0_async_host_octet(struct ds0_ppp *ds0, uint8_t octet)
{
	struct ds0_async_host_rx *rx = &ds0->async_host_rx;

	if (octet == DS0_ASYNC_FLAG) {
		if (rx->in_frame) ds0_async_host_finish_frame(ds0);
		rx->in_frame = true;
		return;
	}

	if (!rx->in_frame || rx->frame_error)
		return;
	/* Remove mapped raw controls before unescaping, including controls
	 * injected between an escape and its following octet (RFC 1662 4.2).
	 * Freeze the policy at the first body byte across partial writes. */
	uint32_t map = rx->frame_len || rx->escaped ? rx->accm : ds0->host_accm;
	if (octet < 32u && (map & (UINT32_C(1) << octet)))
		return;
	rx->accm = map;

	if (rx->escaped) {
		rx->escaped = 0;
		ds0_async_host_append(
			ds0, (uint8_t)(octet ^ DS0_ASYNC_ESCAPE_XOR));
		return;
	}

	if (octet == DS0_ASYNC_ESCAPE) {
		rx->escaped = 1;
		return;
	}

	ds0_async_host_append(ds0, octet);
}

static size_t convert(struct ds0_ppp *ds0, const uint8_t *async_octets,
		       size_t async_count, uint8_t *codec_octets,
		       size_t codec_capacity, size_t *async_used, bool pad_idle)
{
	size_t produced = 0;
	size_t consumed = 0;

	if (async_used)
		*async_used = 0;
	if (!ds0 || (!async_octets && async_count) ||
	    (!codec_octets && codec_capacity))
		return 0;

	/* An automatic rate choice can arrive in the middle of a queued frame.
     * Restart that owned frame at the selected rate, preserving its DTE
     * payload rather than sending the old codeword packing at a new rate. */
    if (ds0->tx_codec_pending_len && ds0->tx_encoding_rate != ds0->codec.tx_rate) {
        ds0->tx_codec_pending_len = ds0_tx_frame(&ds0->codec, ds0->tx_body,
            ds0->tx_body_len, ds0->tx_codec_pending, sizeof(ds0->tx_codec_pending));
        ds0->tx_codec_pending_pos = 0;
        ds0->tx_encoding_rate = ds0->codec.tx_rate;
        ds0->counters.tx_reencoded++;
    }
	produced += ds0_drain_pending(
		ds0->tx_codec_pending, &ds0->tx_codec_pending_pos,
		&ds0->tx_codec_pending_len, codec_octets, codec_capacity);
	if (produced == codec_capacity)
		return produced;

	while (consumed < async_count && !ds0->overflow && !ds0->tx_codec_pending_len) {
		ds0_async_host_octet(ds0, async_octets[consumed++]);
		produced += ds0_drain_pending(
			ds0->tx_codec_pending, &ds0->tx_codec_pending_pos,
			&ds0->tx_codec_pending_len, codec_octets + produced,
			codec_capacity - produced);
		if (produced == codec_capacity)
			break;
	}

	if (async_used)
		*async_used = consumed;
	if (pad_idle && produced < codec_capacity && !ds0->tx_codec_pending_len)
		produced += ds0_tx_idle(&ds0->codec, codec_octets + produced,
					   codec_capacity - produced);
	return produced;
}

size_t ds0_ppp_encode(struct ds0_ppp *p, const uint8_t *input, size_t n,
	uint8_t *out, size_t capacity, size_t *used)
{
	return convert(p, input, n, out, capacity, used, false);
}

size_t ds0_ppp_transmit(struct ds0_ppp *p, const uint8_t *input, size_t n,
	uint8_t *out, size_t capacity, size_t *used)
{
	return convert(p, input, n, out, capacity, used, true);
}

void ds0_ppp_receive_frame(void *user, const uint8_t *frame,
				  size_t len, enum ds0_rate rate)
{
	struct ds0_ppp *ds0 = user;
	size_t async_len;

	(void)rate;

	if (!ds0 || len < HDLC_FCS16_MIN_FRAME_OCTETS - 2u)
		return;
	if (ds0->rx_async_pending_len) {
		ds0->counters.async_rx_overflow++;
		ds0->overflow = true;
		return;
	}

	async_len = ds0_async_encode_frame(frame, len, ds0->rx_async_pending,
					  sizeof(ds0->rx_async_pending));
	if (!async_len) {
		ds0->counters.async_rx_overflow++;
		ds0->overflow = true;
		return;
	}

	ds0->rx_async_pending_pos = 0;
	ds0->rx_async_pending_len = async_len;
	ds0->counters.async_rx_frames++;
	observe_lcp(ds0, frame, len, false);
}

size_t ds0_ppp_read(struct ds0_ppp *ppp, uint8_t *out, size_t capacity)
{
    if (ppp == NULL || (out == NULL && capacity)) return 0;
    return ds0_drain_pending(ppp->rx_async_pending, &ppp->rx_async_pending_pos,
        &ppp->rx_async_pending_len, out, capacity);
}
