// SPDX-License-Identifier: BSD-3-Clause

#include "sip_media_bridge.h"

#include "net_socket.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static bool forward_admit(void *context)
{
	struct sip_media_bridge *bridge = context;
	if (bridge->deferred || bridge->endpoint->discard_pending || bridge->inactive_pending ||
	    bridge->receive_draining)
		return false;
	if (bridge->destination.admit == NULL)
		return true;
	bridge->callback_depth++;
	bool allowed = bridge->destination.admit(bridge->destination.context);
	bridge->callback_depth--;
	return allowed;
}

static void forward_ready(void *context)
{
	struct sip_media_bridge *bridge = context;

	if (bridge->destination.ready == NULL)
		return;
	bridge->callback_depth++;
	bridge->destination.ready(bridge->destination.context);
	bridge->callback_depth--;
}

#define DEFINE_FORWARDER(function_name, member, record_type)                 \
	static void function_name(void *context, const record_type *record)    \
	{                                                                      \
		struct sip_media_bridge *bridge = context;            \
	                                                                       \
		if (bridge->destination.member == NULL)                           \
			return;                                                    \
		bridge->callback_depth++;                                         \
		bridge->destination.member(bridge->destination.context, record);  \
		bridge->callback_depth--;                                         \
	}

DEFINE_FORWARDER(forward_error, error,
		 struct sip_signaling_error)
DEFINE_FORWARDER(forward_trace, trace,
		 struct sip_trace_record)
DEFINE_FORWARDER(forward_registration, registration_changed,
		 struct sip_registration_event)
DEFINE_FORWARDER(forward_media_changed, media_changed,
		 struct sip_media_event)

#undef DEFINE_FORWARDER

static void deliver_call(void *context, const struct sip_call_event *event)
{
	struct sip_media_bridge *bridge = context;

	bridge->call_state = event->state;
	if (bridge->destination.call_changed == NULL)
		return;
	bridge->callback_depth++;
	bridge->destination.call_changed(bridge->destination.context, event);
	bridge->callback_depth--;
}

/* The mailbox has no borrowed callback strings and is touched only by the
 * SIP owner. The worker continues using the established binding unchanged. */
static bool copy_text(char storage[256], const char **text)
{
	if (*text == NULL)
		return true;
	size_t length = strnlen(*text, 256);
	if (length == 256)
		return false;
	memcpy(storage, *text, length + 1);
	*text = storage;
	return true;
}

static struct sip_media_pending *pending(struct sip_media_bridge *bridge)
{
	if (bridge->pending_count == SIP_MEDIA_PENDING_CAPACITY) {
		bridge->pending_overflow = true;
		return NULL;
	}
	return &bridge->pending[bridge->pending_count];
}

static void forward_call(void *context, const struct sip_call_event *event)
{
	struct sip_media_bridge *bridge = context;
	if (!bridge->deferred) {
		deliver_call(context, event);
		return;
	}
	struct sip_media_pending *record = pending(bridge);
	if (record == NULL)
		return;
	record->media = false;
	record->call = *event;
	if (!copy_text(record->text[0], &record->call.called_number) ||
	    !copy_text(record->text[1], &record->call.caller_number) ||
	    !copy_text(record->text[2], &record->call.reason) ||
	    !copy_text(record->text[3], &record->call.remote_identity)) {
		bridge->pending_overflow = true;
		return;
	}
	bridge->pending_count++;
}

static void forward_media(
	void *context, const struct sip_media_event *event)
{
	struct sip_media_bridge *bridge = context;

	if (bridge->deferred) {
		struct sip_media_pending *record = pending(bridge);
		if (record == NULL)
			return;
		record->media = true;
		record->binding = *event;
		if (!copy_text(record->text[0], &record->binding.remote_address)) {
			bridge->pending_overflow = true;
			return;
		}
		bridge->pending_count++;
		return;
	}

	/* SDK owners decide after the following call-IDLE callback. A hold or
	 * local cancellation therefore still clears media without a receive tail. */
	if (event->state == SIP_MEDIA_INACTIVE && bridge->media_configured &&
	    bridge->destination.drain_received_media != NULL) {
		bridge->last_binding = SIP_RTP_OK;
		bridge->inactive_pending = true;
		forward_media_changed(context, event);
		return;
	}
	bridge->inactive_pending = bridge->receive_draining = false;
	bridge->last_binding =
		sip_rtp_apply(bridge->endpoint, event);
	if (bridge->last_binding == SIP_RTP_OK) {
		if (!bridge->media_configured) {
			bridge->trace_endpoint = bridge->endpoint->stats;
			bridge->trace_receiver = bridge->endpoint->stream->receiver.stats;
			bridge->trace_started = false;
		}
		bridge->media_configured =
			event->state == SIP_MEDIA_CONFIGURED;
	} else if (bridge->binding_failures != UINT64_MAX) {
		bridge->binding_failures++;
	}
	forward_media_changed(context, event);
}

bool sip_media_bridge_init(
	struct sip_media_bridge *bridge,
	struct rtp_endpoint *endpoint,
	const struct sip_signaling_callbacks *destination)
{
	struct sip_media_bridge initialized;

	if (bridge == NULL || endpoint == NULL || endpoint->stream == NULL)
		return false;
	memset(&initialized, 0, sizeof(initialized));
	initialized.endpoint = endpoint;
	if (destination != NULL)
		initialized.destination = *destination;
	initialized.source.context = bridge;
	initialized.source.ready = forward_ready;
	initialized.source.admit = forward_admit;
	initialized.source.line_owner = initialized.destination.line_owner;
	initialized.source.error = forward_error;
	initialized.source.trace = forward_trace;
	initialized.source.registration_changed = forward_registration;
	initialized.source.call_changed = forward_call;
	initialized.source.media_changed = forward_media;
	*bridge = initialized;
	return true;
}

