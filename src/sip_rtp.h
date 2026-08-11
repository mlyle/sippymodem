// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIP_RTP_H
#define SIP_RTP_H

#include "rtp_endpoint.h"
#include "sip_signaling.h"

enum sip_rtp_result {
	SIP_RTP_OK,
	SIP_RTP_INVALID_ARGUMENT,
	SIP_RTP_INCOMPATIBLE_MEDIA,
	SIP_RTP_UNSUPPORTED_ADDRESS,
	SIP_RTP_INVALID_ADDRESS,
	SIP_RTP_ENDPOINT_ERROR,
};

/* Apply one signaling media event without retaining borrowed event data. */
enum sip_rtp_result sip_rtp_apply(
	struct rtp_endpoint *endpoint,
	const struct sip_media_event *event);

const char *sip_rtp_result_name(
	enum sip_rtp_result result);

#endif
