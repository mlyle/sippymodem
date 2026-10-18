// SPDX-License-Identifier: BSD-3-Clause

#include "data_quality.h"

#include <math.h>
#include <stddef.h>

bool v32_data_quality_start(struct v32_data_quality *quality, unsigned bit_rate)
{
	unsigned snr_db;
	unsigned power = 5120;

	if (quality == NULL)
		return false;
	switch (bit_rate) {
	case 4800: snr_db = 9; break;
	case 7200: snr_db = 11; break;
	case 9600: snr_db = 13; break;
	case 12000: snr_db = 20; power = 21504; break;
	case 14400: snr_db = 24; power = 21504; break;
	default: return false;
	}
	/* The established rate policy estimates SNR as 10*log10((P-E)/E),
	 * where E is one quarter of the equalizer's decision-error metric.
	 * Solve for E once per training, rather than taking logs in the hot path.
	 * Half a dB covers integer SNR rounding; allow another 5% error headroom.
	 * These are recovery limits, not a measured BER or training rate selector. */
	uint16_t threshold = (uint16_t)ceil(
		4.0 * power * 1.05 / (1.0 + pow(10.0, (snr_db - 0.5) / 10.0)));
	*quality = (struct v32_data_quality){
		.threshold = threshold,
		.warmup_windows = V32_QUALITY_WARMUP_WINDOWS,
		.recovery.attempts = quality->recovery.attempts,
	};
	return true;
}

bool v32_data_quality_observe(struct v32_data_quality *quality,
	int16_t decision_error, unsigned symbols)
{
	if (quality == NULL || quality->threshold == 0 || decision_error < 0)
		return false;
	while (symbols != 0) {
		unsigned count = V32_QUALITY_WINDOW_SYMBOLS - quality->window_symbols;
		if (count > symbols)
			count = symbols;
		quality->error_sum += (uint32_t)decision_error * count;
		quality->window_symbols += count;
		symbols -= count;
		if (quality->window_symbols != V32_QUALITY_WINDOW_SYMBOLS)
			continue;
		bool bad = quality->error_sum >=
			(uint32_t)quality->threshold * V32_QUALITY_WINDOW_SYMBOLS;
		quality->error_sum = 0;
		quality->window_symbols = 0;
		if (quality->warmup_windows != 0) {
			--quality->warmup_windows;
			continue;
		}
		recovery_window(&quality->recovery, bad, !bad,
			V32_QUALITY_BAD_WINDOWS, V32_QUALITY_REARM_WINDOWS);
		if (quality->recovery.bad_windows == V32_QUALITY_BAD_WINDOWS &&
		    recovery_take(&quality->recovery, V32_QUALITY_MAX_RETRAINS))
			return true;
	}
	return false;
}
