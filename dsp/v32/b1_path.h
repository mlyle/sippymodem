// SPDX-License-Identifier: BSD-3-Clause

#ifndef V32_B1_PATH_H
#define V32_B1_PATH_H

#include "end_path.h"
#include "trellis_receiver.h"
#include "uncoded_receiver.h"

#include <stdbool.h>
#include <stdint.h>

enum v32_b1_path_result {
	V32_B1_PATH_OK,
	V32_B1_PATH_COMPLETE,
	V32_B1_PATH_INVALID_ARGUMENT,
	V32_B1_PATH_INVALID_STATE,
};

enum v32_b1_mode {
	V32_B1_MODE_UNCODED,
	V32_B1_MODE_TRELLIS,
};

union v32_b1_receiver {
	struct v32_uncoded_receiver uncoded;
	struct v32_trellis_receiver trellis;
};

/* Own the selected-rate receive lane through 128 startup or 24 rate-change
 * B1 symbols.  A complete owner retains the semantic receiver and any already
 * recovered first payload observation for the following data path. */
struct v32_b1_path {
	struct v32_receive_path receive;
	union v32_b1_receiver receiver;
	struct v32_rate_offer selected_offer;
	uint64_t observation_epoch;
	uint8_t observations;
	uint8_t symbols;
	uint8_t data_bits_per_symbol;
	enum v32_b1_mode mode;
	/* Evidence does not gate the configured B1 transition.  Trellis evidence
	 * excludes the final delayed traceback symbols. */
	bool b1_matches;
};

enum v32_b1_path_result v32_b1_path_init(
	struct v32_b1_path *path,
	const struct v32_end_path_handoff *end, uint8_t symbols);
bool v32_b1_path_is_valid(
	const struct v32_b1_path *path);
bool v32_b1_path_is_complete(
	const struct v32_b1_path *path);

/* Consume one 8 kHz receive sample after its matching transmit sample. */
enum v32_b1_path_result v32_b1_path_sample(
	struct v32_b1_path *path, int16_t received,
	const struct v32_echo_reference *accepted_reference);

#endif
