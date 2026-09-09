// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_RU_RX_H
#define V92_RU_RX_H

#include <stdbool.h>
#include <stdint.h>

enum {
	V92_RU_RX_MIN_SYMBOLS = 240,
	V92_RU_RX_FRAME_SYMBOLS = 12,
	V92_RU_RX_BAR_SYMBOLS = 24,
};

enum v92_ru_rx_result {
	V92_RU_RX_BUFFERING,
	V92_RU_RX_DETECTED,
	V92_RU_RX_BAR_STARTED,
	V92_RU_RX_COMPLETE,
	V92_RU_RX_MISMATCH,
	V92_RU_RX_INVALID_ARGUMENT,
	V92_RU_RX_INVALID_STATE,
};

/* V.92 8.5.5 and 9.8: Ru bypasses the data precoder/prefilter. Supply
 * equalized, polarity-corrected 8000-symbol/s levels at the trained LU,
 * NOT data-decoder decisions. Frame phase is the next symbol's position
 * in the established 12-symbol data frame; never reacquire it from Ru.
 * The caller owns the channel filter/delay and the subsequent TRN2u epoch. */
struct v92_ru_rx {
	int16_t lu;
	uint16_t run;
	uint8_t phase;
	uint8_t bar_symbols;
};

bool v92_ru_rx_init(struct v92_ru_rx *receiver, int16_t lu,
		    unsigned frame_phase);
bool v92_ru_rx_is_valid(const struct v92_ru_rx *receiver);

/* Require 240 consecutive levels within LU/4 of the expected +/-LU.
 * This is a conservative acquisition tolerance, not a normative limit.
 * Report the first reversed symbol only at a data-frame boundary, and
 * COMPLETE on the 24th reversed symbol. Once complete, consume nothing:
 * the next level belongs to TRN2u. MISMATCH consumes its level and resets
 * acquisition without moving the established frame alignment. Invalid
 * arguments/state leave the receiver unchanged. */
enum v92_ru_rx_result v92_ru_rx_sample(struct v92_ru_rx *receiver,
				     double equalized_level);

#endif /* V92_RU_RX_H */
