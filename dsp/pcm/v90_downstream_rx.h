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
	V90_DOWNSTREAM_RX_ED,
	V90_DOWNSTREAM_RX_ED_REJECTED,
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
	V90_DOWNSTREAM_RX_ED_SEARCH,
	V90_DOWNSTREAM_RX_COMPLETE,
};

struct v90_downstream_ed_event {
	uint8_t bit_count;
	uint8_t bit_errors;
};

/* A successful event result writes exactly one corresponding member. MP and
 * MP_PRIME differ only in the decoded acknowledge bit. ED_REJECTED reports
 * the number of nonzero descrambled bits across its exact two-frame window. */
struct v90_downstream_event {
	struct v90_trn2d_frame trn2d;
	struct v90_mp_event mp;
	struct v90_downstream_ed_event ed;
};

/* Own the continuous digital-to-analogue Phase 4 decoding epoch. MP follows
 * TRN2d without resetting the PCM decoder, GPC descrambler, differential
 * decoder, or spectral-shaping histories. */
struct v90_downstream_rx {
	struct v90_trn2d_rx stream;
	struct v90_mp_receiver mp;
	enum v90_downstream_rx_stage stage;
	uint8_t ed_frames;
	uint8_t ed_bit_errors;
	bool ed_requested;
	bool mp_prime_boundary;
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

/* Arm the transition to Ed after the local CP-prime/E prerequisite. A request
 * made immediately after MP-prime starts the search at that boundary; an
 * earlier or mid-record request is latched. Whole repeated MP-prime records
 * remain valid while the sender drains already accepted spectral lookahead. */
enum v90_downstream_rx_result v90_downstream_rx_expect_ed(
	struct v90_downstream_rx *receiver);

bool v90_downstream_rx_is_complete(
	const struct v90_downstream_rx *receiver);
bool v90_downstream_rx_has_failed(
	const struct v90_downstream_rx *receiver);

/* Consume one authoritative G.711 network character. The first 2040 symbols
 * remain TRN2d and produce one event per six-symbol frame. Later TRN2d bits
 * feed the saturated MP synchronizer until a complete record is accepted or
 * rejected. Once armed, two frames that contain no newer MP prefix form Ed.
 * BUFFERING accepts the character but leaves event unchanged; rejected calls
 * change neither receiver nor event. */
enum v90_downstream_rx_result v90_downstream_rx_receive(
	struct v90_downstream_rx *receiver, uint8_t network_octet,
	struct v90_downstream_event *event);

const char *v90_downstream_rx_result_name(
	enum v90_downstream_rx_result result);

#endif /* V90_DOWNSTREAM_RX_H */
