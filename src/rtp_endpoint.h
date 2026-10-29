// SPDX-License-Identifier: BSD-3-Clause

#ifndef RTP_ENDPOINT_H
#define RTP_ENDPOINT_H

#include "rtp_stream.h"
#include "rtp_transmit.h"

#include "net_socket.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	RTP_ENDPOINT_MAX_DATAGRAM = 2048,
	RTP_ENDPOINT_MAX_DRAIN = 64,
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

/* One host pass emits at most four frames. Owned by the RTP runtime and
 * borrowed exclusively by its endpoint between owner-side begin/finish. */
struct rtp_transmit_batch {
	unsigned int count;
	struct { size_t size; uint8_t data[RTP_ENDPOINT_MAX_DATAGRAM]; }
		packets[MEDIA_MAX_CATCHUP_FRAMES];
};

struct rtp_transmit_observation {
	uint64_t playout_samples, concealed_samples;
};

struct rtp_endpoint {
	net_socket socket_fd;
#ifdef _WIN32
	WSAEVENT read_event; /* Owned, level observation only; drain acknowledges it. */
#endif
	int last_system_error;
	enum media_result last_media_error;
	struct rtp_stream *stream;
	struct rtp_endpoint_config config;
	struct sockaddr_in local_address;
	struct sockaddr_in remote_address;
	struct sockaddr_in learned_address;
	bool remote_configured;
	bool remote_learned;
	bool discard_pending; /* Bounded cleanup of the previous media incarnation. */
	struct rtp_endpoint_stats stats;
	struct rtp_transmit_batch *transmit_batch;
	/* Optional borrowed queue. Only the native owner accesses it;
	 * DSP jobs keep using the existing copied batch. */
	struct rtp_transmit *transmit_queue;
	uint64_t transmit_now_ns;
	struct rtp_transmit_observation transmit_observations[RTP_TRANSMIT_CAPACITY];
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

net_socket rtp_endpoint_fd(
	const struct rtp_endpoint *endpoint);
/* Borrowed Windows FD_READ event; observe only, drain acknowledges it.
 * NULL on POSIX or while closed. */
void *rtp_endpoint_read_handle(const struct rtp_endpoint *endpoint);
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

/* Drain at most MAX_DRAIN datagrams without blocking. An unfinished call
 * boundary flush remains pending; those old bytes are never routed as media. */
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
enum rtp_endpoint_result rtp_endpoint_send_g711(
	struct rtp_endpoint *endpoint,
	const uint8_t *codewords,
	const int16_t *decoded_samples,
	size_t count,
	uint8_t *packet_workspace,
	size_t packet_capacity);

/* Native owner only, including while a DSP batch is in flight. Actual-send
 * records use the queued packet's receive-cursor snapshot. */
enum rtp_endpoint_result rtp_endpoint_process_transmit(struct rtp_endpoint *endpoint, uint64_t now_ns);
uint64_t rtp_endpoint_transmit_wait_ns(const struct rtp_endpoint *endpoint, uint64_t now_ns);

const char *rtp_endpoint_result_name(
	enum rtp_endpoint_result result);

/* send/send_g711 copy packets without socket I/O; finish hands the batch
 * to the owner transport for immediate sending or bounded queue admission.
 * A full batch is an explicit error. No buffer pointer survives finish. */
enum rtp_endpoint_result rtp_endpoint_begin_transmit_batch(
	struct rtp_endpoint *endpoint, struct rtp_transmit_batch *batch);
enum rtp_endpoint_result rtp_endpoint_finish_transmit_batch(struct rtp_endpoint *endpoint);

#endif
