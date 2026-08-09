// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_RTP_RUNTIME_H
#define SIPPYMODEM_RTP_RUNTIME_H

#include "sippymodem_config.h"
#include "sippymodem_rtp_endpoint.h"

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

struct sippymodem_rtp_runtime;

/* The runtime copies recorder and configuration values; callback context and
 * recorded blocks retain the borrowing rules from sippymodem_recorder. */
struct sippymodem_rtp_runtime *sippymodem_rtp_runtime_create(
	const struct sippymodem_media_config *config,
	const struct sippymodem_recorder *recorder);
void sippymodem_rtp_runtime_destroy(
	struct sippymodem_rtp_runtime *runtime);

enum sippymodem_rtp_endpoint_result sippymodem_rtp_runtime_open(
	struct sippymodem_rtp_runtime *runtime,
	const struct sockaddr_in *local_address);
void sippymodem_rtp_runtime_close(
	struct sippymodem_rtp_runtime *runtime);

size_t sippymodem_rtp_runtime_frame_samples(
	const struct sippymodem_rtp_runtime *runtime);
uint16_t sippymodem_rtp_runtime_local_port(
	const struct sippymodem_rtp_runtime *runtime);

enum sippymodem_rtp_endpoint_result sippymodem_rtp_runtime_drain(
	struct sippymodem_rtp_runtime *runtime,
	size_t *datagrams_received);
enum sippymodem_media_result sippymodem_rtp_runtime_play(
	struct sippymodem_rtp_runtime *runtime,
	int16_t *samples,
	size_t count,
	struct sippymodem_rtp_playout_report *report);
enum sippymodem_rtp_endpoint_result sippymodem_rtp_runtime_send(
	struct sippymodem_rtp_runtime *runtime,
	const int16_t *samples,
	size_t count);

/* Borrowed mutable endpoint for narrow protocol-composition layers. */
struct sippymodem_rtp_endpoint *sippymodem_rtp_runtime_endpoint(
	struct sippymodem_rtp_runtime *runtime);

#endif