const struct sip_signaling_callbacks *
sip_media_bridge_callbacks(
	const struct sip_media_bridge *bridge)
{
	return bridge != NULL ? &bridge->source : NULL;
}

void sip_media_bridge_begin_deferred(struct sip_media_bridge *bridge)
{
	if (bridge != NULL)
		bridge->deferred = true;
}

bool sip_media_bridge_finish_deferred(struct sip_media_bridge *bridge)
{
	if (bridge == NULL || !bridge->deferred)
		return false;
	bridge->deferred = false;
	bool valid = !bridge->pending_overflow;
	unsigned int count = bridge->pending_count;
	bridge->pending_count = 0;
	bridge->pending_overflow = false;
	for (unsigned int i = 0; i < count; ++i) {
		struct sip_media_pending *record = &bridge->pending[i];
		if (record->media)
			forward_media(bridge, &record->binding);
		else
			deliver_call(bridge, &record->call);
	}
	return valid;
}

bool sip_media_bridge_resolve_inactive(struct sip_media_bridge *bridge)
{
	if (bridge == NULL || !bridge->inactive_pending)
		return false;
	bridge->inactive_pending = false;
	bridge->callback_depth++;
	bool drain = bridge->destination.drain_received_media(bridge->destination.context);
	bridge->callback_depth--;
	if (drain)
		bridge->receive_draining = true;
	else
		sip_media_bridge_clear_media(bridge);
	return drain;
}

void sip_media_bridge_clear_media(
	struct sip_media_bridge *bridge)
{
	if (bridge == NULL)
		return;
	rtp_endpoint_clear_remote(bridge->endpoint);
	bridge->media_configured = false;
	bridge->inactive_pending = bridge->receive_draining = false;
}

void sip_media_bridge_trace_media(struct sip_media_bridge *bridge, uint64_t now_ns)
{
	struct rtp_endpoint *endpoint;
	const struct rtp_receiver_stats *rx;
	struct sockaddr_in remote;
	bool learned;
	char address[INET_ADDRSTRLEN];
	char text[1024];
	int length;
	struct sip_trace_record record = {
		.origin = SIP_TRACE_ADAPTER,
		.level = 3,
		.data = text,
	};

	if (bridge == NULL || bridge->destination.trace == NULL ||
	    !bridge->media_configured)
		return;
	if (bridge->trace_started && now_ns >= bridge->trace_time_ns &&
	    now_ns - bridge->trace_time_ns < UINT64_C(1000000000))
		return;
	endpoint = bridge->endpoint;
	rx = &endpoint->stream->receiver.stats;
	if (!rtp_endpoint_transmit_remote(endpoint, &remote, &learned) ||
	    inet_ntop(AF_INET, &remote.sin_addr, address, sizeof(address)) == NULL)
		return;
	/* Interval counts expose stalls even after earlier successful reception.
	 * No payload inspection, per-packet printing, or extra audio buffering. */
#define RX(field) (rx->field - bridge->trace_receiver.field)
#define IO(field) (endpoint->stats.field - bridge->trace_endpoint.field)
	length = snprintf(text, sizeof(text),
		"RTP %s pt=%u local=:%u remote=%s:%u (%s) "
		"TX=%" PRIu64 " RX=%" PRIu64 " accepted=%" PRIu64 " "
		"filtered=%" PRIu64 " malformed=%" PRIu64 " bad-pt=%" PRIu64 " "
		"oversized=%" PRIu64 " rx-errors=%" PRIu64 " "
		"tx-errors=%" PRIu64 " no-remote=%" PRIu64 " "
		"samples: buffered=%" PRIu64 " played=%" PRIu64 " "
		"concealed=%" PRIu64 " idle=%" PRIu64 " late=%" PRIu64 "\n",
		media_codec_name(endpoint->stream->codec),
		(unsigned)endpoint->stream->receiver.config.payload_type,
		(unsigned)rtp_endpoint_local_port(endpoint), address,
		(unsigned)ntohs(remote.sin_port), learned ? "learned" : "SDP",
		IO(transmit_sent), IO(receive_datagrams), RX(packets_accepted),
		IO(receive_filtered), RX(malformed_packets), RX(unexpected_payloads),
		IO(receive_oversized), IO(receive_errors),
		IO(transmit_errors), IO(transmit_no_remote),
		RX(samples_buffered), RX(samples_played), RX(samples_concealed),
		RX(samples_idle), RX(late_samples));
#undef IO
#undef RX
	bridge->trace_started = true;
	bridge->trace_time_ns = now_ns;
	bridge->trace_endpoint = endpoint->stats;
	bridge->trace_receiver = *rx;
	if (length <= 0 || (size_t)length >= sizeof(text))
		return;
	record.size = (size_t)length;
	forward_trace(bridge, &record);
}

enum sip_rtp_result sip_media_bridge_last_binding(
	const struct sip_media_bridge *bridge)
{
	return bridge != NULL ? bridge->last_binding :
			      SIP_RTP_INVALID_ARGUMENT;
}

uint64_t sip_media_bridge_binding_failures(
	const struct sip_media_bridge *bridge)
{
	return bridge != NULL ? bridge->binding_failures : 0;
}

bool sip_media_bridge_is_in_callback(
	const struct sip_media_bridge *bridge)
{
	return bridge != NULL && bridge->callback_depth != 0;
}

bool sip_media_bridge_is_configured(
	const struct sip_media_bridge *bridge)
{
	return bridge != NULL && bridge->media_configured;
}
