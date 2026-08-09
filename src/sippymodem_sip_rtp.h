// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_SIP_RTP_H
#define SIPPYMODEM_SIP_RTP_H

#include "sippymodem_rtp_endpoint.h"
#include "sippymodem_sip_signaling.h"

enum sippymodem_sip_rtp_result {
	SIPPYMODEM_SIP_RTP_OK,
	SIPPYMODEM_SIP_RTP_INVALID_ARGUMENT,
	SIPPYMODEM_SIP_RTP_INCOMPATIBLE_MEDIA,
	SIPPYMODEM_SIP_RTP_UNSUPPORTED_ADDRESS,
	SIPPYMODEM_SIP_RTP_INVALID_ADDRESS,
	SIPPYMODEM_SIP_RTP_ENDPOINT_ERROR,
};

/* Apply one signaling media event without retaining borrowed event data. */
enum sippymodem_sip_rtp_result sippymodem_sip_rtp_apply(
	struct sippymodem_rtp_endpoint *endpoint,
	const struct sippymodem_sip_media_event *event);

const char *sippymodem_sip_rtp_result_name(
	enum sippymodem_sip_rtp_result result);

#endif
