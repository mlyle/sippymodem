// SPDX-License-Identifier: BSD-3-Clause

#include "rtp_endpoint.h"

#include "media.h"
#include "rtp.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static bool address_is_valid(const struct sockaddr_in *address,
			     bool port_may_be_zero)
{
	return address != NULL && address->sin_family == AF_INET &&
	       (port_may_be_zero || address->sin_port != 0);
}

static bool addresses_are_equal(const struct sockaddr_in *left,
				const struct sockaddr_in *right)
{
	return left->sin_family == right->sin_family &&
	       left->sin_port == right->sin_port &&
	       left->sin_addr.s_addr == right->sin_addr.s_addr;
}

static struct sockaddr_in canonical_address(
	const struct sockaddr_in *address)
{
	struct sockaddr_in canonical = { 0 };

	canonical.sin_family = AF_INET;
	canonical.sin_port = address->sin_port;
	canonical.sin_addr = address->sin_addr;
	return canonical;
}

static int make_socket_nonblocking(int socket_fd)
{
	int flags = fcntl(socket_fd, F_GETFL, 0);
	int descriptor_flags;

	if (flags < 0 || fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) < 0)
		return -1;
	descriptor_flags = fcntl(socket_fd, F_GETFD, 0);
	if (descriptor_flags < 0 ||
	    fcntl(socket_fd, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0)
		return -1;
	return 0;
}

static size_t discard_queued_datagrams(struct rtp_endpoint *endpoint)
{
	uint8_t packet[RTP_ENDPOINT_MAX_DATAGRAM];

	if (endpoint->socket_fd < 0)
		return 0;
	size_t drained = 0;
	for (unsigned int attempt = 0; attempt < RTP_ENDPOINT_MAX_DRAIN; ++attempt) {
		ssize_t received = recv(endpoint->socket_fd, packet,
					sizeof(packet), MSG_DONTWAIT);

		if (received >= 0) {
			drained++;
			continue;
		}
		if (errno == EINTR)
			continue;
		endpoint->discard_pending = false;
		return drained;
	}
	endpoint->discard_pending = true;
	return drained;
}

bool rtp_endpoint_init(
	struct rtp_endpoint *endpoint,
	struct rtp_stream *stream,
	const struct rtp_endpoint_config *config)
{
	if (endpoint == NULL || stream == NULL || config == NULL ||
	    stream->receiver.slots == NULL)
		return false;
	memset(endpoint, 0, sizeof(*endpoint));
	endpoint->socket_fd = -1;
	endpoint->stream = stream;
	endpoint->config = *config;
	return true;
}

void rtp_endpoint_config_init(
	struct rtp_endpoint_config *config)
{
	if (config == NULL)
		return;
	memset(config, 0, sizeof(*config));
	config->learn_remote = true;
	config->symmetric_rtp = true;
}

enum rtp_endpoint_result rtp_endpoint_open(
	struct rtp_endpoint *endpoint,
	const struct sockaddr_in *local_address)
{
	struct sockaddr_in bound_address;
	socklen_t bound_size = sizeof(bound_address);
	int socket_fd;

	if (endpoint == NULL || endpoint->stream == NULL ||
	    !address_is_valid(local_address, true))
		return RTP_ENDPOINT_INVALID_ARGUMENT;
	if (endpoint->socket_fd >= 0)
		return RTP_ENDPOINT_ALREADY_OPEN;

	socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (socket_fd < 0)
		goto system_error;
	if (make_socket_nonblocking(socket_fd) != 0 ||
	    bind(socket_fd, (const struct sockaddr *)local_address,
		 sizeof(*local_address)) != 0 ||
	    getsockname(socket_fd, (struct sockaddr *)&bound_address,
			&bound_size) != 0) {
		int saved_error = errno;

		close(socket_fd);
		errno = saved_error;
		goto system_error;
	}

	endpoint->socket_fd = socket_fd;
	endpoint->local_address = canonical_address(&bound_address);
	endpoint->last_system_error = 0;
	return RTP_ENDPOINT_OK;

system_error:
	endpoint->last_system_error = errno;
	return RTP_ENDPOINT_SYSTEM_ERROR;
}

