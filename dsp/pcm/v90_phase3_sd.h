// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE3_SD_H
#define V90_PHASE3_SD_H

#include "v90_pcm_symbol.h"

#include <stddef.h>
#include <stdint.h>

enum {
	V90_PHASE3_SD_SYMBOLS = 384,
	V90_PHASE3_SD_BAR_SYMBOLS = 48,
};

enum v90_phase3_sd_kind {
	V90_PHASE3_SD,
	V90_PHASE3_SD_BAR,
};

enum v90_phase3_sd_result {
	V90_PHASE3_SD_OK,
	V90_PHASE3_SD_COMPLETE,
	V90_PHASE3_SD_INVALID_ARGUMENT,
	V90_PHASE3_SD_INVALID_STATE,
};

struct v90_phase3_sd {
	enum g711_law law;
	enum v90_phase3_sd_kind kind;
	uint8_t training_ucode;
	uint16_t emitted_symbols;
	uint16_t total_symbols;
};

enum v90_phase3_sd_result v90_phase3_sd_init(
	struct v90_phase3_sd *sequence, enum g711_law law,
	uint8_t training_ucode, enum v90_phase3_sd_kind kind);

/* Generates one 8000-symbol/s network character.  A call after the last
 * symbol returns COMPLETE and leaves both the sequence and output untouched. */
enum v90_phase3_sd_result v90_phase3_sd_next(
	struct v90_phase3_sd *sequence, struct v90_pcm_symbol *symbol);

size_t v90_phase3_sd_remaining(
	const struct v90_phase3_sd *sequence);
const char *v90_phase3_sd_result_name(
	enum v90_phase3_sd_result result);

#endif
