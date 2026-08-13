// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_PCM_PHASE3_RU_H
#define V92_PCM_PHASE3_RU_H

#include "v90_pcm_symbol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	V92_PCM_PHASE3_RU_PATTERN_SYMBOLS = 6,
	V92_PCM_PHASE3_RU_SYMBOLS = 384,
	V92_PCM_PHASE3_RU_BAR_SYMBOLS = 24,
};

enum v92_pcm_phase3_ru_kind {
	V92_PCM_PHASE3_RU,
	V92_PCM_PHASE3_RU_BAR,
};

enum v92_pcm_phase3_ru_result {
	V92_PCM_PHASE3_RU_OK,
	V92_PCM_PHASE3_RU_COMPLETE,
	V92_PCM_PHASE3_RU_INVALID_ARGUMENT,
	V92_PCM_PHASE3_RU_INVALID_STATE,
};

/* One bounded 8000-symbol/s Ru or Ru-bar span. lu_ucode is the local upstream
 * magnitude, not INFO1a's downstream UINFO. The caller sends these raw symbols
 * without the upstream precoder or prefilter. */
struct v92_pcm_phase3_ru {
	enum g711_law law;
	enum v92_pcm_phase3_ru_kind kind;
	uint16_t emitted_symbols;
	uint8_t lu_ucode;
};

enum v92_pcm_phase3_ru_result v92_pcm_phase3_ru_init(
	struct v92_pcm_phase3_ru *sequence, enum g711_law law,
	uint8_t lu_ucode, enum v92_pcm_phase3_ru_kind kind);
bool v92_pcm_phase3_ru_is_valid(
	const struct v92_pcm_phase3_ru *sequence);

/* COMPLETE leaves both sequence and symbol unchanged. */
enum v92_pcm_phase3_ru_result v92_pcm_phase3_ru_next(
	struct v92_pcm_phase3_ru *sequence,
	struct v90_pcm_symbol *symbol);
size_t v92_pcm_phase3_ru_remaining(
	const struct v92_pcm_phase3_ru *sequence);

const char *v92_pcm_phase3_ru_result_name(
	enum v92_pcm_phase3_ru_result result);

#endif /* V92_PCM_PHASE3_RU_H */
