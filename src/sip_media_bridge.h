// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIP_MEDIA_BRIDGE_H
#define SIP_MEDIA_BRIDGE_H

#include "sip_rtp.h"

#include <stdbool.h>
#include <stdint.h>

struct sip_media_bridge {
	struct rtp_endpoint *endpoint;
	struct sip_signaling_callbacks destination;
	struct sip_signaling_callbacks source;
	enum sip_rtp_result last_binding;
	uint64_t binding_failures;
	uint64_t trace_time_ns;
	struct rtp_endpoint_stats trace_endpoint;
	struct rtp_receiver_stats trace_receiver;
	unsigned int callback_depth;
	bool media_configured;
	bool trace_started;
	bool inactive_pending;
	bool receive_draining;
};

/* endpoint and copied callback context must outlive the bridge. */
bool sip_media_bridge_init(
	struct sip_media_bridge *bridge,
	struct rtp_endpoint *endpoint,
	const struct sip_signaling_callbacks *destination);

/* Borrowed callback table to pass to the signaling adapter. */
const struct sip_signaling_callbacks *
sip_media_bridge_callbacks(
	const struct sip_media_bridge *bridge);
/* Resolve deferred inactivity; true starts one bounded socket drain. */
bool sip_media_bridge_resolve_inactive(struct sip_media_bridge *bridge);
void sip_media_bridge_clear_media(
	struct sip_media_bridge *bridge);
/* Bounded, once-per-second media diagnostics through the existing trace sink.
 * now_ns is the host's monotonic clock, not the remote RTP clock. */
void sip_media_bridge_trace_media(struct sip_media_bridge *bridge, uint64_t now_ns);
enum sip_rtp_result sip_media_bridge_last_binding(
	const struct sip_media_bridge *bridge);
uint64_t sip_media_bridge_binding_failures(
	const struct sip_media_bridge *bridge);
bool sip_media_bridge_is_in_callback(
	const struct sip_media_bridge *bridge);
bool sip_media_bridge_is_configured(
	const struct sip_media_bridge *bridge);

#endif
