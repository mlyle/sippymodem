// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_DATA_QUALITY_H
#define V92_DATA_QUALITY_H

#include <stdbool.h>
#include <stdint.h>

enum {
	/* Receiver policy in signed PCM sample units, not a normative threshold:
	 * equalized filter output, or wire PCM for a channel-compensated CPd with
	 * identified post-codec host gain (otherwise unscaled raw input).
	 * Initial B1u and online recovery use the same ceiling. */
	V92_DATA_MAX_RMS_ERROR = 64,
	V92_DATA_QUALITY_WINDOW_FRAMES = 128,
	V92_DATA_QUALITY_BAD_WINDOWS = 3,
	V92_DATA_QUALITY_MAX_RETRAINS = 2,
	/* 64 clean 192 ms windows separate recovered incidents. This is local
	 * receiver policy, not a V.92 timer or permission to deliver weak data. */
	V92_DATA_QUALITY_REARM_WINDOWS = 64,
};

enum v92_data_quality_result {
	V92_DATA_QUALITY_OK,
	V92_DATA_QUALITY_RETRAIN,
	V92_DATA_QUALITY_EXHAUSTED,
	V92_DATA_QUALITY_INVALID_ARGUMENT,
};

/* PCM-upstream quality policy, separate from the frame decoder. A zero
 * initializer begins a new call. Each observation covers one twelve-symbol
 * frame before equalizer adaptation. Three consecutive bad 192 ms windows
 * request full retraining. A window is bad on excess residual or low-margin
 * sequence disagreement; a single uncertain window cannot force recovery.
 * A decoder-reported ambiguous payload or loss of all decoder paths requests
 * recovery immediately. Fixed mappings also use this budget for lost paths
 * during B1u; automatic initial selection instead lowers its rate ceiling.
 * Unusable TRN2u measurements and unrepresentable peer B1u selections
 * share the same retry budget.
 * None of these conditions licenses delivering ambiguous bits to the host. */
struct v92_data_quality {
	double squared_error;
	uint16_t frames;
	uint8_t bad_windows;
	uint8_t automatic_retrains;
	bool uncertain_window;
	uint8_t good_windows;
};

bool v92_data_quality_is_valid(const struct v92_data_quality *quality);
/* A new CONNECT discards measurements and healthy credit, not used attempts. */
bool v92_data_quality_restart(struct v92_data_quality *quality);
/* Consume an immediate recovery attempt for unusable training or decoding.
 * Exhausted or invalid state is unchanged; measurements do not carry over. */
enum v92_data_quality_result v92_data_quality_retrain(struct v92_data_quality *quality);
/* RETRAIN consumes one of two attempts until sustained clean reception rearms
 * recovery. Bad or uncertain windows break healthy credit. EXHAUSTED is
 * terminal for the host adapter. Invalid inputs change nothing. */
enum v92_data_quality_result v92_data_quality_observe(
	struct v92_data_quality *quality, double squared_error, bool ambiguous,
	bool uncertain);

#endif /* V92_DATA_QUALITY_H */
