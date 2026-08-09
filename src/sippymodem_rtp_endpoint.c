// SPDX-License-Identifier: BSD-3-Clause

#include "sippymodem_rtp_endpoint.h"

#include "sippymodem_media.h"
#include "sippymodem_rtp.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
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

static void discard_queued_datagrams(struct sippymodem_rtp_endpoint *endpoint)
{
	uint8_t packet[SIPPYMODEM_RTP_ENDPOINT_MAX_DATAGRAM];

	if (endpoint->socket_fd < 0)
		return;
	for (;;) {
		ssize_t received = recv(endpoint->socket_fd, packet,
					sizeof(packet), MSG_DONTWAIT);

		if (received >= 0)
			continue;
		if (errno == EINTR)
			continue;
		return;
	}
}

bool sippymodem_rtp_endpoint_init(
	struct sippymodem_rtp_endpoint *endpoint,
	struct sippymodem_rtp_stream *stream)
{
	if (endpoint == NULL || stream == NULL ||
	    stream->receiver.slots == NULL)
		return false;
	memset(endpoint, 0, sizeof(*endpoint));
	endpoint->socket_fd = -1;
	endpoint->stream = stream;
	return true;
}

enum sippymodem_rtp_endpoint_result sippymodem_rtp_endpoint_open(
	struct sippymodem_rtp_endpoint *endpoint,
	const struct sockaddr_in *local_address)
{
	struct sockaddr_in bound_address;
	socklen_t bound_size = sizeof(bound_address);
	int socket_fd;

	if (endpoint == NULL || endpoint->stream == NULL ||
	    !address_is_valid(local_address, true))
		return SIPPYMODEM_RTP_ENDPOINT_INVALID_ARGUMENT;
	if (endpoint->socket_fd >= 0)
		return SIPPYMODEM_RTP_ENDPOINT_ALREADY_OPEN;

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
	return SIPPYMODEM_RTP_ENDPOINT_OK;

system_error:
	endpoint->last_system_error = errno;
	return SIPPYMODEM_RTP_ENDPOINT_SYSTEM_ERROR;
}

void sippymodem_rtp_endpoint_close(
	struct sippymodem_rtp_endpoint *endpoint)
{
	if (endpoint == NULL)
		return;
	if (endpoint->socket_fd >= 0)
		close(endpoint->socket_fd);
	endpoint->socket_fd = -1;
	memset(&endpoint->local_address, 0, sizeof(endpoint->local_address));
	endpoint->remote_configured = false;
	memset(&endpoint->remote_address, 0, sizeof(endpoint->remote_address));
	if (endpoint->stream != NULL)
		sippymodem_rtp_receiver_restart(&endpoint->stream->receiver);
}

int sippymodem_rtp_endpoint_fd(
	const struct sippymodem_rtp_endpoint *endpoint)
{
	return endpoint != NULL ? endpoint->socket_fd : -1;
}

uint16_t sippymodem_rtp_endpoint_local_port(
	const struct sippymodem_rtp_endpoint *endpoint)
{
	if (endpoint == NULL || endpoint->socket_fd < 0)
		return 0;
	return ntohs(endpoint->local_address.sin_port);
}

enum sippymodem_rtp_endpoint_result sippymodem_rtp_endpoint_set_remote(
	struct sippymodem_rtp_endpoint *endpoint,
	const struct sockaddr_in *remote_address)
{
	struct sockaddr_in canonical;

	if (endpoint == NULL || !address_is_valid(remote_address, false))
		return SIPPYMODEM_RTP_ENDPOINT_INVALID_ARGUMENT;
	if (endpoint->socket_fd < 0)
		return SIPPYMODEM_RTP_ENDPOINT_NOT_OPEN;
	canonical = canonical_address(remote_address);
	if (endpoint->remote_configured &&
	    addresses_are_equal(&endpoint->remote_address, &canonical))
		return SIPPYMODEM_RTP_ENDPOINT_OK;

	discard_queued_datagrams(endpoint);
	sippymodem_rtp_receiver_restart(&endpoint->stream->receiver);
	endpoint->remote_address = canonical;
	endpoint->remote_configured = true;
	return SIPPYMODEM_RTP_ENDPOINT_OK;
}

void sippymodem_rtp_endpoint_clear_remote(
	struct sippymodem_rtp_endpoint *endpoint)
{
	if (endpoint == NULL)
		return;
	if (endpoint->socket_fd >= 0)
		discard_queued_datagrams(endpoint);
	endpoint->remote_configured = false;
	memset(&endpoint->remote_address, 0, sizeof(endpoint->remote_address));
	if (endpoint->stream != NULL)
		sippymodem_rtp_receiver_restart(&endpoint->stream->receiver);
}

static void record_arriving_payload(
	struct sippymodem_rtp_endpoint *endpoint,
	const uint8_t *packet,
	size_t packet_size)
{
	struct sippymodem_rtp_packet_view view;

	if (sippymodem_rtp_packet_parse(packet, packet_size, &view) !=
	    SIPPYMODEM_RTP_OK)
		return;
	sippymodem_recording_emit(
		&endpoint->stream->recorder,
		SIPPYMODEM_RECORD_RX_RTP_PAYLOAD_ARRIVAL,
		SIPPYMODEM_RECORD_OCTETS, SIPPYMODEM_MEDIA_SAMPLE_RATE,
		view.payload, view.payload_size);
}

