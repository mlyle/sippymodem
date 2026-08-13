// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_PCM_PHASE3_OPENING_H
#define V92_PCM_PHASE3_OPENING_H

#include "v92_pcm_info1a.h"
#include "v92_pcm_phase3_ru.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	/* The nominal value within the required 70 +/- 5 ms interval. */
	V92_PCM_PHASE3_OPENING_SILENCE_SYMBOLS = 560,
	V92_PCM_PHASE3_MD_UNIT_SYMBOLS = 276,
};

enum v92_pcm_phase3_opening_stage {
	V92_PCM_PHASE3_OPENING_SILENCE,
	V92_PCM_PHASE3_OPENING_RU,
	V92_PCM_PHASE3_OPENING_RU_BAR,
	V92_PCM_PHASE3_OPENING_MD,
	V92_PCM_PHASE3_OPENING_SECOND_RU,
	V92_PCM_PHASE3_OPENING_SECOND_RU_BAR,
	V92_PCM_PHASE3_OPENING_TRN1U_READY,
};

enum v92_pcm_phase3_opening_result {
	V92_PCM_PHASE3_OPENING_OK,
	V92_PCM_PHASE3_OPENING_NEED_MD_SYMBOL,
	V92_PCM_PHASE3_OPENING_COMPLETE,
	V92_PCM_PHASE3_OPENING_INVALID_ARGUMENT,
	V92_PCM_PHASE3_OPENING_INVALID_STATE,
};

union v92_pcm_phase3_opening_state {
	struct v92_pcm_phase3_ru ru;
	uint32_t emitted_symbols;
};

/* Own the standardized analogue-modem opening through the TRN1u boundary.
 * Ru output is a raw network-code sequence and bypasses the PCM-upstream
 * precoder and prefilter. MD is deliberately not synthesized: it is
 * manufacturer-defined, so its exact G.711 symbols remain caller-owned.
 * lu_ucode selects the local upstream magnitude independently of UINFO,
 * which belongs to the digital modem's downstream training signal. */
struct v92_pcm_phase3_opening {
	union v92_pcm_phase3_opening_state state;
	enum v92_pcm_phase3_opening_stage stage;
	enum g711_law law;
	uint8_t lu_ucode;
	uint8_t md_length_276_symbol_units;
};

enum v92_pcm_phase3_opening_result v92_pcm_phase3_opening_init(
	struct v92_pcm_phase3_opening *opening, enum g711_law law,
	uint8_t lu_ucode, const struct v92_pcm_info1a *info1a);
bool v92_pcm_phase3_opening_is_valid(
	const struct v92_pcm_phase3_opening *opening);
bool v92_pcm_phase3_opening_is_complete(
	const struct v92_pcm_phase3_opening *opening);

/* Pass NULL for md_symbol outside MD. During MD, a NULL md_symbol returns
 * NEED_MD_SYMBOL without changing either output; a supplied symbol is copied
 * exactly and consumes one symbol of the negotiated interval. */
enum v92_pcm_phase3_opening_result v92_pcm_phase3_opening_next(
	struct v92_pcm_phase3_opening *opening,
	const struct v90_pcm_symbol *md_symbol,
	struct v90_pcm_symbol *symbol);
size_t v92_pcm_phase3_opening_remaining(
	const struct v92_pcm_phase3_opening *opening);

const char *v92_pcm_phase3_opening_stage_name(
	enum v92_pcm_phase3_opening_stage stage);
const char *v92_pcm_phase3_opening_result_name(
	enum v92_pcm_phase3_opening_result result);

#endif /* V92_PCM_PHASE3_OPENING_H */
