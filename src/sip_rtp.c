// SPDX-License-Identifier: BSD-3-Clause

#include "sip_rtp.h"

#include <arpa/inet.h>
#include <stdint.h>
#include <string.h>

static bool media_is_compatible(
	const struct sippymodem_rtp_endpoint *endpoint,
	const struct sippymodem_sip_media_event *event)
{
	const struct sippymodem_rtp_stream *stream = endpoint->stream;

	return stream != NULL && event->codec == stream->codec &&
	       event->payload_type == stream->receiver.config.payload_type &&
	       event->sample_rate == SIPPYMODEM_MEDIA_SAMPLE_RATE &&
	       event->channels == 1 && event->local_port != 0 &&
	       event->local_port ==
		       sippymodem_rtp_endpoint_local_port(endpoint);
}

enum sippymodem_sip_rtp_result sippymodem_sip_rtp_apply(
	struct sippymodem_rtp_endpoint *endpoint,
	const struct sippymodem_sip_media_event *event)
{
	struct sockaddr_in remote = {
		.sin_family = AF_INET,
	};
	enum sippymodem_rtp_endpoint_result endpoint_result;

	if (endpoint == NULL || event == NULL)
		return SIPPYMODEM_SIP_RTP_INVALID_ARGUMENT;
	if (event->state == SIPPYMODEM_SIP_MEDIA_INACTIVE) {
		sippymodem_rtp_endpoint_clear_remote(endpoint);
		return SIPPYMODEM_SIP_RTP_OK;
	}
	if (event->state != SIPPYMODEM_SIP_MEDIA_CONFIGURED ||
	    event->remote_address == NULL || event->remote_address[0] == '\0' ||
	    event->remote_port == 0 || event->remote_port > UINT16_MAX)
		return SIPPYMODEM_SIP_RTP_INVALID_ARGUMENT;
	if (sippymodem_rtp_endpoint_fd(endpoint) < 0)
		return SIPPYMODEM_SIP_RTP_ENDPOINT_ERROR;
	if (!media_is_compatible(endpoint, event))
		return SIPPYMODEM_SIP_RTP_INCOMPATIBLE_MEDIA;
	if (event->remote_address_family ==
	    SIPPYMODEM_SIP_MEDIA_ADDRESS_IPV6)
		return SIPPYMODEM_SIP_RTP_UNSUPPORTED_ADDRESS;
	if (event->remote_address_family !=
	    SIPPYMODEM_SIP_MEDIA_ADDRESS_IPV4)
		return SIPPYMODEM_SIP_RTP_INVALID_ARGUMENT;
	if (inet_pton(AF_INET, event->remote_address, &remote.sin_addr) != 1)
		return SIPPYMODEM_SIP_RTP_INVALID_ADDRESS;
	remote.sin_port = htons((uint16_t)event->remote_port);
	endpoint_result = sippymodem_rtp_endpoint_set_remote(endpoint, &remote);
	return endpoint_result == SIPPYMODEM_RTP_ENDPOINT_OK ?
		       SIPPYMODEM_SIP_RTP_OK :
		       SIPPYMODEM_SIP_RTP_ENDPOINT_ERROR;
}

const char *sippymodem_sip_rtp_result_name(
	enum sippymodem_sip_rtp_result result)
{
	switch (result) {
	case SIPPYMODEM_SIP_RTP_OK:
		return "ok";
	case SIPPYMODEM_SIP_RTP_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_SIP_RTP_INCOMPATIBLE_MEDIA:
		return "negotiated media does not match the RTP endpoint";
	case SIPPYMODEM_SIP_RTP_UNSUPPORTED_ADDRESS:
		return "RTP address family is not supported";
	case SIPPYMODEM_SIP_RTP_INVALID_ADDRESS:
		return "RTP address is invalid";
	case SIPPYMODEM_SIP_RTP_ENDPOINT_ERROR:
		return "RTP endpoint rejected the media event";
	}
	return "unknown SIP RTP result";
}
