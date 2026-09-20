// SPDX-License-Identifier: BSD-3-Clause

#include "v92_ru_rx.h"

#include <math.h>
#include <stddef.h>

bool v92_ru_rx_is_valid(const struct v92_ru_rx *receiver)
{
	return receiver != NULL && receiver->lu > 0 &&
	       isfinite(receiver->mean_level) && receiver->mean_level >= 0 &&
	       receiver->run <= V92_RU_RX_MIN_SYMBOLS &&
	       receiver->phase < V92_RU_RX_FRAME_SYMBOLS &&
	       receiver->bar_symbols <= V92_RU_RX_BAR_SYMBOLS &&
	       (receiver->bar_symbols == 0 ||
		(receiver->run == V92_RU_RX_MIN_SYMBOLS &&
		 receiver->phase == receiver->bar_symbols %
			V92_RU_RX_FRAME_SYMBOLS));
}

bool v92_ru_rx_init(struct v92_ru_rx *receiver, int16_t lu,
		    unsigned frame_phase)
{
	if (receiver == NULL || lu <= 0 ||
	    frame_phase >= V92_RU_RX_FRAME_SYMBOLS)
		return false;
	*receiver = (struct v92_ru_rx){
		.lu = lu,
		.phase = (uint8_t)frame_phase,
	};
	return true;
}

enum v92_ru_rx_result v92_ru_rx_sample(struct v92_ru_rx *receiver,
				     double equalized_level)
{
	enum v92_ru_rx_result result = V92_RU_RX_BUFFERING;
	double level;
	bool ru, bar;

	if (receiver == NULL || !isfinite(equalized_level))
		return V92_RU_RX_INVALID_ARGUMENT;
	if (!v92_ru_rx_is_valid(receiver))
		return V92_RU_RX_INVALID_STATE;
	if (receiver->bar_symbols == V92_RU_RX_BAR_SYMBOLS)
		return V92_RU_RX_COMPLETE;
	level = receiver->phase % 6u < 3u ?
		equalized_level : -equalized_level;
	/* Residual ISI can ripple individual Ru levels while preserving its
	 * sign sequence and matched amplitude. Smooth over its six-symbol
	 * period, but never smooth over a polarity error or move frame phase. */
	if (level <= 0)
		receiver->mean_level = 0;
	else if (receiver->mean_level == 0)
		receiver->mean_level = level;
	else
		receiver->mean_level += (level - receiver->mean_level) / 6.0;
	ru = level > 0 && fabs(receiver->mean_level - receiver->lu) <= receiver->lu * 0.25;
	bar = fabs(level + receiver->lu) <= receiver->lu * 0.25;
	if (receiver->bar_symbols != 0) {
		if (bar) {
			receiver->bar_symbols++;
			if (receiver->bar_symbols == V92_RU_RX_BAR_SYMBOLS)
				result = V92_RU_RX_COMPLETE;
		} else {
			result = V92_RU_RX_MISMATCH;
		}
	} else if (receiver->run == V92_RU_RX_MIN_SYMBOLS) {
		if (bar && receiver->phase == 0) {
			receiver->bar_symbols = 1;
			result = V92_RU_RX_BAR_STARTED;
		} else if (!ru) {
			result = V92_RU_RX_MISMATCH;
		}
	} else if (ru) {
		if (++receiver->run == V92_RU_RX_MIN_SYMBOLS)
			result = V92_RU_RX_DETECTED;
	} else {
		receiver->run = 0;
	}
	if (result == V92_RU_RX_MISMATCH) {
		receiver->run = ru ? 1 : 0;
		receiver->bar_symbols = 0;
	}
	receiver->phase = (receiver->phase + 1u) % V92_RU_RX_FRAME_SYMBOLS;
	return result;
}
