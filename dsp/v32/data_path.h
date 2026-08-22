// SPDX-License-Identifier: BSD-3-Clause

#ifndef V32_DATA_PATH_H
#define V32_DATA_PATH_H

#include "b1_path.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	/* One line sample can release a complete retained timing block. */
	V32_DATA_PATH_MAX_GROUPS = V32_RECEIVE_PATH_RECOVERED_CAPACITY,
	V32_DATA_RETRAIN_OBSERVATIONS = 60,
};

enum v32_data_path_result {
	V32_DATA_PATH_OK,
	V32_DATA_PATH_INVALID_ARGUMENT,
	V32_DATA_PATH_INVALID_STATE,
};

struct v32_data_group {
	uint8_t value;
	uint8_t bit_count;
};

struct v32_data_batch {
	struct v32_data_group groups[V32_DATA_PATH_MAX_GROUPS];
	uint8_t count;
	bool retrain_requested;
};

/* Online retrain is a repeated-pair decision pattern in the persistent
 * equalizer coordinate.  Keep that evidence with the data owner rather than
 * reacquiring an unrelated raw line tone beside the trained receive path. */
struct v32_data_retrain_detector {
	int16_t i_last;
	int16_t q_last;
	int16_t i_previous;
	int16_t q_previous;
	int16_t average_error;
	uint16_t repeated_pair_observations;
};

/* Continue the selected-rate semantic receiver and physical lane retained at
 * the B1 boundary.  Trellis reception drains its delayed B1 decisions before
 * publishing the first payload group. */
struct v32_data_path {
	struct v32_b1_path b1;
	struct v32_data_retrain_detector retrain;
};

enum v32_data_path_result v32_data_path_init(
	struct v32_data_path *path,
	const struct v32_b1_path *b1,
	struct v32_data_batch *batch);
bool v32_data_path_is_valid(
	const struct v32_data_path *path);

/* Consume one 8 kHz receive sample after its matching transmit sample.  A
 * single sample may release several retained timing decisions, all returned
 * in order in the bounded batch. */
enum v32_data_path_result v32_data_path_sample(
	struct v32_data_path *path, int16_t received,
	const struct v32_echo_reference *accepted_reference,
	struct v32_data_batch *batch);

#endif