void rtp_endpoint_close(
	struct rtp_endpoint *endpoint)
{
	if (endpoint == NULL)
		return;
	if (endpoint->socket_fd >= 0)
		close(endpoint->socket_fd);
	endpoint->socket_fd = -1;
	endpoint->discard_pending = false;
	memset(&endpoint->local_address, 0, sizeof(endpoint->local_address));
	endpoint->remote_configured = false;
	endpoint->remote_learned = false;
	memset(&endpoint->remote_address, 0, sizeof(endpoint->remote_address));
	memset(&endpoint->learned_address, 0, sizeof(endpoint->learned_address));
	if (endpoint->stream != NULL)
		rtp_receiver_restart(&endpoint->stream->receiver);
}

int rtp_endpoint_fd(
	const struct rtp_endpoint *endpoint)
{
	return endpoint != NULL ? endpoint->socket_fd : -1;
}

uint16_t rtp_endpoint_local_port(
	const struct rtp_endpoint *endpoint)
{
	if (endpoint == NULL || endpoint->socket_fd < 0)
		return 0;
	return ntohs(endpoint->local_address.sin_port);
}

enum rtp_endpoint_result rtp_endpoint_set_remote(
	struct rtp_endpoint *endpoint,
	const struct sockaddr_in *remote_address)
{
	struct sockaddr_in canonical;

	if (endpoint == NULL || !address_is_valid(remote_address, false))
		return RTP_ENDPOINT_INVALID_ARGUMENT;
	if (endpoint->socket_fd < 0)
		return RTP_ENDPOINT_NOT_OPEN;
	canonical = canonical_address(remote_address);
	if (endpoint->remote_configured &&
	    addresses_are_equal(&endpoint->remote_address, &canonical))
		return RTP_ENDPOINT_OK;

	discard_queued_datagrams(endpoint);
	rtp_receiver_restart(&endpoint->stream->receiver);
	endpoint->remote_learned = false;
	memset(&endpoint->learned_address, 0, sizeof(endpoint->learned_address));
	endpoint->remote_address = canonical;
	endpoint->remote_configured = true;
	return RTP_ENDPOINT_OK;
}

void rtp_endpoint_clear_remote(
	struct rtp_endpoint *endpoint)
{
	if (endpoint == NULL)
		return;
	if (endpoint->socket_fd >= 0)
		discard_queued_datagrams(endpoint);
	endpoint->remote_configured = false;
	endpoint->remote_learned = false;
	memset(&endpoint->remote_address, 0, sizeof(endpoint->remote_address));
	memset(&endpoint->learned_address, 0, sizeof(endpoint->learned_address));
	if (endpoint->stream != NULL)
		rtp_receiver_restart(&endpoint->stream->receiver);
}

bool rtp_endpoint_transmit_remote(
	const struct rtp_endpoint *endpoint,
	struct sockaddr_in *remote_address,
	bool *learned)
{
	bool use_learned;

	if (endpoint == NULL || remote_address == NULL)
		return false;
	use_learned = endpoint->remote_learned &&
		      (endpoint->config.symmetric_rtp ||
		       !endpoint->remote_configured);
	if (!use_learned && !endpoint->remote_configured)
		return false;
	*remote_address = use_learned ? endpoint->learned_address :
				       endpoint->remote_address;
	if (learned != NULL)
		*learned = use_learned;
	return true;
}

static bool packet_can_establish_remote(
	const struct rtp_endpoint *endpoint,
	const uint8_t *packet,
	size_t packet_size)
{
	struct rtp_packet_view view;

	return !endpoint->stream->receiver.stream_active &&
	       rtp_packet_parse(packet, packet_size, &view) ==
		       RTP_OK &&
	       view.payload_size != 0 &&
	       view.header.payload_type ==
		       endpoint->stream->receiver.config.payload_type;
}

static bool source_is_allowed(struct rtp_endpoint *endpoint,
			      const struct sockaddr_in *source,
			      const uint8_t *packet, size_t packet_size)
{
	if ((endpoint->remote_configured &&
	     addresses_are_equal(source, &endpoint->remote_address)) ||
	    (endpoint->remote_learned &&
	     addresses_are_equal(source, &endpoint->learned_address)))
		return true;
	if (!endpoint->config.learn_remote || endpoint->remote_learned ||
	    !packet_can_establish_remote(endpoint, packet, packet_size))
		return false;

	endpoint->learned_address = canonical_address(source);
	endpoint->remote_learned = true;
	endpoint->stats.remote_learns++;
	return true;
}

