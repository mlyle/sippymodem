// SPDX-License-Identifier: BSD-3-Clause

#ifndef SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_H
#define SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_H

#include "qam_equalizer.h"
#include "v34_data_channel_receiver.h"

#include <stdbool.h>
#include <stdint.h>

enum sippymodem_v34_data_equalizer_receiver_result {
	/* A point was accepted without completing a 4-D interval. */
	SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_OK,
	/* A present-time interval was decided without a delayed mapping frame. */
	SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_INTERVAL_READY,
	SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_MAPPING_FRAME_READY,
	SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_B1_IN_PROGRESS,
	SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_B1_COMPLETE,
	SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_INVALID_ARGUMENT,
	SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_NUMERIC_FAILURE,
	SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_OUT_OF_RANGE,
};

/* Own the symbol-spaced equalizer and normalized-symbol data path. The first
 * equalized symbol and its delayed gradient remain explicit until the second
 * symbol permits one joint four-dimensional decision. */
struct sippymodem_v34_data_equalizer_receiver {
	struct sippymodem_qam_equalizer equalizer;
	struct sippymodem_qam_equalizer_observation first_observation;
	struct sippymodem_qam_point first_symbol;
	struct sippymodem_v34_data_channel_receiver data_receiver;
	double adaptation;
	uint8_t training_outputs_remaining;
	bool has_first_symbol;
};

/* Begin at the first raw B1 symbol after carrier and timing recovery. The
 * equalizer must carry the completed Phase 4 channel training and have no
 * externally retained observations. Its final delayed training outputs are
 * drained internally before B1 decisions begin. All state is copied. */
enum sippymodem_v34_data_equalizer_receiver_result
sippymodem_v34_data_equalizer_receiver_init(
	struct sippymodem_v34_data_equalizer_receiver *receiver,
	const struct sippymodem_qam_equalizer *equalizer,
	const struct sippymodem_v34_power_normalizer *normalizer,
	const struct sippymodem_v34_frame_geometry *geometry,
	bool expanded_shaping,
	const struct sippymodem_v34_q14_coefficient
		coefficients[SIPPYMODEM_V34_PRECODER_TAP_COUNT],
	enum sippymodem_v34_trellis_selection selection,
	enum sippymodem_v34_station_role transmitting_role,
	double adaptation);

bool sippymodem_v34_data_equalizer_receiver_is_valid(
	const struct sippymodem_v34_data_equalizer_receiver *receiver);
bool sippymodem_v34_data_equalizer_receiver_is_receiving_b1(
	const struct sippymodem_v34_data_equalizer_receiver *receiver);

/* Accept one finite, carrier- and timing-recovered QAM point. `current`
 * changes for every interval result. `received` changes only for a mapping or
 * B1 result. Receiver state and both outputs are unchanged on failure. */
enum sippymodem_v34_data_equalizer_receiver_result
sippymodem_v34_data_equalizer_receive_point(
	struct sippymodem_v34_data_equalizer_receiver *receiver,
	const struct sippymodem_qam_point *received_point,
	struct sippymodem_v34_survivor_decision *current,
	struct sippymodem_v34_received_mapping_frame *received);

const char *sippymodem_v34_data_equalizer_receiver_result_name(
	enum sippymodem_v34_data_equalizer_receiver_result result);

#endif /* SIPPYMODEM_V34_DATA_EQUALIZER_RECEIVER_H */
