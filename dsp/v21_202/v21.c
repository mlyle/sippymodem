// SPDX-License-Identifier: BSD-3-Clause

#include "v21.h"

enum {
	V21_CHANNEL_1_MARK_HZ = 980,
	V21_CHANNEL_1_SPACE_HZ = 1180,
	V21_CHANNEL_2_MARK_HZ = 1650,
	V21_CHANNEL_2_SPACE_HZ = 1850,
	V21_METRIC_SMOOTHING_SHIFT = 2,
	V21_DETECTOR_WINDOW =
		(SIPPYMODEM_V21_SAMPLE_RATE + SIPPYMODEM_V21_BIT_RATE / 2) /
		SIPPYMODEM_V21_BIT_RATE,
};

const struct sippymodem_fsk_channel sippymodem_v21_channel_1 = {
	.sample_rate = SIPPYMODEM_V21_SAMPLE_RATE,
	.symbol_rate_numerator = SIPPYMODEM_V21_BIT_RATE,
	.symbol_rate_denominator = 1,
	.mark_frequency_hz = V21_CHANNEL_1_MARK_HZ,
	.space_frequency_hz = V21_CHANNEL_1_SPACE_HZ,
	.detector_window = V21_DETECTOR_WINDOW,
	.metric_smoothing_shift = V21_METRIC_SMOOTHING_SHIFT,
};

const struct sippymodem_fsk_channel sippymodem_v21_channel_2 = {
	.sample_rate = SIPPYMODEM_V21_SAMPLE_RATE,
	.symbol_rate_numerator = SIPPYMODEM_V21_BIT_RATE,
	.symbol_rate_denominator = 1,
	.mark_frequency_hz = V21_CHANNEL_2_MARK_HZ,
	.space_frequency_hz = V21_CHANNEL_2_SPACE_HZ,
	.detector_window = V21_DETECTOR_WINDOW,
	.metric_smoothing_shift = V21_METRIC_SMOOTHING_SHIFT,
};