static void record_packet(
	struct rtp_endpoint *endpoint, const uint8_t *packet, size_t packet_size,
	enum recording_packet_event event)
{
	const struct media_recorder *recorder = &endpoint->stream->recorder;
	const struct rtp_receiver_stats *stats = &endpoint->stream->receiver.stats;
	struct rtp_packet_view view;
	struct recording_packet observation = { .event = event };
	struct timespec now;
	int saved_error = errno;

	if (recorder->packet == NULL)
		return;
	/* Take the timestamp before any potentially blocking evidence write. */
	if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
		observation.monotonic_ns = (uint64_t)now.tv_sec * UINT64_C(1000000000) +
			(uint64_t)now.tv_nsec;
	if (rtp_packet_parse(packet, packet_size, &view) == RTP_OK) {
		observation.sequence = view.header.sequence;
		observation.timestamp = view.header.timestamp;
		observation.ssrc = view.header.ssrc;
		observation.payload_type = view.header.payload_type;
		observation.marker = view.header.marker;
		observation.payload_octets = (uint32_t)view.payload_size;
		observation.playout_samples = stats->samples_played + stats->samples_concealed +
			stats->samples_prefilled + stats->samples_idle;
		observation.concealed_samples = stats->samples_concealed;
		recorder->packet(recorder->context, &observation);
	}
	errno = saved_error;
}

static void record_arriving_payload(
	struct rtp_endpoint *endpoint,
	const uint8_t *packet,
	size_t packet_size)
{
	struct rtp_packet_view view;

	if (rtp_packet_parse(packet, packet_size, &view) !=
	    RTP_OK)
		return;
	recording_emit(
		&endpoint->stream->recorder,
		RECORD_RX_RTP_PAYLOAD_ARRIVAL,
		RECORD_OCTETS, MEDIA_SAMPLE_RATE,
		view.payload, view.payload_size);
}

static void route_datagram(struct rtp_endpoint *endpoint,
			   const uint8_t *packet, size_t packet_size)
{
	enum rtp_result packet_result;
	enum rtp_receive_result receive_result =
		rtp_stream_receive(endpoint->stream, packet,
					       packet_size, &packet_result);

	endpoint->stats.receive_routed++;
	record_packet(endpoint, packet, packet_size,
		receive_result == RTP_RECEIVE_ACCEPTED ? RECORD_PACKET_ACCEPTED :
		receive_result == RTP_RECEIVE_LATE ? RECORD_PACKET_LATE :
		receive_result == RTP_RECEIVE_TOO_EARLY ? RECORD_PACKET_EARLY :
		RECORD_PACKET_REJECTED);
	if (receive_result == RTP_RECEIVE_ACCEPTED ||
	    receive_result == RTP_RECEIVE_LATE ||
	    receive_result == RTP_RECEIVE_TOO_EARLY)
		record_arriving_payload(endpoint, packet, packet_size);
}

enum rtp_endpoint_result rtp_endpoint_drain(
	struct rtp_endpoint *endpoint,
	size_t *datagrams_received)
{
	uint8_t packet[RTP_ENDPOINT_MAX_DATAGRAM];
	size_t drained = 0;

	if (endpoint == NULL || endpoint->stream == NULL)
		return RTP_ENDPOINT_INVALID_ARGUMENT;
	if (endpoint->socket_fd < 0)
		return RTP_ENDPOINT_NOT_OPEN;
	if (endpoint->discard_pending) {
		size_t discarded = discard_queued_datagrams(endpoint);
		if (datagrams_received != NULL)
			*datagrams_received = discarded;
		return RTP_ENDPOINT_OK;
	}

	for (unsigned int attempt = 0; attempt < RTP_ENDPOINT_MAX_DRAIN; ++attempt) {
		struct sockaddr_in source = { 0 };
		struct iovec vector = {
			.iov_base = packet,
			.iov_len = sizeof(packet),
		};
		struct msghdr message = {
			.msg_name = &source,
			.msg_namelen = sizeof(source),
			.msg_iov = &vector,
			.msg_iovlen = 1,
		};
		ssize_t received = recvmsg(endpoint->socket_fd, &message,
					   MSG_DONTWAIT | MSG_TRUNC);

		if (received < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			endpoint->last_system_error = errno;
			endpoint->stats.receive_errors++;
			return RTP_ENDPOINT_SYSTEM_ERROR;
		}

		drained++;
		endpoint->stats.receive_datagrams++;
		if ((message.msg_flags & MSG_TRUNC) != 0 ||
		    (size_t)received > sizeof(packet)) {
			endpoint->stats.receive_oversized++;
			continue;
		}
		if (!source_is_allowed(endpoint, &source, packet,
				       (size_t)received)) {
			endpoint->stats.receive_filtered++;
			continue;
		}
		route_datagram(endpoint, packet, (size_t)received);
	}

	if (datagrams_received != NULL)
		*datagrams_received = drained;
	return RTP_ENDPOINT_OK;
}