static void route_datagram(struct sippymodem_rtp_endpoint *endpoint,
			   const uint8_t *packet, size_t packet_size)
{
	enum sippymodem_rtp_result packet_result;
	enum sippymodem_rtp_receive_result receive_result =
		sippymodem_rtp_stream_receive(endpoint->stream, packet,
					       packet_size, &packet_result);

	endpoint->stats.receive_routed++;
	if (receive_result == SIPPYMODEM_RTP_RECEIVE_ACCEPTED ||
	    receive_result == SIPPYMODEM_RTP_RECEIVE_LATE ||
	    receive_result == SIPPYMODEM_RTP_RECEIVE_TOO_EARLY)
		record_arriving_payload(endpoint, packet, packet_size);
}

enum sippymodem_rtp_endpoint_result sippymodem_rtp_endpoint_drain(
	struct sippymodem_rtp_endpoint *endpoint,
	size_t *datagrams_received)
{
	uint8_t packet[SIPPYMODEM_RTP_ENDPOINT_MAX_DATAGRAM];
	size_t drained = 0;

	if (endpoint == NULL || endpoint->stream == NULL)
		return SIPPYMODEM_RTP_ENDPOINT_INVALID_ARGUMENT;
	if (endpoint->socket_fd < 0)
		return SIPPYMODEM_RTP_ENDPOINT_NOT_OPEN;

	for (;;) {
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
			return SIPPYMODEM_RTP_ENDPOINT_SYSTEM_ERROR;
		}

		drained++;
		endpoint->stats.receive_datagrams++;
		if ((message.msg_flags & MSG_TRUNC) != 0 ||
		    (size_t)received > sizeof(packet)) {
			endpoint->stats.receive_oversized++;
			continue;
		}
		if (!endpoint->remote_configured ||
		    !addresses_are_equal(&source, &endpoint->remote_address)) {
			endpoint->stats.receive_filtered++;
			continue;
		}
		route_datagram(endpoint, packet, (size_t)received);
	}

	if (datagrams_received != NULL)
		*datagrams_received = drained;
	return SIPPYMODEM_RTP_ENDPOINT_OK;
}

enum sippymodem_rtp_endpoint_result sippymodem_rtp_endpoint_send(
	struct sippymodem_rtp_endpoint *endpoint,
	const int16_t *samples,
	size_t count,
	uint8_t *packet_workspace,
	size_t packet_capacity)
{
	size_t packet_size = 0;
	ssize_t sent;

	if (endpoint == NULL || endpoint->stream == NULL || samples == NULL ||
	    count == 0 || packet_workspace == NULL)
		return SIPPYMODEM_RTP_ENDPOINT_INVALID_ARGUMENT;
	if (endpoint->socket_fd < 0)
		return SIPPYMODEM_RTP_ENDPOINT_NOT_OPEN;
	if (!endpoint->remote_configured) {
		endpoint->stats.transmit_no_remote++;
		return SIPPYMODEM_RTP_ENDPOINT_NO_REMOTE;
	}

	endpoint->last_media_error = sippymodem_rtp_stream_write(
		endpoint->stream, samples, count, packet_workspace,
		packet_capacity, &packet_size);
	if (endpoint->last_media_error != SIPPYMODEM_MEDIA_OK)
		return SIPPYMODEM_RTP_ENDPOINT_MEDIA_ERROR;
	endpoint->stats.transmit_attempts++;

	do {
		sent = sendto(endpoint->socket_fd, packet_workspace, packet_size, 0,
			      (const struct sockaddr *)&endpoint->remote_address,
			      sizeof(endpoint->remote_address));
	} while (sent < 0 && errno == EINTR);
	if (sent != (ssize_t)packet_size) {
		endpoint->last_system_error = sent < 0 ? errno : EIO;
		endpoint->stats.transmit_errors++;
		return SIPPYMODEM_RTP_ENDPOINT_SYSTEM_ERROR;
	}

	endpoint->stats.transmit_sent++;
	sippymodem_recording_emit(
		&endpoint->stream->recorder,
		SIPPYMODEM_RECORD_TX_RTP_PAYLOAD_SENT,
		SIPPYMODEM_RECORD_OCTETS, SIPPYMODEM_MEDIA_SAMPLE_RATE,
		packet_workspace + SIPPYMODEM_RTP_HEADER_SIZE, count);
	return SIPPYMODEM_RTP_ENDPOINT_OK;
}

const char *sippymodem_rtp_endpoint_result_name(
	enum sippymodem_rtp_endpoint_result result)
{
	switch (result) {
	case SIPPYMODEM_RTP_ENDPOINT_OK:
		return "ok";
	case SIPPYMODEM_RTP_ENDPOINT_INVALID_ARGUMENT:
		return "invalid argument";
	case SIPPYMODEM_RTP_ENDPOINT_NOT_OPEN:
		return "RTP endpoint is not open";
	case SIPPYMODEM_RTP_ENDPOINT_ALREADY_OPEN:
		return "RTP endpoint is already open";
	case SIPPYMODEM_RTP_ENDPOINT_NO_REMOTE:
		return "RTP remote is not configured";
	case SIPPYMODEM_RTP_ENDPOINT_MEDIA_ERROR:
		return "RTP media conversion failed";
	case SIPPYMODEM_RTP_ENDPOINT_SYSTEM_ERROR:
		return "RTP socket operation failed";
	}
	return "unknown RTP endpoint result";
}
