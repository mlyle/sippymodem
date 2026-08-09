// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_sip_media_bridge.h"

#include <string.h>

static void forward_ready(void *context)
{
	struct sippymodem_sip_media_bridge *bridge = context;

	if (bridge->destination.ready == NULL)
		return;
	bridge->callback_depth++;
	bridge->destination.ready(bridge->destination.context);
	bridge->callback_depth--;
}

#define DEFINE_FORWARDER(function_name, member, record_type)                 \
	static void function_name(void *context, const record_type *record)    \
	{                                                                      \
		struct sippymodem_sip_media_bridge *bridge = context;            \
	                                                                       \
		if (bridge->destination.member == NULL)                           \
			return;                                                    \
		bridge->callback_depth++;                                         \
		bridge->destination.member(bridge->destination.context, record);  \
		bridge->callback_depth--;                                         \
	}

DEFINE_FORWARDER(forward_error, error,
		 struct sippymodem_sip_signaling_error)
DEFINE_FORWARDER(forward_trace, trace,
		 struct sippymodem_sip_trace_record)
DEFINE_FORWARDER(forward_registration, registration_changed,
		 struct sippymodem_sip_registration_event)
DEFINE_FORWARDER(forward_call, call_changed,
		 struct sippymodem_sip_call_event)

#undef DEFINE_FORWARDER

static void forward_media(
	void *context, const struct sippymodem_sip_media_event *event)
{
	struct sippymodem_sip_media_bridge *bridge = context;

	bridge->last_binding =
		sippymodem_sip_rtp_apply(bridge->endpoint, event);
	if (bridge->last_binding == SIPPYMODEM_SIP_RTP_OK) {
		bridge->media_configured =
			event->state == SIPPYMODEM_SIP_MEDIA_CONFIGURED;
	} else if (bridge->binding_failures != UINT64_MAX) {
		bridge->binding_failures++;
	}
	if (bridge->destination.media_changed == NULL)
		return;
	bridge->callback_depth++;
	bridge->destination.media_changed(bridge->destination.context, event);
	bridge->callback_depth--;
}

bool sippymodem_sip_media_bridge_init(
	struct sippymodem_sip_media_bridge *bridge,
	struct sippymodem_rtp_endpoint *endpoint,
	const struct sippymodem_sip_signaling_callbacks *destination)
{
	struct sippymodem_sip_media_bridge initialized;

	if (bridge == NULL || endpoint == NULL || endpoint->stream == NULL)
		return false;
	memset(&initialized, 0, sizeof(initialized));
	initialized.endpoint = endpoint;
	if (destination != NULL)
		initialized.destination = *destination;
	initialized.source.context = bridge;
	initialized.source.ready = forward_ready;
	initialized.source.error = forward_error;
	initialized.source.trace = forward_trace;
	initialized.source.registration_changed = forward_registration;
	initialized.source.call_changed = forward_call;
	initialized.source.media_changed = forward_media;
	*bridge = initialized;
	return true;
}

const struct sippymodem_sip_signaling_callbacks *
sippymodem_sip_media_bridge_callbacks(
	const struct sippymodem_sip_media_bridge *bridge)
{
	return bridge != NULL ? &bridge->source : NULL;
}

void sippymodem_sip_media_bridge_clear_media(
	struct sippymodem_sip_media_bridge *bridge)
{
	if (bridge == NULL)
		return;
	sippymodem_rtp_endpoint_clear_remote(bridge->endpoint);
	bridge->media_configured = false;
}

enum sippymodem_sip_rtp_result sippymodem_sip_media_bridge_last_binding(
	const struct sippymodem_sip_media_bridge *bridge)
{
	return bridge != NULL ? bridge->last_binding :
			      SIPPYMODEM_SIP_RTP_INVALID_ARGUMENT;
}

uint64_t sippymodem_sip_media_bridge_binding_failures(
	const struct sippymodem_sip_media_bridge *bridge)
{
	return bridge != NULL ? bridge->binding_failures : 0;
}

bool sippymodem_sip_media_bridge_is_in_callback(
	const struct sippymodem_sip_media_bridge *bridge)
{
	return bridge != NULL && bridge->callback_depth != 0;
}

bool sippymodem_sip_media_bridge_is_configured(
	const struct sippymodem_sip_media_bridge *bridge)
{
	return bridge != NULL && bridge->media_configured;
}
