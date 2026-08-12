// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_DATA_LINE_RECEIVER_H
#define V34_DATA_LINE_RECEIVER_H

#include "v34_data_carrier.h"
#include "v34_data_equalizer_receiver.h"

#include <stdbool.h>
#include <stdint.h>

enum v34_data_line_receiver_result {
	/* A line sample was accepted without completing a 4-D interval. */
	V34_DATA_LINE_RECEIVER_OK,
	V34_DATA_LINE_RECEIVER_INTERVAL_READY,
	V34_DATA_LINE_RECEIVER_MAPPING_FRAME_READY,
	V34_DATA_LINE_RECEIVER_B1_IN_PROGRESS,
	V34_DATA_LINE_RECEIVER_B1_COMPLETE,
	/* A prior sample reached an unrecoverable decoding boundary. */
	V34_DATA_LINE_RECEIVER_FAILED,
	V34_DATA_LINE_RECEIVER_INVALID_ARGUMENT,
	V34_DATA_LINE_RECEIVER_NUMERIC_FAILURE,
	V34_DATA_LINE_RECEIVER_OUT_OF_RANGE,
};

struct v34_data_line_receiver_configuration {
	struct v34_phase3_profile transmitting_profile;
	struct v34_frame_geometry geometry;
	struct v34_power_normalizer normalizer;
	struct v34_q14_coefficient
		precoder_coefficients[V34_PRECODER_TAP_COUNT];
	double equalizer_adaptation;
	enum v34_trellis_selection trellis_selection;
	bool expanded_shaping;
};

/* Own the continuous sample-to-data path after Phase 4 has established
 * carrier timing and trained the symbol equalizer. A terminal decoding error
 * is latched because its line sample has already advanced carrier timing and
 * cannot be replayed safely. */
struct v34_data_line_receiver {
	struct qam_demodulator demodulator;
	struct v34_data_equalizer_receiver equalizer_receiver;
	struct v34_phase3_profile transmitting_profile;
	struct v34_frame_geometry geometry;
	bool failed;
};

/* Begin after Phase 4 has decoded E and retained the first equalizer-delay B1
 * inputs. The demodulator and handoff must describe that same sample boundary.
 * Both trained values and the negotiated configuration are copied; the
 * demodulator must hold timing lock for the exact waveform. */
enum v34_data_line_receiver_result v34_data_line_receiver_init(
	struct v34_data_line_receiver *receiver,
	const struct qam_demodulator *demodulator,
	const struct v34_equalizer_handoff *handoff,
	const struct v34_data_line_receiver_configuration *configuration);

bool v34_data_line_receiver_is_valid(
	const struct v34_data_line_receiver *receiver);
bool v34_data_line_receiver_has_failed(
	const struct v34_data_line_receiver *receiver);
bool v34_data_line_receiver_is_receiving_b1(
	const struct v34_data_line_receiver *receiver);

/* Consume one signed line sample. `current` changes for every interval result;
 * `received` changes only for a mapping-frame or B1 result. Invalid calls and
 * calls after failure leave the receiver and both outputs unchanged. */
enum v34_data_line_receiver_result v34_data_line_receiver_sample(
	struct v34_data_line_receiver *receiver, int16_t sample,
	struct v34_survivor_decision *current,
	struct v34_received_mapping_frame *received);

const char *v34_data_line_receiver_result_name(
	enum v34_data_line_receiver_result result);

#endif /* V34_DATA_LINE_RECEIVER_H */
