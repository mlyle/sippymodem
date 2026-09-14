// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_QC_SIGNAL_H
#define V92_QC_SIGNAL_H

#include "v90_pcm_symbol.h"

enum {
	V92_QTS_SYMBOLS = 128 * 6,
	V92_QTS_BAR_SYMBOLS = 8 * 6,
	V92_QTS_TOTAL_SYMBOLS = V92_QTS_SYMBOLS + V92_QTS_BAR_SYMBOLS,
	V92_ANSPCM_PERIOD = 301,
	V92_ANSPCM_REVERSAL_PERIOD = 12 * V92_ANSPCM_PERIOD,
};

/* The caller retains data-frame alignment from QTS symbol zero onward.
 * index is Table 2 UQTS, 0..14; index 15 requests cleardown, not QTS. */
bool v92_qts_symbol(enum g711_law law, uint8_t index, unsigned position,
	struct v90_pcm_symbol *symbol);

struct v92_anspcm {
	/* Generated once per instance; copied state has no borrowed storage. */
	uint8_t period[V92_ANSPCM_PERIOD];
	enum g711_law law;
	uint16_t position;
};

/* level is Table 11's LM field: 0..3 selects -9.5, -12, -15, -18 dBm0.
 * The generated period exactly reproduces Tables 7..10 without checked-in
 * waveform constants. Sampling begins at k=0 and remains bounded through
 * arbitrarily many phase reversals. Invalid calls change neither output
 * nor state. These are signal generators, not Quick Connect policy. */
bool v92_anspcm_init(struct v92_anspcm *signal, enum g711_law law,
	uint8_t level);
bool v92_anspcm_sample(struct v92_anspcm *signal,
	struct v90_pcm_symbol *symbol);

#endif
