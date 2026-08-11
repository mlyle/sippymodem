// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_CHANNEL_DECODER_H
#define SIPPYMODEM_V34_CHANNEL_DECODER_H

#include "v34_lattice.h"
#include "v34_path_decoder.h"

#include <stdbool.h>
#include <stdint.h>

enum sippymodem_v34_channel_decoder_result {
	SIPPYMODEM_V34_CHANNEL_DECODER_OK,
	SIPPYMODEM_V34_CHANNEL_DECODER_INVALID_ARGUMENT,
	SIPPYMODEM_V34_CHANNEL_DECODER_NUMERIC_FAILURE,
	SIPPYMODEM_V34_CHANNEL_DECODER_OUT_OF_RANGE,
};

struct sippymodem_v34_channel_decoder {
	struct sippymodem_v34_mapping_parameters parameters;
	struct sippymodem_v34_survivor survivor;
	struct sippymodem_v34_precoder inverse_precoder;
};

/* Initialize the convolutional and inverse-precoder histories to their B1
 * all-zero state. The destination is unchanged on rejection. */
enum sippymodem_v34_channel_decoder_result
sippymodem_v34_channel_decoder_init(
	struct sippymodem_v34_channel_decoder *decoder,
	const struct sippymodem_v34_mapping_parameters *parameters,
	const struct sippymodem_v34_q14_coefficient
		coefficients[SIPPYMODEM_V34_PRECODER_TAP_COUNT],
	enum sippymodem_v34_trellis_selection selection);

bool sippymodem_v34_channel_decoder_is_valid(
	const struct sippymodem_v34_channel_decoder *decoder);

/* Decode one pair of already conditioned integer-grid channel estimates.
 * Scaling, equalization, timing recovery, and nonlinear inversion precede this
 * boundary. `current` always receives the present-time hard decision for
 * channel tracking. `decided` is unchanged until `decided_ready` becomes true
 * at the survivor traceback depth, then receives one mapper interval in
 * transmission order. State and every output are unchanged on failure. */
enum sippymodem_v34_channel_decoder_result
sippymodem_v34_channel_decoder_advance(
	struct sippymodem_v34_channel_decoder *decoder,
	const struct sippymodem_qam_point
		estimates[SIPPYMODEM_V34_2D_SYMBOLS_PER_4D_INTERVAL],
	uint8_t inversion_bit,
	struct sippymodem_v34_survivor_decision *current,
	struct sippymodem_v34_decided_4d_interval *decided,
	bool *decided_ready);

const char *sippymodem_v34_channel_decoder_result_name(
	enum sippymodem_v34_channel_decoder_result result);

#endif /* SIPPYMODEM_V34_CHANNEL_DECODER_H */
