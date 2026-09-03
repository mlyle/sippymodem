// SPDX-License-Identifier: BSD-3-Clause

#ifndef DC_REMOVER_H
#define DC_REMOVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum dc_remover_result {
	DC_REMOVER_OK,
	DC_REMOVER_INVALID_ARGUMENT,
	DC_REMOVER_INVALID_STATE,
};

enum dc_remover_stage {
	DC_REMOVER_WARMUP,
	DC_REMOVER_CALIBRATION,
	DC_REMOVER_TRACKING,
};

enum {
	/* Fixed 8-kHz front-end windows: 720 ms, 1.2 s, and 2.4 s. */
	DC_REMOVER_WARMUP_SAMPLES = 0x1680,
	DC_REMOVER_CALIBRATION_SAMPLES = 0x2580,
	DC_REMOVER_TRACKING_SAMPLES = 0x4b00,
};

/*
 * Instance-owned DC estimate for conventional modem receive samples.  PCM
 * constellation owners must bypass this front end because their exact sample
 * levels are part of the modulation.
 */
struct dc_remover {
	int64_t sum;
	uint32_t sample_count;
	int16_t level;
	enum dc_remover_stage stage;
};

enum dc_remover_result dc_remover_init(struct dc_remover *remover);
bool dc_remover_is_valid(const struct dc_remover *remover);

/* Process samples in place. */
enum dc_remover_result dc_remover_process(
	struct dc_remover *remover, int16_t *samples, size_t count);

#endif
