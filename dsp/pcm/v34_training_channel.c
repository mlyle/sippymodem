// SPDX-License-Identifier: BSD-3-Clause

#include "v34_training_channel.h"

enum sippymodem_v34_training_channel_result
sippymodem_v34_training_channel_decide(
	struct sippymodem_qam_gain *gain,
	enum sippymodem_v34_constellation_size constellation,
	const struct sippymodem_qam_point *received, uint8_t *quarter_point,
	uint8_t *clockwise_quarter_turns)
{
	struct sippymodem_qam_gain next;
	struct sippymodem_qam_point normalized;
	struct sippymodem_qam_point decision;
	uint8_t next_quarter_point;
	uint8_t next_clockwise_quarter_turns;

	if (!sippymodem_qam_gain_is_valid(gain) || quarter_point == NULL ||
	    clockwise_quarter_turns == NULL ||
	    quarter_point == clockwise_quarter_turns)
		return SIPPYMODEM_V34_TRAINING_CHANNEL_INVALID_ARGUMENT;
	next = *gain;
	if (sippymodem_qam_gain_equalize(&next, received, &normalized) !=
		    SIPPYMODEM_QAM_GAIN_OK ||
	    sippymodem_v34_training_constellation_slice(
		    constellation, &normalized, &next_quarter_point,
		    &next_clockwise_quarter_turns) !=
		    SIPPYMODEM_V34_CONSTELLATION_OK ||
	    sippymodem_v34_training_constellation_point(
		    constellation, next_quarter_point,
		    next_clockwise_quarter_turns, &decision) !=
		    SIPPYMODEM_V34_CONSTELLATION_OK ||
	    sippymodem_qam_gain_track(&next, received, &decision) !=
		    SIPPYMODEM_QAM_GAIN_OK ||
	    !sippymodem_qam_gain_is_valid(&next))
		return SIPPYMODEM_V34_TRAINING_CHANNEL_INVALID_ARGUMENT;
	*gain = next;
	*quarter_point = next_quarter_point;
	*clockwise_quarter_turns = next_clockwise_quarter_turns;
	return SIPPYMODEM_V34_TRAINING_CHANNEL_OK;
}

const char *sippymodem_v34_training_channel_result_name(
	enum sippymodem_v34_training_channel_result result)
{
	switch (result) {
	case SIPPYMODEM_V34_TRAINING_CHANNEL_OK:
		return "ok";
	case SIPPYMODEM_V34_TRAINING_CHANNEL_INVALID_ARGUMENT:
		return "invalid argument";
	}
	return "unknown";
}
