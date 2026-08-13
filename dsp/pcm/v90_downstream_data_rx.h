// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_DOWNSTREAM_DATA_RX_H
#define V90_DOWNSTREAM_DATA_RX_H

#include "v90_pcm_stream_rx.h"
#include "v90_phase4.h"

#include <stdbool.h>
#include <stdint.h>

enum v90_downstream_data_rx_stage {
	V90_DOWNSTREAM_DATA_RX_STAGE_B1D,
	V90_DOWNSTREAM_DATA_RX_STAGE_DATA,
};

enum v90_downstream_data_rx_result {
	V90_DOWNSTREAM_DATA_RX_OK,
	V90_DOWNSTREAM_DATA_RX_BUFFERING,
	V90_DOWNSTREAM_DATA_RX_B1D_FRAME,
	V90_DOWNSTREAM_DATA_RX_B1D_COMPLETE,
	V90_DOWNSTREAM_DATA_RX_DATA_FRAME,
	V90_DOWNSTREAM_DATA_RX_INVALID_ARGUMENT,
	V90_DOWNSTREAM_DATA_RX_INVALID_PARAMETERS,
	V90_DOWNSTREAM_DATA_RX_INVALID_STATE,
	V90_DOWNSTREAM_DATA_RX_INVALID_SYMBOL,
	V90_DOWNSTREAM_DATA_RX_OUT_OF_RANGE,
	V90_DOWNSTREAM_DATA_RX_INTERNAL_ERROR,
};

/* Own the fresh receive epoch that begins after Ed. The PCM decoder, GPC
 * descrambler, differential sign state, and shaping history are initialized
 * once for B1d and continue unchanged into downstream data. */
struct v90_downstream_data_rx {
	struct v90_pcm_stream_rx stream;
	uint64_t b1d_bit_errors;
	enum v90_downstream_data_rx_stage stage;
	uint8_t b1d_frames;
};

enum v90_downstream_data_rx_result v90_downstream_data_rx_init(
	struct v90_downstream_data_rx *receiver,
	const struct v90_pcm_mapping *data_mapping,
	enum v90_pcm_mapping_plane decision_plane);
bool v90_downstream_data_rx_is_valid(
	const struct v90_downstream_data_rx *receiver);
bool v90_downstream_data_rx_b1d_complete(
	const struct v90_downstream_data_rx *receiver);

/* Consume one hard decision at the known mapper phase. A complete frame is
 * always returned, including the 48th B1d frame reported by B1D_COMPLETE.
 * B1d departures from binary ones are diagnostic and do not delay DATA. */
enum v90_downstream_data_rx_result
v90_downstream_data_rx_receive_decision(
	struct v90_downstream_data_rx *receiver, uint8_t ucode,
	bool positive, struct v90_pcm_stream_frame *frame);

/* Parse and consume one authoritative G.711 network character. BUFFERING
 * accepts the character but leaves frame unchanged. Rejected calls change
 * neither receiver nor frame. */
enum v90_downstream_data_rx_result v90_downstream_data_rx_receive_octet(
	struct v90_downstream_data_rx *receiver, uint8_t network_octet,
	struct v90_pcm_stream_frame *frame);

const char *v90_downstream_data_rx_result_name(
	enum v90_downstream_data_rx_result result);

#endif /* V90_DOWNSTREAM_DATA_RX_H */
