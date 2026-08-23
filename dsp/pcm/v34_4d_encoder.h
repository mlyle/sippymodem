// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_4D_ENCODER_H
#define V34_4D_ENCODER_H

#include "v34_mapper.h"
#include "v34_parser.h"
#include "v34_precoder.h"
#include "v34_trellis.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_4d_encoder_result {
	V34_4D_ENCODER_OK,
	V34_4D_ENCODER_INVALID_ARGUMENT,
	V34_4D_ENCODER_OUT_OF_RANGE,
};

struct v34_4d_encoder {
	struct v34_mapping_parameters parameters;
	struct v34_precoder precoder;
	struct v34_convolutional_encoder convolutional_encoder;
	/* Z(m - 1), expressed as clockwise quarter turns. */
	uint8_t previous_differential_output;
};

struct v34_4d_encoder_input {
	struct v34_4d_mapping_input mapping;
	uint8_t ring_index[V34_2D_SYMBOLS_PER_4D_INTERVAL];
	/* V0(m), supplied by the mapping-frame schedule. */
	uint8_t inversion_bit;
};

struct v34_4d_encoder_output {
	/* Z(m), C0(m), Y0(m), and U0(m), respectively. */
	uint8_t differential_output;
	uint8_t modulo_output;
	uint8_t convolutional_output;
	uint8_t trellis_output;
	uint8_t subset_label[V34_2D_SYMBOLS_PER_4D_INTERVAL];
	struct v34_convolutional_input convolutional_input;
	struct v34_2d_mapping_output
		mapped[V34_2D_SYMBOLS_PER_4D_INTERVAL];
	struct v34_precoder_symbol
		precoded[V34_2D_SYMBOLS_PER_4D_INTERVAL];
};

/* Initialize the differential, precoder, and convolutional histories to zero.
 * The destination is unchanged when any configuration value is invalid. */
enum v34_4d_encoder_result v34_4d_encoder_init(
	struct v34_4d_encoder *encoder,
	const struct v34_mapping_parameters *parameters,
	const struct v34_q14_coefficient
		coefficients[V34_PRECODER_TAP_COUNT],
	enum v34_trellis_selection trellis_selection);

bool v34_4d_encoder_is_valid(
	const struct v34_4d_encoder *encoder);
bool v34_4d_encoder_input_is_valid(
	const struct v34_4d_encoder_input *input,
	const struct v34_mapping_parameters *parameters);

/* Encode one four-dimensional interval in clause 9.6's required order. Both
 * state and output are unchanged if any intermediate value is rejected. */
enum v34_4d_encoder_result v34_4d_encoder_step(
	struct v34_4d_encoder *encoder,
	const struct v34_4d_encoder_input *input,
	struct v34_4d_encoder_output *output);

#endif
