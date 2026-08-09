// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_RTP_ENDPOINT_H
#define SIPPYMODEM_RTP_ENDPOINT_H

#include "sippymodem_rtp_stream.h"

#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	SIPPYMODEM_RTP_ENDPOINT_MAX_DATAGRAM = 2048,
};

enum sippymodem_rtp_endpoint_result {
	SIPPYMODEM_RTP_ENDPOINT_OK,
	SIPPYMODEM_RTP_ENDPOINT_INVALID_ARGUMENT,
	SIPPYMODEM_RTP_ENDPOINT_NOT_OPEN,
	SIPPYMODEM_RTP_ENDPOINT_ALREADY_OPEN,
	SIPPYMODEM_RTP_ENDPOINT_NO_REMOTE,
	SIPPYMODEM_RTP_ENDPOINT_MEDIA_ERROR,
	SIPPYMODEM_RTP_ENDPOINT_SYSTEM_ERROR,
};

struct sippymodem_rtp_endpoint_stats {
	uint64_t receive_datagrams;
	uint64_t receive_routed;
	uint64_t receive_filtered;
	uint64_t receive_oversized;
	uint64_t receive_errors;
	uint64_t transmit_attempts;
	uint64_t transmit_sent;
	uint64_t transmit_no_remote;
	uint64_t transmit_errors;
};

struct sippymodem_rtp_endpoint {
	int socket_fd;
	int last_system_error;
	enum sippymodem_media_result last_media_error;
	struct sippymodem_rtp_stream *stream;
	struct sockaddr_in local_address;
	struct sockaddr_in remote_address;
	bool remote_configured;
	struct sippymodem_rtp_endpoint_stats stats;
};

bool sippymodem_rtp_endpoint_init(
	struct sippymodem_rtp_endpoint *endpoint,
	struct sippymodem_rtp_stream *stream);
enum sippymodem_rtp_endpoint_result sippymodem_rtp_endpoint_open(
	struct sippymodem_rtp_endpoint *endpoint,
	const struct sockaddr_in *local_address);
void sippymodem_rtp_endpoint_close(
	struct sippymodem_rtp_endpoint *endpoint);

int sippymodem_rtp_endpoint_fd(
	const struct sippymodem_rtp_endpoint *endpoint);
uint16_t sippymodem_rtp_endpoint_local_port(
	const struct sippymodem_rtp_endpoint *endpoint);

enum sippymodem_rtp_endpoint_result sippymodem_rtp_endpoint_set_remote(
	struct sippymodem_rtp_endpoint *endpoint,
	const struct sockaddr_in *remote_address);
void sippymodem_rtp_endpoint_clear_remote(
	struct sippymodem_rtp_endpoint *endpoint);

/* Drain every currently queued datagram without blocking. */
enum sippymodem_rtp_endpoint_result sippymodem_rtp_endpoint_drain(
	struct sippymodem_rtp_endpoint *endpoint,
	size_t *datagrams_received);

/* packet_workspace is caller-owned scratch storage for one outgoing packet. */
enum sippymodem_rtp_endpoint_result sippymodem_rtp_endpoint_send(
	struct sippymodem_rtp_endpoint *endpoint,
	const int16_t *samples,
	size_t count,
	uint8_t *packet_workspace,
	size_t packet_capacity);

const char *sippymodem_rtp_endpoint_result_name(
	enum sippymodem_rtp_endpoint_result result);

#endif
