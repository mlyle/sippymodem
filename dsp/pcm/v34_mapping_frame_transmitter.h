// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_H
#define SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_H

#include "v34_framing.h"
#include "v34_mapping_frame_encoder.h"
#include "v34_nonlinear.h"

#include <stddef.h>
#include <stdint.h>

enum {
	SIPPYMODEM_V34_SYMBOLS_PER_MAPPING_FRAME =
		SIPPYMODEM_V34_4D_INTERVALS_PER_MAPPING_FRAME *
		SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL,
};

enum sippymodem_v34_mapping_frame_transmitter_result {
	SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_OK,
	SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_FRAME_COMPLETE,
	SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_INVALID_ARGUMENT,
	SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_OUT_OF_RANGE,
	SIPPYMODEM_V34_MAPPING_FRAME_TRANSMITTER_NUMERIC_FAILURE,
};

struct sippymodem_v34_transmitted_mapping_frame {
	struct sippymodem_v34_mapping_frame mapping_frame;
	struct sippymodem_v34_encoded_mapping_frame encoded;
	/* Clause 9.7 outputs in interval-major, then symbol-major order. */
	struct sippymodem_qam_point
		projected_symbol[SIPPYMODEM_V34_SYMBOLS_PER_MAPPING_FRAME];
};

/* Transmit one scheduled mapping frame from raw primary bits. Primary data is
 * scrambled in transmission order; an auxiliary bit is never scrambled. The
 * projected symbols retain constellation-grid units and therefore still need
 * the data-mode modulation owner's power normalization. Schedule, scrambler,
 * mapping encoder, and destination are unchanged unless all eight symbols
 * succeed. */
enum sippymodem_v34_mapping_frame_transmitter_result
sippymodem_v34_transmit_mapping_frame(
	struct sippymodem_v34_frame_schedule *schedule,
	struct sippymodem_v34_scrambler *scrambler,
	struct sippymodem_v34_4d_encoder *mapping_encoder,
	const uint8_t *primary_bits, size_t primary_bit_count,
	const uint8_t *auxiliary_bit,
	enum sippymodem_v34_nonlinear_parameter nonlinear_parameter,
	double mean_squared_magnitude,
	struct sippymodem_v34_transmitted_mapping_frame *transmitted);

const char *sippymodem_v34_mapping_frame_transmitter_result_name(
	enum sippymodem_v34_mapping_frame_transmitter_result result);

#endif
