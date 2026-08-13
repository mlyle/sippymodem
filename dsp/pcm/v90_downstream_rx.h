// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_DOWNSTREAM_RX_H
#define V90_DOWNSTREAM_RX_H

#include "v90_mp_receiver.h"
#include "v90_trn2d_rx.h"

#include <stdbool.h>
#include <stdint.h>

enum v90_downstream_rx_result {
	V90_DOWNSTREAM_RX_OK,
	V90_DOWNSTREAM_RX_BUFFERING,
	V90_DOWNSTREAM_RX_TRN2D_FRAME,
	V90_DOWNSTREAM_RX_MP,
	V90_DOWNSTREAM_RX_MP_PRIME,
	V90_DOWNSTREAM_RX_MP_REJECTED,
	V90_DOWNSTREAM_RX_INVALID_ARGUMENT,
	V90_DOWNSTREAM_RX_INVALID_PARAMETERS,
	V90_DOWNSTREAM_RX_INVALID_STATE,
	V90_DOWNSTREAM_RX_INVALID_SYMBOL,
	V90_DOWNSTREAM_RX_OUT_OF_RANGE,
	V90_DOWNSTREAM_RX_INTERNAL_ERROR,
};

enum v90_downstream_rx_stage {
	V90_DOWNSTREAM_RX_TRN2D,
	V90_DOWNSTREAM_RX_MP_SEARCH,
};

/* A successful event result writes exactly one corresponding member.
 * MP and MP_PRIME differ only in the decoded acknowledge bit. */
struct v90_downstream_event {
	struct v90_trn2d_frame trn2d;
	struct v90_mp_event mp;
};

/* Own the continuous digital-to-analogue Phase 4 decoding epoch. MP follows
 * TRN2d without resetting the PCM decoder, GPC descrambler, differential
 * decoder, or spectral-shaping histories. */
struct v90_downstream_rx {
	struct v90_trn2d_rx stream;
	struct v90_mp_receiver mp;
	enum v90_downstream_rx_stage stage;
};

enum v90_downstream_rx_result v90_downstream_rx_init(
	struct v90_downstream_rx *receiver,
	const struct v90_pcm_mapping *training_mapping,
	enum v90_pcm_mapping_plane decision_plane);
bool v90_downstream_rx_is_valid(
	const struct v90_downstream_rx *receiver);

/* A fresh receiver is at the first network character following Ri-bar, with
 * every coding history and counter in its specified zero state. */
bool v90_downstream_rx_is_fresh(
	const struct v90_downstream_rx *receiver);

/* Consume one authoritative G.711 network character. The first 2040 symbols
 * remain TRN2d and produce one event per six-symbol frame. Later TRN2d bits
 * feed the saturated MP synchronizer until a complete record is accepted or
 * rejected. BUFFERING accepts the character but leaves event unchanged;
 * rejected calls change neither receiver nor event. */
enum v90_downstream_rx_result v90_downstream_rx_receive(
	struct v90_downstream_rx *receiver, uint8_t network_octet,
	struct v90_downstream_event *event);

const char *v90_downstream_rx_result_name(
	enum v90_downstream_rx_result result);

#endif /* V90_DOWNSTREAM_RX_H */
