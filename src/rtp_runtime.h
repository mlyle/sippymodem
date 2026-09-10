// SPDX-License-Identifier: BSD-3-Clause

#ifndef RTP_RUNTIME_H
#define RTP_RUNTIME_H

#include "config.h"
#include "rtp_endpoint.h"

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

struct rtp_runtime;

/* The runtime copies recorder and configuration values; callback context and
 * recorded blocks retain the borrowing rules from media_recorder. */
struct rtp_runtime *rtp_runtime_create(
	const struct media_config *config,
	const struct media_recorder *recorder);
void rtp_runtime_destroy(
	struct rtp_runtime *runtime);

enum rtp_endpoint_result rtp_runtime_open(
	struct rtp_runtime *runtime,
	const struct sockaddr_in *local_address);
void rtp_runtime_close(
	struct rtp_runtime *runtime);

size_t rtp_runtime_frame_samples(
	const struct rtp_runtime *runtime);
int rtp_runtime_fd(
	const struct rtp_runtime *runtime);
uint16_t rtp_runtime_local_port(
	const struct rtp_runtime *runtime);

enum rtp_endpoint_result rtp_runtime_drain(
	struct rtp_runtime *runtime,
	size_t *datagrams_received);
/* Complete frames of remote RTP time observed by the most recent drain. */
size_t rtp_runtime_remote_clock_frames(
	const struct rtp_runtime *runtime);
enum media_result rtp_runtime_play(
	struct rtp_runtime *runtime,
	int16_t *samples,
	uint8_t *source_octets, bool *received_mask,
	size_t count,
	struct rtp_playout_report *report);
enum rtp_endpoint_result rtp_runtime_send(
	struct rtp_runtime *runtime,
	const int16_t *samples,
	size_t count);
enum rtp_endpoint_result rtp_runtime_send_g711(
	struct rtp_runtime *runtime,
	const uint8_t *codewords,
	const int16_t *decoded_samples,
	size_t count);

/* Borrowed mutable endpoint for narrow protocol-composition layers. */
struct rtp_endpoint *rtp_runtime_endpoint(
	struct rtp_runtime *runtime);

#endif