static enum rtp_endpoint_result send_frame(
	struct rtp_endpoint *endpoint, const int16_t *samples,
	const uint8_t *codewords, size_t count,
	uint8_t *packet_workspace, size_t packet_capacity)
{
	struct sockaddr_in remote_address;
	size_t packet_size = 0;
	ssize_t sent;

	if (endpoint == NULL || endpoint->stream == NULL || samples == NULL ||
	    count == 0 || packet_workspace == NULL)
		return RTP_ENDPOINT_INVALID_ARGUMENT;
	if (endpoint->socket_fd < 0)
		return RTP_ENDPOINT_NOT_OPEN;
	if (!rtp_endpoint_transmit_remote(
		    endpoint, &remote_address, NULL)) {
		endpoint->stats.transmit_no_remote++;
		return RTP_ENDPOINT_NO_REMOTE;
	}

	endpoint->last_media_error = codewords != NULL ?
		rtp_stream_write_g711(
			endpoint->stream, codewords, samples, count,
			packet_workspace, packet_capacity, &packet_size) :
		rtp_stream_write(
			endpoint->stream, samples, count, packet_workspace,
			packet_capacity, &packet_size);
	if (endpoint->last_media_error != MEDIA_OK)
		return RTP_ENDPOINT_MEDIA_ERROR;
	endpoint->stats.transmit_attempts++;

	for (unsigned int attempt = 0; attempt < 4; ++attempt) {
		sent = sendto(endpoint->socket_fd, packet_workspace, packet_size, 0,
			      (const struct sockaddr *)&remote_address,
			      sizeof(remote_address));
		if (sent >= 0 || errno != EINTR)
			break;
	}
	record_packet(endpoint, packet_workspace, packet_size,
		sent == (ssize_t)packet_size ? RECORD_PACKET_SENT :
		RECORD_PACKET_SEND_FAILED);
	if (sent != (ssize_t)packet_size) {
		endpoint->last_system_error = sent < 0 ? errno : EIO;
		endpoint->stats.transmit_errors++;
		return RTP_ENDPOINT_SYSTEM_ERROR;
	}

	endpoint->stats.transmit_sent++;
	recording_emit(
		&endpoint->stream->recorder,
		RECORD_TX_RTP_PAYLOAD_SENT,
		RECORD_OCTETS, MEDIA_SAMPLE_RATE,
		packet_workspace + RTP_HEADER_SIZE, count);
	return RTP_ENDPOINT_OK;
}

enum rtp_endpoint_result rtp_endpoint_send(
	struct rtp_endpoint *endpoint,
	const int16_t *samples,
	size_t count,
	uint8_t *packet_workspace,
	size_t packet_capacity)
{
	return send_frame(
		endpoint, samples, NULL, count,
		packet_workspace, packet_capacity);
}

enum rtp_endpoint_result rtp_endpoint_send_g711(
	struct rtp_endpoint *endpoint,
	const uint8_t *codewords,
	const int16_t *decoded_samples,
	size_t count,
	uint8_t *packet_workspace,
	size_t packet_capacity)
{
	if (codewords == NULL)
		return RTP_ENDPOINT_INVALID_ARGUMENT;
	return send_frame(
		endpoint, decoded_samples, codewords, count,
		packet_workspace, packet_capacity);
}

const char *rtp_endpoint_result_name(
	enum rtp_endpoint_result result)
{
	switch (result) {
	case RTP_ENDPOINT_OK:
		return "ok";
	case RTP_ENDPOINT_INVALID_ARGUMENT:
		return "invalid argument";
	case RTP_ENDPOINT_NOT_OPEN:
		return "RTP endpoint is not open";
	case RTP_ENDPOINT_ALREADY_OPEN:
		return "RTP endpoint is already open";
	case RTP_ENDPOINT_NO_REMOTE:
		return "RTP remote is not configured";
	case RTP_ENDPOINT_MEDIA_ERROR:
		return "RTP media conversion failed";
	case RTP_ENDPOINT_SYSTEM_ERROR:
		return "RTP socket operation failed";
	}
	return "unknown RTP endpoint result";
}
