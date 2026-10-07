// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_DOWNSTREAM_DATA_TX_H
#define V90_DOWNSTREAM_DATA_TX_H

#include "v34_framing.h"
#include "v90_pcm_frame_encoder.h"
#include "v90_phase4.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum v90_downstream_data_tx_stage {
	V90_DOWNSTREAM_DATA_TX_STAGE_B1D,
	V90_DOWNSTREAM_DATA_TX_STAGE_DATA,
	V90_DOWNSTREAM_DATA_TX_STAGE_FINISHING,
	V90_DOWNSTREAM_DATA_TX_STAGE_COMPLETE,
};

enum v90_downstream_data_tx_result {
	V90_DOWNSTREAM_DATA_TX_OK,
	V90_DOWNSTREAM_DATA_TX_BUFFERING,
	V90_DOWNSTREAM_DATA_TX_SYMBOLS_READY,
	V90_DOWNSTREAM_DATA_TX_COMPLETE,
	V90_DOWNSTREAM_DATA_TX_INVALID_ARGUMENT,
	V90_DOWNSTREAM_DATA_TX_INVALID_PARAMETERS,
	V90_DOWNSTREAM_DATA_TX_INVALID_STATE,
	V90_DOWNSTREAM_DATA_TX_INVALID_BITS,
	V90_DOWNSTREAM_DATA_TX_NUMERIC_FAILURE,
	V90_DOWNSTREAM_DATA_TX_INTERNAL_ERROR,
};

/* Own the fresh data-coding epoch that begins after Ed. The GPC scrambler,
 * differential sign encoder, and spectral shaper are initialized once for
 * B1d and continue unchanged into downstream data. */
struct v90_downstream_data_tx {
	struct v90_pcm_frame_encoder encoder;
	struct v34_scrambler scrambler;
	enum v90_downstream_data_tx_stage stage;
	uint8_t b1d_frames;
};

enum v90_downstream_data_tx_result v90_downstream_data_tx_init(
	struct v90_downstream_data_tx *transmitter,
	const struct v90_pcm_mapping *data_mapping);
bool v90_downstream_data_tx_is_valid(
	const struct v90_downstream_data_tx *transmitter);
bool v90_downstream_data_tx_b1d_complete(
	const struct v90_downstream_data_tx *transmitter);
bool v90_downstream_data_tx_is_complete(
	const struct v90_downstream_data_tx *transmitter);

/* Accept one of B1d's 48 all-one source frames. Spectral lookahead can leave
 * the frame buffered or emit symbols belonging to earlier accepted frames.
 * Query b1d_complete after the call to detect the DATA handoff. */
enum v90_downstream_data_tx_result v90_downstream_data_tx_next_b1d(
	struct v90_downstream_data_tx *transmitter,
	struct v90_pcm_frame_output *output);

/* Accept exactly one negotiated D-bit data frame in transmission order.
 * This is valid only after all 48 B1d source frames have been accepted. */
enum v90_downstream_data_tx_result v90_downstream_data_tx_process(
	struct v90_downstream_data_tx *transmitter,
	const uint8_t *bits, size_t bit_count,
	struct v90_pcm_frame_output *output);

/* Drain delayed shaping output after DATA has ended. Once first called, no
 * further input is accepted. A live connection normally remains in DATA and
 * need not drain its discarded tail during teardown. */
enum v90_downstream_data_tx_result v90_downstream_data_tx_finish(
	struct v90_downstream_data_tx *transmitter,
	struct v90_pcm_frame_output *output);

#endif /* V90_DOWNSTREAM_DATA_TX_H */
