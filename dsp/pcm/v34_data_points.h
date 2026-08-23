// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_DATA_POINTS_H
#define V34_DATA_POINTS_H

#include "v34_mapping_frame_transmitter.h"
#include "v34_symbol_power.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum v34_data_points_result {
	V34_DATA_POINTS_OK,
	V34_DATA_POINTS_MAPPING_FRAME_REQUIRED,
	V34_DATA_POINTS_MAPPING_FRAME_PENDING,
	V34_DATA_POINTS_INVALID_ARGUMENT,
	V34_DATA_POINTS_OUT_OF_RANGE,
	V34_DATA_POINTS_NUMERIC_FAILURE,
};

/* Own the continuously evolving data mapper and one bounded normalized
 * mapping frame. Carrier, pulse, filter, gain, and sample-clock state remain
 * the responsibility of the line owner that consumes these points. */
struct v34_data_points {
	struct v34_frame_schedule schedule;
	struct v34_scrambler scrambler;
	struct v34_4d_encoder encoder;
	struct v34_power_normalizer normalizer;
	struct qam_point point[V34_SYMBOLS_PER_MAPPING_FRAME];
	enum v34_station_role local_role;
	uint8_t next_point;
};

/* Copy prepared mapping state at a mapping-frame boundary. The destination
 * and every input remain unchanged on failure. */
enum v34_data_points_result v34_data_points_init(
	struct v34_data_points *points,
	const struct v34_frame_schedule *schedule,
	const struct v34_scrambler *scrambler,
	const struct v34_4d_encoder *encoder,
	const struct v34_power_normalizer *normalizer,
	enum v34_station_role local_role);

bool v34_data_points_is_valid(
	const struct v34_data_points *points);
bool v34_data_points_needs_mapping_frame(
	const struct v34_data_points *points);

/* Describe the next frame without advancing any state. */
enum v34_data_points_result v34_data_points_preview_mapping_frame(
	const struct v34_data_points *points,
	struct v34_mapping_frame *mapping_frame);

/* Generate, normalize, and queue all eight points atomically. A completed
 * data-frame schedule advances automatically, including superframe wrap. */
enum v34_data_points_result v34_data_points_queue_mapping_frame(
	struct v34_data_points *points,
	const uint8_t *primary_bits, size_t primary_bit_count,
	const uint8_t *auxiliary_bit);

/* Consume one normalized point. Empty-frame backpressure and all failures
 * leave the stream and destination unchanged. */
enum v34_data_points_result v34_data_points_next(
	struct v34_data_points *points, struct qam_point *point);

#endif
