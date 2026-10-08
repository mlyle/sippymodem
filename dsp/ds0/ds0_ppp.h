// SPDX-License-Identifier: BSD-3-Clause
#ifndef DS0_PPP_H
#define DS0_PPP_H
#include "ds0_codec.h"
#define DS0_MAX_TX_CODEC_OCTETS 4096u
#define DS0_MAX_ASYNC_FRAME_OCTETS (2u * (DS0_MAX_FRAME_OCTETS + 2u) + 2u)

struct ds0_async_host_rx {
	bool in_frame, escaped, frame_error;
	uint32_t accm;
	size_t frame_len;
	uint8_t frame[DS0_MAX_FRAME_OCTETS + 2u];
};
struct ds0_lcp_request {
	size_t length;
	uint32_t accm;
	bool acknowledged, replied;
	/* Identifier, length and options: compare Configure-Ack exactly. */
	uint8_t bytes[DS0_MAX_FRAME_OCTETS - 5u];
};
struct ds0_ppp_counters {
	uint32_t async_tx_valid_frames, async_tx_fcs_fail, async_tx_short;
	uint32_t async_tx_overflow, async_tx_invalid_escape;
	uint32_t async_rx_frames, async_rx_overflow, tx_reencoded;
};
struct ds0_ppp {
	struct ds0 codec;
	struct ds0_async_host_rx async_host_rx;
	struct ds0_ppp_counters counters;
	struct ds0_lcp_request requests[2]; /* synchronous peer, local host */
	uint32_t host_accm;
	bool overflow;
	size_t tx_codec_pending_pos, tx_codec_pending_len;
	size_t rx_async_pending_pos, rx_async_pending_len;
	enum ds0_rate tx_encoding_rate;
	size_t tx_body_len;
	uint8_t tx_body[DS0_MAX_FRAME_OCTETS];
	uint8_t tx_codec_pending[DS0_MAX_TX_CODEC_OCTETS];
	uint8_t rx_async_pending[DS0_MAX_ASYNC_FRAME_OCTETS];
};
int ds0_ppp_init(struct ds0_ppp *, const struct ds0_config *);
/* Exact consumed prefix; keeps frame state across partial writes. Output is
 * padded with HDLC flags. Async input discards raw controls in the default
 * or negotiated receive map; output escapes all controls and flag/escape. */
size_t ds0_ppp_transmit(struct ds0_ppp *, const uint8_t *, size_t,
	uint8_t *, size_t, size_t *used);
/* Same exact prefix contract without idle padding: refill bounded DTE input
 * until a complete frame is available or the caller has no further bytes. */
size_t ds0_ppp_encode(struct ds0_ppp *, const uint8_t *, size_t,
	uint8_t *, size_t, size_t *used);
/* The callback copies a validated synchronous frame. If the previous output
 * is still retained, overflow is explicit and terminal integration must end
 * the call; a real-time carrier cannot pause incoming DS0 samples. */
void ds0_ppp_receive_frame(void *, const uint8_t *, size_t, enum ds0_rate);
size_t ds0_ppp_read(struct ds0_ppp *, uint8_t *, size_t);
#endif
