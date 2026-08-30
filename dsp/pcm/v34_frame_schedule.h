// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_FRAME_SCHEDULE_H
#define V34_FRAME_SCHEDULE_H

#include "v34_info1.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V34_4D_INTERVALS_PER_MAPPING_FRAME = 4,
};

enum v34_frame_schedule_result {
	V34_FRAME_SCHEDULE_OK,
	V34_FRAME_SCHEDULE_FRAME_COMPLETE,
	V34_FRAME_SCHEDULE_INVALID_ARGUMENT,
	V34_FRAME_SCHEDULE_UNSUPPORTED_SELECTION,
};

struct v34_frame_geometry {
	enum v34_symbol_rate symbol_rate;
	uint16_t data_bits_per_frame;
	uint8_t primary_data_rate_2400_bps_units;
	uint8_t data_frames_per_superframe;
	uint8_t mapping_frames_per_data_frame;
	uint8_t high_mapping_frame_bits;
	uint8_t high_mapping_frame_count;
	uint8_t auxiliary_bits_per_frame;
	bool auxiliary_channel_enabled;
};

/* Derive clause 8 frame geometry using its counter construction rather than
 * storing the rate-by-symbol-rate switching table. */
enum v34_frame_schedule_result
v34_frame_geometry_init(
	struct v34_frame_geometry *geometry,
	enum v34_symbol_rate symbol_rate,
	uint8_t primary_data_rate_2400_bps_units,
	bool auxiliary_channel_enabled);
bool v34_frame_geometry_is_valid(
	const struct v34_frame_geometry *geometry);
/* Compare every derived geometry field; NULL never compares equal. */
bool v34_frame_geometry_is_equal(
	const struct v34_frame_geometry *left,
	const struct v34_frame_geometry *right);

struct v34_frame_schedule {
	struct v34_frame_geometry geometry;
	uint8_t data_frame_in_superframe;
	uint8_t next_mapping_frame;
};

struct v34_mapping_frame {
	uint8_t index;
	uint8_t data_bits;
	uint8_t primary_bits;
	/* Bit j marks inversion V0 in 4D interval j of this mapping frame. */
	uint8_t inversion_4d_mask;
	bool high_frame;
	bool carries_auxiliary_bit;
	bool completes_data_frame;
};

enum v34_frame_schedule_result
v34_frame_schedule_init(
	struct v34_frame_schedule *schedule,
	const struct v34_frame_geometry *geometry,
	uint8_t data_frame_in_superframe);

/* B1 is sent with the inversion position of the last data frame in a
 * superframe; ordinary data starts a fresh superframe after it. */
enum v34_frame_schedule_result
v34_b1_frame_schedule_init(
	struct v34_frame_schedule *schedule,
	const struct v34_frame_geometry *geometry);

bool v34_frame_schedule_is_valid(
	const struct v34_frame_schedule *schedule);
bool v34_frame_schedule_is_complete(
	const struct v34_frame_schedule *schedule);

/* Leave an in-progress schedule unchanged, or continue a completed schedule
 * at the next data frame. Invalid schedules are unchanged. */
enum v34_frame_schedule_result
v34_frame_schedule_prepare_mapping_frame(
	struct v34_frame_schedule *schedule);

/* Emit one mapping-frame description. Rejected and completed calls leave the
 * schedule and output unchanged. */
enum v34_frame_schedule_result
v34_frame_schedule_next(
	struct v34_frame_schedule *schedule,
	struct v34_mapping_frame *mapping_frame);

#endif
