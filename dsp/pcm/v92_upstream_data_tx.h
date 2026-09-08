// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_UPSTREAM_DATA_TX_H
#define V92_UPSTREAM_DATA_TX_H

#include "v92_pcm_frame.h"

enum v92_upstream_data_tx_result {
	V92_UPSTREAM_DATA_TX_OK,
	V92_UPSTREAM_DATA_TX_SOURCE_FAILURE,
	V92_UPSTREAM_DATA_TX_INVALID_ARGUMENT,
	V92_UPSTREAM_DATA_TX_INVALID_PARAMETERS,
	V92_UPSTREAM_DATA_TX_INVALID_STATE,
	V92_UPSTREAM_DATA_TX_OUT_OF_RANGE,
	V92_UPSTREAM_DATA_TX_NUMERIC_FAILURE,
};

/* Supply one complete post-B1u frame, in earliest-bit-first order. Initialize
 * every requested bit to zero or one; false rejects the frame. NULL emits
 * idle ones. The source owns its external read side effects. */
typedef bool (*v92_upstream_frame_source)(
	void *context, uint8_t *bits, size_t bit_count);

/* Adapt the twelve-symbol upstream encoder to the 8 kHz media clock. Coding
 * histories reset once at B1u and continue unchanged into ordinary data. */
struct v92_upstream_data_tx {
	struct v92_pcm_frame_tx encoder;
	int16_t queue[V92_PCM_MAPPING_FRAME_SYMBOLS];
	int16_t upstream_level;
	uint16_t b1u_symbols_remaining;
	uint8_t queue_count;
};

/* The caller supplies the accepted CPd mapping and trained LU at the boundary
 * immediately after E2u. No TRN2u scrambler or differential history survives
 * into B1u (V.92 8.7.1). Rejected calls leave the transmitter unchanged. */
enum v92_upstream_data_tx_result v92_upstream_data_tx_init(
	struct v92_upstream_data_tx *transmitter,
	const struct v92_pcm_mapping *mapping, int16_t upstream_level);
bool v92_upstream_data_tx_is_valid(
	const struct v92_upstream_data_tx *transmitter);
/* True only after all 576 B1u samples have left the transmitter. */
bool v92_upstream_data_tx_is_online(
	const struct v92_upstream_data_tx *transmitter);

/* Start control at an empty twelve-symbol queue after B1u. Preserve the
 * mapping, level, trellis and filters; the next source frame is control,
 * with fresh scrambler/differential coding and no new B1u. The caller owns
 * request signaling and control records. Rejected resets change nothing. */
enum v92_upstream_data_tx_result v92_upstream_data_tx_reset_control(
	struct v92_upstream_data_tx *transmitter);

/* Emit exactly one signed sample. Acquire at most one complete source frame,
 * and never call the source while sending B1u or draining a queued frame.
 * Rejected calls leave transmitter and sample unchanged. */
enum v92_upstream_data_tx_result v92_upstream_data_tx_next(
	struct v92_upstream_data_tx *transmitter,
	v92_upstream_frame_source source, void *context, int16_t *sample);

#endif /* V92_UPSTREAM_DATA_TX_H */
