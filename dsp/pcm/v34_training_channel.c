// SPDX-License-Identifier: BSD-3-Clause

#include "v34_training_channel.h"

enum v34_training_channel_result
v34_training_channel_decide(
	struct qam_gain *gain,
	enum v34_constellation_size constellation,
	const struct qam_point *received, uint8_t *quarter_point,
	uint8_t *clockwise_quarter_turns)
{
	struct qam_gain next;
	struct qam_point normalized;
	struct qam_point decision;
	uint8_t next_quarter_point;
	uint8_t next_clockwise_quarter_turns;

	if (!qam_gain_is_valid(gain) || quarter_point == NULL ||
	    clockwise_quarter_turns == NULL ||
	    quarter_point == clockwise_quarter_turns)
		return V34_TRAINING_CHANNEL_INVALID_ARGUMENT;
	next = *gain;
	if (qam_gain_equalize(&next, received, &normalized) !=
		    QAM_GAIN_OK ||
	    v34_training_constellation_slice(
		    constellation, &normalized, &next_quarter_point,
		    &next_clockwise_quarter_turns) !=
		    V34_CONSTELLATION_OK ||
	    v34_training_constellation_point(
		    constellation, next_quarter_point,
		    next_clockwise_quarter_turns, &decision) !=
		    V34_CONSTELLATION_OK ||
	    qam_gain_track(&next, received, &decision) !=
		    QAM_GAIN_OK ||
	    !qam_gain_is_valid(&next))
		return V34_TRAINING_CHANNEL_INVALID_ARGUMENT;
	*gain = next;
	*quarter_point = next_quarter_point;
	*clockwise_quarter_turns = next_clockwise_quarter_turns;
	return V34_TRAINING_CHANNEL_OK;
}
