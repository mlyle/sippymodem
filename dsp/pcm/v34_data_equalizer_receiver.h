// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_DATA_EQUALIZER_RECEIVER_H
#define V34_DATA_EQUALIZER_RECEIVER_H

#include "qam_equalizer.h"
#include "v34_data_channel_receiver.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_data_equalizer_receiver_result {
	/* A point was accepted without completing a 4-D interval. */
	V34_DATA_EQUALIZER_RECEIVER_OK,
	/* A present-time interval was decided without a delayed mapping frame. */
	V34_DATA_EQUALIZER_RECEIVER_INTERVAL_READY,
	V34_DATA_EQUALIZER_RECEIVER_MAPPING_FRAME_READY,
	V34_DATA_EQUALIZER_RECEIVER_B1_IN_PROGRESS,
	V34_DATA_EQUALIZER_RECEIVER_B1_COMPLETE,
	V34_DATA_EQUALIZER_RECEIVER_INVALID_ARGUMENT,
	V34_DATA_EQUALIZER_RECEIVER_NUMERIC_FAILURE,
	V34_DATA_EQUALIZER_RECEIVER_OUT_OF_RANGE,
};

/* Own the symbol-spaced equalizer and normalized-symbol data path. The first
 * equalized symbol and its delayed gradient remain explicit until the second
 * symbol permits one joint four-dimensional decision. */
struct v34_data_equalizer_receiver {
	struct sippymodem_qam_equalizer equalizer;
	struct sippymodem_qam_equalizer_observation first_observation;
	struct sippymodem_qam_point first_symbol;
	struct v34_data_channel_receiver data_receiver;
	double adaptation;
	uint8_t training_outputs_remaining;
	bool has_first_symbol;
};

/* Begin at the first raw B1 symbol after carrier and timing recovery. The
 * equalizer must carry the completed Phase 4 channel training and have no
 * externally retained observations. Its final delayed training outputs are
 * drained internally before B1 decisions begin. All state is copied. */
enum v34_data_equalizer_receiver_result
v34_data_equalizer_receiver_init(
	struct v34_data_equalizer_receiver *receiver,
	const struct sippymodem_qam_equalizer *equalizer,
	const struct v34_power_normalizer *normalizer,
	const struct v34_frame_geometry *geometry,
	bool expanded_shaping,
	const struct v34_q14_coefficient
		coefficients[V34_PRECODER_TAP_COUNT],
	enum v34_trellis_selection selection,
	enum v34_station_role transmitting_role,
	double adaptation);

bool v34_data_equalizer_receiver_is_valid(
	const struct v34_data_equalizer_receiver *receiver);
bool v34_data_equalizer_receiver_is_receiving_b1(
	const struct v34_data_equalizer_receiver *receiver);

/* Accept one finite, carrier- and timing-recovered QAM point. `current`
 * changes for every interval result. `received` changes only for a mapping or
 * B1 result. Receiver state and both outputs are unchanged on failure. */
enum v34_data_equalizer_receiver_result
v34_data_equalizer_receive_point(
	struct v34_data_equalizer_receiver *receiver,
	const struct sippymodem_qam_point *received_point,
	struct v34_survivor_decision *current,
	struct v34_received_mapping_frame *received);

const char *v34_data_equalizer_receiver_result_name(
	enum v34_data_equalizer_receiver_result result);

#endif /* V34_DATA_EQUALIZER_RECEIVER_H */
