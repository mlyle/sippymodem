// SPDX-License-Identifier: BSD-3-Clause
#ifndef RTP_TRANSMIT_H
#define RTP_TRANSMIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { RTP_TRANSMIT_CAPACITY = 64 };
enum rtp_transmit_result {
	RTP_TRANSMIT_OK,
	RTP_TRANSMIT_FULL,
	RTP_TRANSMIT_INVALID,
};
struct rtp_transmit;
struct rtp_transmit_status {
	uint64_t accepted, sent, canceled, clock_epochs, interval_ns;
	size_t queued;
	bool credit_limited;
};

/* One native media owner, no internal thread. Storage is bounded at creation.
 * Frames span 1 ms to 1 s, with an integer nanosecond quarter-period.
 * Reserve is at most half the queue's nominal duration. */
struct rtp_transmit *rtp_transmit_create(uint64_t frame_ns,
	uint64_t reserve_ns, size_t packet_bytes);
void rtp_transmit_destroy(struct rtp_transmit *transmit);
/* Copies the entire datagram. Rejection changes no bytes, clock or counters. */
enum rtp_transmit_result rtp_transmit_queue(struct rtp_transmit *transmit,
	const void *packet, size_t size, uint64_t now_ns);
/* Borrowed head until sent/clear/destroy. Observation spends no pacing credit.
 * Commit only after successful socket send; a failed send retains the head. */
const uint8_t *rtp_transmit_peek(const struct rtp_transmit *transmit,
	uint64_t now_ns, size_t *size);
bool rtp_transmit_sent(struct rtp_transmit *transmit, uint64_t now_ns);
uint64_t rtp_transmit_wait_ns(const struct rtp_transmit *transmit, uint64_t now_ns);
/* Explicit cancellation; sent+canceled is the retired admission watermark.
 * Retirement of canceled packets is not successful transmission. */
size_t rtp_transmit_clear(struct rtp_transmit *transmit);
struct rtp_transmit_status rtp_transmit_get_status(const struct rtp_transmit *transmit);

#endif
