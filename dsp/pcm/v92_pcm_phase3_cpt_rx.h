// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_PCM_PHASE3_CPT_RX_H
#define V92_PCM_PHASE3_CPT_RX_H

#include "pcm_binary_sign.h"
#include "v92_cpt_receiver.h"
#include "v92_phase3_sync.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum v92_pcm_phase3_cpt_rx_stage {
	V92_PCM_PHASE3_CPT_RX_TRAINING,
	V92_PCM_PHASE3_CPT_RX_SEARCHING,
	V92_PCM_PHASE3_CPT_RX_RECORDS,
	V92_PCM_PHASE3_CPT_RX_E1U_CANDIDATE,
	V92_PCM_PHASE3_CPT_RX_COMPLETE,
};

enum v92_pcm_phase3_cpt_rx_event_type {
	V92_PCM_PHASE3_CPT_RX_NO_EVENT,
	V92_PCM_PHASE3_CPT_RX_PARAMETERS,
	V92_PCM_PHASE3_CPT_RX_REPEAT,
	V92_PCM_PHASE3_CPT_RX_REJECTED,
	V92_PCM_PHASE3_CPT_RX_E1U,
};

struct v92_pcm_phase3_cpt_rx_event {
	enum v92_pcm_phase3_cpt_rx_event_type type;
	/* Codec error for malformed records; INVALID_FRAME for a changed repeat. */
	enum v92_cpt_result frame_result;
	size_t observed_bits;
	struct v92_cpt_parameters parameters;
};

enum v92_pcm_phase3_cpt_rx_result {
	V92_PCM_PHASE3_CPT_RX_OK,
	V92_PCM_PHASE3_CPT_RX_DONE,
	V92_PCM_PHASE3_CPT_RX_INVALID_ARGUMENT,
	V92_PCM_PHASE3_CPT_RX_INVALID_STATE,
	V92_PCM_PHASE3_CPT_RX_INTERNAL_ERROR,
};

/* Consume the polarity-normalized hard signs from the analogue modem's
 * second TRN1u, repeated CPt records, and E1u. The caller owns channel lock
 * and decides when enough TRN1u has been observed to begin marker search. */
struct v92_pcm_phase3_cpt_rx {
	struct v92_phase3_sync sync;
	struct pcm_binary_sign coding;
	struct v92_cpt_receiver records;
	/* The first CRC-checked record owns the negotiated parameters. Compare
	 * decoded repeats directly; no second serialized copy is needed. */
	struct v92_cpt_parameters parameters;
	uint8_t boundary_bits[V92_CPT_SYMBOLS_PER_FRAME];
	uint32_t matching_records;
	enum v92_pcm_phase3_cpt_rx_stage stage;
	uint8_t boundary_count;
	uint8_t boundary_ones;
};

enum v92_pcm_phase3_cpt_rx_result v92_pcm_phase3_cpt_rx_init(
	struct v92_pcm_phase3_cpt_rx *receiver);

/* Start marker search from an owned second-TRN1u synchronizer snapshot. The
 * snapshot must still be in training and contain enough signs for search. */
enum v92_pcm_phase3_cpt_rx_result
v92_pcm_phase3_cpt_rx_init_from_training(
	struct v92_pcm_phase3_cpt_rx *receiver,
	const struct v92_phase3_sync *training);
bool v92_pcm_phase3_cpt_rx_is_valid(
	const struct v92_pcm_phase3_cpt_rx *receiver);
bool v92_pcm_phase3_cpt_rx_is_complete(
	const struct v92_pcm_phase3_cpt_rx *receiver);

enum v92_pcm_phase3_cpt_rx_result v92_pcm_phase3_cpt_rx_train(
	struct v92_pcm_phase3_cpt_rx *receiver, uint8_t sign);
enum v92_pcm_phase3_cpt_rx_result v92_pcm_phase3_cpt_rx_begin(
	struct v92_pcm_phase3_cpt_rx *receiver);

/* Consume one differential CPt/E1u sign. A PARAMETERS event is emitted for
 * the first valid record, REPEAT for an identical later record, and E1U at
 * the end of the one-frame all-zero transition. One decoded error is
 * tolerated in E1u. Rejected calls change neither state nor event. */
enum v92_pcm_phase3_cpt_rx_result v92_pcm_phase3_cpt_rx_receive(
	struct v92_pcm_phase3_cpt_rx *receiver, uint8_t sign,
	struct v92_pcm_phase3_cpt_rx_event *event);

bool v92_pcm_phase3_cpt_rx_parameters(
	const struct v92_pcm_phase3_cpt_rx *receiver,
	struct v92_cpt_parameters *parameters);

/* TRN2u starts a fresh scrambler but retains this final E1u sign as its
 * differential memory. Failed calls leave previous_sign unchanged. */
bool v92_pcm_phase3_cpt_rx_previous_sign(
	const struct v92_pcm_phase3_cpt_rx *receiver,
	uint8_t *previous_sign);

#endif /* V92_PCM_PHASE3_CPT_RX_H */
