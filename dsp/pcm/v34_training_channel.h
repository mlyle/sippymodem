// SPDX-License-Identifier: BSD-3-Clause

#ifndef V34_TRAINING_CHANNEL_H
#define V34_TRAINING_CHANNEL_H

#include "qam_gain.h"
#include "v34_constellation.h"

#include <stdint.h>

enum v34_training_channel_result {
	V34_TRAINING_CHANNEL_OK,
	V34_TRAINING_CHANNEL_INVALID_ARGUMENT,
};

/* Equalize one raw channel point, slice the Phase 3/4 training
 * constellation, and decision-direct the scalar gain estimate. Rejected
 * calls leave the tracker and both labels unchanged. */
enum v34_training_channel_result
v34_training_channel_decide(
	struct qam_gain *gain,
	enum v34_constellation_size constellation,
	const struct qam_point *received, uint8_t *quarter_point,
	uint8_t *clockwise_quarter_turns);

const char *v34_training_channel_result_name(
	enum v34_training_channel_result result);

#endif
