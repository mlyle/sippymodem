// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE3_SIGNS_H
#define V90_PHASE3_SIGNS_H

#include "v34_framing.h"
#include "v90_pcm_symbol.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V90_PHASE3_TRN1D_MIN_SYMBOLS = 2040,
	V90_PHASE3_TRN1D_ALIGNMENT = 6,
	/* V.90 permits Jd after at least 2040T and requires it within four
	 * seconds.  This longer head-end interval leaves a remote-silent window
	 * for delayed echo acquisition while remaining inside that deadline. */
	V90_PHASE3_TRN1D_SYMBOLS = 15996,
	V90_PHASE3_TRN1D_MAX_SYMBOLS = 31998,
	V90_PHASE3_JD_PRIME_BITS = 12,
};

enum v90_phase3_sign_phase {
	V90_PHASE3_SIGNS_TRN1D,
	V90_PHASE3_SIGNS_JD,
};

enum v90_phase3_sign_result {
	V90_PHASE3_SIGNS_OK,
	V90_PHASE3_SIGNS_INVALID_ARGUMENT,
	V90_PHASE3_SIGNS_INVALID_STATE,
};

/* Owns the digital modem's GPC scrambler and differential sign continuity
 * from TRN1d through every Jd repetition and the terminating Jd-prime. */
struct v90_phase3_signs {
	struct v34_scrambler scrambler;
	enum g711_law law;
	enum v90_phase3_sign_phase phase;
	uint32_t trn1d_symbols;
	uint8_t training_ucode;
	uint8_t last_sign;
};

enum v90_phase3_sign_result v90_phase3_signs_init(
	struct v90_phase3_signs *signs, enum g711_law law,
	uint8_t training_ucode);
bool v90_phase3_signs_is_valid(
	const struct v90_phase3_signs *signs);

/* TRN1d applies binary ones to a zero-initialized GPC scrambler. */
enum v90_phase3_sign_result v90_phase3_signs_next_trn1d(
	struct v90_phase3_signs *signs,
	struct v90_pcm_symbol *symbol);

/* The transition is permitted only after at least 2040 symbols and on a
 * six-symbol boundary.  It preserves both scrambler and final TRN1d sign. */
enum v90_phase3_sign_result v90_phase3_signs_begin_jd(
	struct v90_phase3_signs *signs);

/* Use record bits for Jd and twelve zero bits for Jd-prime. */
enum v90_phase3_sign_result v90_phase3_signs_next_jd_bit(
	struct v90_phase3_signs *signs, uint8_t bit,
	struct v90_pcm_symbol *symbol);

const char *v90_phase3_sign_result_name(
	enum v90_phase3_sign_result result);

#endif
