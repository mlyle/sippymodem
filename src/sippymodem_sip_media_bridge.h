// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_SIP_MEDIA_BRIDGE_H
#define SIPPYMODEM_SIP_MEDIA_BRIDGE_H

#include "sippymodem_sip_rtp.h"

#include <stdbool.h>
#include <stdint.h>

struct sippymodem_sip_media_bridge {
	struct sippymodem_rtp_endpoint *endpoint;
	struct sippymodem_sip_signaling_callbacks destination;
	struct sippymodem_sip_signaling_callbacks source;
	enum sippymodem_sip_rtp_result last_binding;
	uint64_t binding_failures;
	unsigned int callback_depth;
};

/* endpoint and copied callback context must outlive the bridge. */
bool sippymodem_sip_media_bridge_init(
	struct sippymodem_sip_media_bridge *bridge,
	struct sippymodem_rtp_endpoint *endpoint,
	const struct sippymodem_sip_signaling_callbacks *destination);

/* Borrowed callback table to pass to the signaling adapter. */
const struct sippymodem_sip_signaling_callbacks *
sippymodem_sip_media_bridge_callbacks(
	const struct sippymodem_sip_media_bridge *bridge);
enum sippymodem_sip_rtp_result sippymodem_sip_media_bridge_last_binding(
	const struct sippymodem_sip_media_bridge *bridge);
uint64_t sippymodem_sip_media_bridge_binding_failures(
	const struct sippymodem_sip_media_bridge *bridge);
bool sippymodem_sip_media_bridge_is_in_callback(
	const struct sippymodem_sip_media_bridge *bridge);

#endif
