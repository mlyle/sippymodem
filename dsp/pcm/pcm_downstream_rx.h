// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_DOWNSTREAM_RX_H
#define PCM_DOWNSTREAM_RX_H

#include "v90_mp_receiver.h"
#include "v90_pcm_stream_rx.h"
#include "v90_phase4.h"

#include <stdbool.h>
#include <stdint.h>

enum pcm_downstream_rx_result {
	PCM_DOWNSTREAM_RX_OK,
	PCM_DOWNSTREAM_RX_BUFFERING,
	PCM_DOWNSTREAM_RX_TRN2D_FRAME,
	PCM_DOWNSTREAM_RX_MP,
	PCM_DOWNSTREAM_RX_MP_PRIME,
	PCM_DOWNSTREAM_RX_MP_REJECTED,
	PCM_DOWNSTREAM_RX_ED,
	PCM_DOWNSTREAM_RX_ED_REJECTED,
	PCM_DOWNSTREAM_RX_INVALID_ARGUMENT,
	PCM_DOWNSTREAM_RX_INVALID_PARAMETERS,
	PCM_DOWNSTREAM_RX_INVALID_STATE,
	PCM_DOWNSTREAM_RX_INVALID_SYMBOL,
	PCM_DOWNSTREAM_RX_OUT_OF_RANGE,
	PCM_DOWNSTREAM_RX_INTERNAL_ERROR,
};

enum pcm_downstream_rx_stage {
	PCM_DOWNSTREAM_RX_TRN2D,
	PCM_DOWNSTREAM_RX_MP_SEARCH,
	PCM_DOWNSTREAM_RX_ED_SEARCH,
	PCM_DOWNSTREAM_RX_COMPLETE,
};

struct pcm_downstream_ed_event {
	uint8_t bit_count;
	uint8_t bit_errors;
};

struct pcm_trn2d_frame {
	uint8_t bits[V90_PCM_FRAME_MAX_BITS];
	enum v90_spectral_rule rules[3];
	uint8_t bit_count;
	uint8_t rule_count;
	uint8_t training_bit_errors;
};

/* A successful event result writes exactly one corresponding member. MP and
 * MP_PRIME differ only in the decoded acknowledge bit. ED_REJECTED reports
 * the number of nonzero descrambled bits across its exact two-frame window. */
struct pcm_downstream_event {
	struct pcm_trn2d_frame trn2d;
	struct v90_mp_event mp;
	struct pcm_downstream_ed_event ed;
};

/* Own the continuous digital-to-analogue Phase 4 decoding epoch. MP follows
 * TRN2d without resetting the PCM decoder, GPC descrambler, differential
 * decoder, or spectral-shaping histories. */
struct pcm_downstream_rx {
	struct v90_pcm_stream_rx stream;
	struct v90_mp_receiver mp;
	uint64_t trn2d_bit_errors;
	enum pcm_downstream_rx_stage stage;
	uint8_t ed_frames;
	uint8_t ed_bit_errors;
	bool ed_requested;
	bool mp_prime_boundary;
};

enum pcm_downstream_rx_result pcm_downstream_rx_init(
	struct pcm_downstream_rx *receiver,
	const struct v90_pcm_mapping *training_mapping,
	enum v90_pcm_mapping_plane decision_plane);
bool pcm_downstream_rx_is_valid(
	const struct pcm_downstream_rx *receiver);

/* A fresh receiver is at the first network character following Ri-bar, with
 * every coding history and counter in its specified zero state. */
bool pcm_downstream_rx_is_fresh(
	const struct pcm_downstream_rx *receiver);

/* Arm the transition to Ed after the local CP-prime/E prerequisite. A request
 * made immediately after MP-prime starts the search at that boundary; an
 * earlier or mid-record request is latched. Whole repeated MP-prime records
 * remain valid while the sender drains already accepted spectral lookahead. */
enum pcm_downstream_rx_result pcm_downstream_rx_expect_ed(
	struct pcm_downstream_rx *receiver);

bool pcm_downstream_rx_is_complete(
	const struct pcm_downstream_rx *receiver);
bool pcm_downstream_rx_has_failed(
	const struct pcm_downstream_rx *receiver);

/* Consume one authoritative G.711 network character. The first 2040 symbols
 * remain TRN2d and produce one event per six-symbol frame. Later TRN2d bits
 * feed the saturated MP synchronizer until a complete record is accepted or
 * rejected. Once armed, two frames that contain no newer MP prefix form Ed.
 * BUFFERING accepts the character but leaves event unchanged; rejected calls
 * change neither receiver nor event. */
enum pcm_downstream_rx_result pcm_downstream_rx_receive(
	struct pcm_downstream_rx *receiver, uint8_t network_octet,
	struct pcm_downstream_event *event);

const char *pcm_downstream_rx_result_name(
	enum pcm_downstream_rx_result result);

#endif /* PCM_DOWNSTREAM_RX_H */
