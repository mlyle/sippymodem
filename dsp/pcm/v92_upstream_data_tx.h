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
	V92_UPSTREAM_DATA_TX_RM_COMPLETE,
	V92_UPSTREAM_DATA_TX_FB1U_COMPLETE,
};

/* Supply one complete post-B1u frame, in earliest-bit-first order. Initialize
 * every requested bit to zero or one; false rejects the frame. NULL emits
 * idle ones. The source owns its external read side effects; it must not
 * modify or reenter the transmitter while its frame is being fetched. */
typedef bool (*v92_upstream_frame_source)(
	void *context, uint8_t *bits, size_t bit_count);

/* Adapt the twelve-symbol upstream encoder to the 8 kHz media clock. Coding
 * histories reset once at B1u and continue unchanged into ordinary data. */
struct v92_upstream_data_tx {
	struct v92_pcm_frame_tx encoder;
	/* Consumed slots are zero; queue_count is the remaining fixed-frame tail. */
	int16_t queue[V92_PCM_MAPPING_FRAME_SYMBOLS];
	int16_t upstream_level;
	uint16_t b1u_symbols_remaining;
	/* Pending old-frame tail plus 384 RM and 24 RM-prime symbols. */
	uint16_t rm_symbols_remaining;
	/* One E2u frame followed by 48 FB1u frames in the old data mapping. */
	uint16_t finish_symbols_remaining;
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
/* True when no B1u, request, or finish sequence remains to be emitted. */
bool v92_upstream_data_tx_is_online(
	const struct v92_upstream_data_tx *transmitter);

/* Start control at an empty twelve-symbol queue after B1u. Preserve the
 * mapping, level, trellis and filters; the next source frame is control,
 * with fresh scrambler/differential coding and no new B1u. The caller owns
 * request signaling and control records. Rejected resets change nothing. */
enum v92_upstream_data_tx_result v92_upstream_data_tx_reset_control(
	struct v92_upstream_data_tx *transmitter);

/* Request fast exchange on the next data-frame boundary (V.92 9.9.2).
 * Drain the already encoded data tail, then emit 384 RM + 24 RM-prime
 * samples without fetching source bits. The final sample returns RM_COMPLETE
 * and resets only scrambler/differential state. Subsequent next() calls use
 * the caller's control source in the preceding data modulation, without B1u.
 * Reject B1u or an already pending request without changing state. */
enum v92_upstream_data_tx_result v92_upstream_data_tx_start_rm(
	struct v92_upstream_data_tx *transmitter);

/* Finish fast exchange at the complete upstream control-record boundary.
 * Emit one scrambled-zero E2u frame and 48 scrambled-one FB1u frames, without
 * resetting coding or fetching source bits. The caller must first establish
 * mutual control acknowledgment and drain the whole record, not just a frame.
 * FB1U_COMPLETE accompanies the final sample; then init() with the accepted
 * CPd and retained LU starts new-mapping B1u with fully reset coding. V.92
 * Table 30 requires the new CPd's E2u-extension bit clear during fast exchange.
 * A partial frame, B1u, or pending request/finish is rejected unchanged. */
enum v92_upstream_data_tx_result v92_upstream_data_tx_start_e2u(
	struct v92_upstream_data_tx *transmitter);

/* Emit exactly one signed sample. Acquire at most one complete source frame.
 * Never call the source during B1u, RM, E2u/FB1u, or a queued frame's tail.
 * Rejected calls leave transmitter and sample unchanged. */
enum v92_upstream_data_tx_result v92_upstream_data_tx_next(
	struct v92_upstream_data_tx *transmitter,
	v92_upstream_frame_source source, void *context, int16_t *sample);

/* Call-owner sample clock for a transmitter validated at the current media
 * block boundary. Skip only its recursive state scan; argument, alias,
 * source-failure and frame-coding checks remain in force. */
enum v92_upstream_data_tx_result v92_upstream_data_tx_next_owned(
	struct v92_upstream_data_tx *transmitter,
	v92_upstream_frame_source source, void *context, int16_t *sample);

#endif /* V92_UPSTREAM_DATA_TX_H */
