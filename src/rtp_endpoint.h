// SPDX-License-Identifier: BSD-3-Clause

#ifndef RTP_ENDPOINT_H
#define RTP_ENDPOINT_H

#include "rtp_stream.h"

#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	RTP_ENDPOINT_MAX_DATAGRAM = 2048,
};

enum rtp_endpoint_result {
	RTP_ENDPOINT_OK,
	RTP_ENDPOINT_INVALID_ARGUMENT,
	RTP_ENDPOINT_NOT_OPEN,
	RTP_ENDPOINT_ALREADY_OPEN,
	RTP_ENDPOINT_NO_REMOTE,
	RTP_ENDPOINT_MEDIA_ERROR,
	RTP_ENDPOINT_SYSTEM_ERROR,
};

struct rtp_endpoint_config {
	bool learn_remote;
	bool symmetric_rtp;
};

struct rtp_endpoint_stats {
	uint64_t receive_datagrams;
	uint64_t receive_routed;
	uint64_t receive_filtered;
	uint64_t receive_oversized;
	uint64_t receive_errors;
	uint64_t remote_learns;
	uint64_t transmit_attempts;
	uint64_t transmit_sent;
	uint64_t transmit_no_remote;
	uint64_t transmit_errors;
};

struct rtp_endpoint {
	int socket_fd;
	int last_system_error;
	enum media_result last_media_error;
	struct rtp_stream *stream;
	struct rtp_endpoint_config config;
	struct sockaddr_in local_address;
	struct sockaddr_in remote_address;
	struct sockaddr_in learned_address;
	bool remote_configured;
	bool remote_learned;
	struct rtp_endpoint_stats stats;
};

void rtp_endpoint_config_init(
	struct rtp_endpoint_config *config);
bool rtp_endpoint_init(
	struct rtp_endpoint *endpoint,
	struct rtp_stream *stream,
	const struct rtp_endpoint_config *config);
enum rtp_endpoint_result rtp_endpoint_open(
	struct rtp_endpoint *endpoint,
	const struct sockaddr_in *local_address);
void rtp_endpoint_close(
	struct rtp_endpoint *endpoint);

int rtp_endpoint_fd(
	const struct rtp_endpoint *endpoint);
uint16_t rtp_endpoint_local_port(
	const struct rtp_endpoint *endpoint);

enum rtp_endpoint_result rtp_endpoint_set_remote(
	struct rtp_endpoint *endpoint,
	const struct sockaddr_in *remote_address);
void rtp_endpoint_clear_remote(
	struct rtp_endpoint *endpoint);
bool rtp_endpoint_transmit_remote(
	const struct rtp_endpoint *endpoint,
	struct sockaddr_in *remote_address,
	bool *learned);

/* Drain every currently queued datagram without blocking. */
enum rtp_endpoint_result rtp_endpoint_drain(
	struct rtp_endpoint *endpoint,
	size_t *datagrams_received);

/* packet_workspace is caller-owned scratch storage for one outgoing packet. */
enum rtp_endpoint_result rtp_endpoint_send(
	struct rtp_endpoint *endpoint,
	const int16_t *samples,
	size_t count,
	uint8_t *packet_workspace,
	size_t packet_capacity);

const char *rtp_endpoint_result_name(
	enum rtp_endpoint_result result);

#endif
