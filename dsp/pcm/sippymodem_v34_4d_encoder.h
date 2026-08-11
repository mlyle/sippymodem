// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_4D_ENCODER_H
#define SIPPYMODEM_V34_4D_ENCODER_H

#include "sippymodem_v34_mapper.h"
#include "sippymodem_v34_parser.h"
#include "v34_precoder.h"
#include "sippymodem_v34_trellis.h"

#include <stdbool.h>
#include <stdint.h>

enum sippymodem_v34_4d_encoder_result {
	SIPPYMODEM_V34_4D_ENCODER_OK,
	SIPPYMODEM_V34_4D_ENCODER_INVALID_ARGUMENT,
	SIPPYMODEM_V34_4D_ENCODER_OUT_OF_RANGE,
};

struct sippymodem_v34_4d_encoder {
	struct sippymodem_v34_mapping_parameters parameters;
	struct sippymodem_v34_precoder precoder;
	struct sippymodem_v34_convolutional_encoder convolutional_encoder;
	/* Z(m - 1), expressed as clockwise quarter turns. */
	uint8_t previous_differential_output;
};

struct sippymodem_v34_4d_encoder_input {
	struct sippymodem_v34_4d_mapping_input mapping;
	uint8_t ring_index[SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL];
	/* V0(m), supplied by the mapping-frame schedule. */
	uint8_t inversion_bit;
};

struct sippymodem_v34_4d_encoder_output {
	/* Z(m), C0(m), Y0(m), and U0(m), respectively. */
	uint8_t differential_output;
	uint8_t modulo_output;
	uint8_t convolutional_output;
	uint8_t trellis_output;
	uint8_t subset_label[SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL];
	struct sippymodem_v34_convolutional_input convolutional_input;
	struct sippymodem_v34_2d_mapping_output
		mapped[SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL];
	struct sippymodem_v34_precoder_symbol
		precoded[SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL];
};

/* Initialize the differential, precoder, and convolutional histories to zero.
 * The destination is unchanged when any configuration value is invalid. */
enum sippymodem_v34_4d_encoder_result sippymodem_v34_4d_encoder_init(
	struct sippymodem_v34_4d_encoder *encoder,
	const struct sippymodem_v34_mapping_parameters *parameters,
	const struct sippymodem_v34_q14_coefficient
		coefficients[SIPPYMODEM_V34_PRECODER_TAP_COUNT],
	enum sippymodem_v34_trellis_selection trellis_selection);

bool sippymodem_v34_4d_encoder_is_valid(
	const struct sippymodem_v34_4d_encoder *encoder);
bool sippymodem_v34_4d_encoder_input_is_valid(
	const struct sippymodem_v34_4d_encoder_input *input,
	const struct sippymodem_v34_mapping_parameters *parameters);

/* Encode one four-dimensional interval in clause 9.6's required order. Both
 * state and output are unchanged if any intermediate value is rejected. */
enum sippymodem_v34_4d_encoder_result sippymodem_v34_4d_encoder_step(
	struct sippymodem_v34_4d_encoder *encoder,
	const struct sippymodem_v34_4d_encoder_input *input,
	struct sippymodem_v34_4d_encoder_output *output);

const char *sippymodem_v34_4d_encoder_result_name(
	enum sippymodem_v34_4d_encoder_result result);

#endif
