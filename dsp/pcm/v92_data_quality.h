// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_DATA_QUALITY_H
#define V92_DATA_QUALITY_H

#include <stdbool.h>
#include <stdint.h>

enum {
	/* Receiver policy in equalizer-coordinate sample units, not a normative
	 * V.92 threshold. Initial B1u and online recovery use the same ceiling. */
	V92_DATA_MAX_RMS_ERROR = 64,
	V92_DATA_QUALITY_WINDOW_FRAMES = 128,
	V92_DATA_QUALITY_BAD_WINDOWS = 3,
	V92_DATA_QUALITY_MAX_RETRAINS = 2,
};

enum v92_data_quality_result {
	V92_DATA_QUALITY_OK,
	V92_DATA_QUALITY_RETRAIN,
	V92_DATA_QUALITY_EXHAUSTED,
	V92_DATA_QUALITY_INVALID_ARGUMENT,
};

/* Online PCM-upstream policy, separate from the frame decoder. A zero
 * initializer begins a new call. Each observation covers one twelve-symbol
 * frame before equalizer adaptation. Three consecutive bad 192 ms windows
 * request full retraining; an ambiguous payload requests it immediately.
 * Neither condition licenses delivering ambiguous bits to the host. */
struct v92_data_quality {
	double squared_error;
	uint16_t frames;
	uint8_t bad_windows;
	uint8_t automatic_retrains;
};

bool v92_data_quality_is_valid(const struct v92_data_quality *quality);
/* A new CONNECT discards the old measurement window, never the call budget. */
bool v92_data_quality_restart(struct v92_data_quality *quality);
/* RETRAIN consumes one of two per-call attempts and clears the measurement.
 * EXHAUSTED is terminal for the host adapter. Invalid inputs change nothing. */
enum v92_data_quality_result v92_data_quality_observe(
	struct v92_data_quality *quality, double squared_error, bool ambiguous);

#endif /* V92_DATA_QUALITY_H */
