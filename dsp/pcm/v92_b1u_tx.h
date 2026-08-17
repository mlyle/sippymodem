// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_B1U_TX_H
#define V92_B1U_TX_H

#include "v92_pcm_frame.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V92_B1U_FRAME_COUNT = 48,
	V92_B1U_SYMBOL_COUNT =
		V92_B1U_FRAME_COUNT * V92_PCM_MAPPING_FRAME_SYMBOLS,
};

enum v92_b1u_tx_result {
	V92_B1U_TX_OK,
	V92_B1U_TX_COMPLETE,
	V92_B1U_TX_FAILED,
	V92_B1U_TX_INVALID_ARGUMENT,
	V92_B1U_TX_INVALID_PARAMETERS,
	V92_B1U_TX_INVALID_STATE,
	V92_B1U_TX_NUMERIC_FAILURE,
};

/* Own the 48 all-one frames and the zero-initialized coding/filter state that
 * continues directly into PCM-upstream data. */
struct v92_b1u_tx {
	struct v92_pcm_frame_tx frame_tx;
	struct v92_pcm_frame frame;
	uint16_t emitted_symbols;
	int16_t upstream_level;
	bool failed;
};

enum v92_b1u_tx_result v92_b1u_tx_init(
	struct v92_b1u_tx *tx, const struct v92_pcm_mapping *mapping,
	int16_t upstream_level);
bool v92_b1u_tx_is_valid(const struct v92_b1u_tx *tx);
bool v92_b1u_tx_is_complete(const struct v92_b1u_tx *tx);
bool v92_b1u_tx_has_failed(const struct v92_b1u_tx *tx);

/* Emit one signed 8 kHz sample. COMPLETE consumes no symbol and leaves sample
 * unchanged. */
enum v92_b1u_tx_result v92_b1u_tx_next(
	struct v92_b1u_tx *tx, int16_t *sample);

/* Export the exact post-B1u coder for the data owner. */
enum v92_b1u_tx_result v92_b1u_tx_handoff(
	const struct v92_b1u_tx *tx,
	struct v92_pcm_frame_tx *frame_tx);

const char *v92_b1u_tx_result_name(enum v92_b1u_tx_result result);

#endif /* V92_B1U_TX_H */
