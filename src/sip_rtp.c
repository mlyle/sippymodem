// SPDX-License-Identifier: BSD-3-Clause

#include "sip_rtp.h"

#include <arpa/inet.h>
#include <stdint.h>
#include <string.h>

static bool media_is_compatible(
	const struct rtp_endpoint *endpoint,
	const struct sip_media_event *event)
{
	const struct rtp_stream *stream = endpoint->stream;

	return stream != NULL && event->codec == stream->codec &&
	       event->payload_type == stream->receiver.config.payload_type &&
	       event->sample_rate == MEDIA_SAMPLE_RATE &&
	       event->channels == 1 && event->local_port != 0 &&
	       event->local_port ==
		       rtp_endpoint_local_port(endpoint);
}

enum sip_rtp_result sip_rtp_apply(
	struct rtp_endpoint *endpoint,
	const struct sip_media_event *event)
{
	struct sockaddr_in remote = {
		.sin_family = AF_INET,
	};
	enum rtp_endpoint_result endpoint_result;

	if (endpoint == NULL || event == NULL)
		return SIP_RTP_INVALID_ARGUMENT;
	if (event->state == SIP_MEDIA_INACTIVE) {
		rtp_endpoint_clear_remote(endpoint);
		return SIP_RTP_OK;
	}
	if (event->state != SIP_MEDIA_CONFIGURED ||
	    event->remote_address == NULL || event->remote_address[0] == '\0' ||
	    event->remote_port == 0 || event->remote_port > UINT16_MAX)
		return SIP_RTP_INVALID_ARGUMENT;
	if (rtp_endpoint_fd(endpoint) < 0)
		return SIP_RTP_ENDPOINT_ERROR;
	if (!media_is_compatible(endpoint, event))
		return SIP_RTP_INCOMPATIBLE_MEDIA;
	if (event->remote_address_family ==
	    SIP_MEDIA_ADDRESS_IPV6)
		return SIP_RTP_UNSUPPORTED_ADDRESS;
	if (event->remote_address_family !=
	    SIP_MEDIA_ADDRESS_IPV4)
		return SIP_RTP_INVALID_ARGUMENT;
	if (inet_pton(AF_INET, event->remote_address, &remote.sin_addr) != 1)
		return SIP_RTP_INVALID_ADDRESS;
	remote.sin_port = htons((uint16_t)event->remote_port);
	endpoint_result = rtp_endpoint_set_remote(endpoint, &remote);
	return endpoint_result == RTP_ENDPOINT_OK ?
		       SIP_RTP_OK :
		       SIP_RTP_ENDPOINT_ERROR;
}

const char *sip_rtp_result_name(
	enum sip_rtp_result result)
{
	switch (result) {
	case SIP_RTP_OK:
		return "ok";
	case SIP_RTP_INVALID_ARGUMENT:
		return "invalid argument";
	case SIP_RTP_INCOMPATIBLE_MEDIA:
		return "negotiated media does not match the RTP endpoint";
	case SIP_RTP_UNSUPPORTED_ADDRESS:
		return "RTP address family is not supported";
	case SIP_RTP_INVALID_ADDRESS:
		return "RTP address is invalid";
	case SIP_RTP_ENDPOINT_ERROR:
		return "RTP endpoint rejected the media event";
	}
	return "unknown SIP RTP result";
}
