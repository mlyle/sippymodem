// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_CHANNEL_DECODER_H
#define V34_CHANNEL_DECODER_H

#include "v34_16_state_branches.h"
#include "v34_path_decoder.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_channel_decoder_result {
	V34_CHANNEL_DECODER_OK,
	V34_CHANNEL_DECODER_INVALID_ARGUMENT,
	V34_CHANNEL_DECODER_NUMERIC_FAILURE,
	V34_CHANNEL_DECODER_OUT_OF_RANGE,
};

struct v34_channel_decoder {
	struct v34_frame_geometry geometry;
	struct v34_mapping_parameters parameters;
	enum v34_trellis_selection selection;
	union {
		struct {
			struct v34_16_state_branch_profile profile;
			struct v34_16_state_survivor survivor;
		} fixed_16;
		struct v34_survivor generic;
	} path;
	struct v34_precoder inverse_precoder;
};

/* Bind the mapping geometry to the fixed 16-state path or the floating
 * 32/64-state path, then initialize its convolutional and inverse-precoder
 * histories to their B1 all-zero state. The destination is unchanged on
 * rejection. */
enum v34_channel_decoder_result
v34_channel_decoder_init(
	struct v34_channel_decoder *decoder,
	const struct v34_frame_geometry *geometry,
	bool expanded_shaping,
	const struct v34_q14_coefficient
		coefficients[V34_PRECODER_TAP_COUNT],
	enum v34_trellis_selection selection);

bool v34_channel_decoder_is_valid(
	const struct v34_channel_decoder *decoder);

/* Decode one pair of already conditioned integer-grid channel estimates.
 * Scaling, equalization, timing recovery, and nonlinear inversion precede this
 * boundary. `current` always receives the present-time hard decision for
 * channel tracking. `decided` is unchanged until `decided_ready` becomes true
 * at the survivor traceback depth, then receives one mapper interval in
 * transmission order. State and every output are unchanged on failure. */
enum v34_channel_decoder_result
v34_channel_decoder_advance(
	struct v34_channel_decoder *decoder,
	const struct qam_point
		estimates[V34_2D_SYMBOLS_PER_4D_INTERVAL],
	uint8_t inversion_bit,
	struct v34_survivor_decision *current,
	struct v34_decided_4d_interval *decided,
	bool *decided_ready);

#endif /* V34_CHANNEL_DECODER_H */
