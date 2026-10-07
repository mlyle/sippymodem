// SPDX-License-Identifier: BSD-3-Clause

#ifndef V90_PHASE3_SIGNS_H
#define V90_PHASE3_SIGNS_H

#include "v34_framing.h"
#include "v90_info1.h"
#include "v90_jd.h"
#include "v90_pcm_symbol.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V90_PHASE3_TRN1D_MIN_SYMBOLS = 2040,
	V90_PHASE3_TRN1D_ALIGNMENT = 6,
	/* V.90 and V.92 permit Jd after at least 2040T and require it within
	 * four seconds. This longer head-end interval leaves a remote-silent window
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

/* Independent transmit/receive snapshots at the downstream Jd-prime boundary.
 * The scrambler field holds the endpoint's polynomial history in either role. */
struct v90_phase3_downstream_handoff {
	struct v34_scrambler scrambler;
	struct v90_jd_parameters jd;
	enum g711_law law;
	uint32_t trn1d_symbols;
	uint8_t training_ucode;
	uint8_t last_sign;
};

static inline bool v90_phase3_downstream_handoff_is_valid(
	const struct v90_phase3_downstream_handoff *handoff)
{
	return handoff != NULL &&
	       v34_scrambler_is_valid(&handoff->scrambler) &&
	       handoff->scrambler.polynomial == V34_POLYNOMIAL_GPC &&
	       v90_jd_parameters_are_valid(&handoff->jd) &&
	       g711_law_is_valid(handoff->law) &&
	       handoff->trn1d_symbols >= V90_PHASE3_TRN1D_MIN_SYMBOLS &&
	       handoff->trn1d_symbols % V90_PHASE3_TRN1D_ALIGNMENT == 0 &&
	       handoff->training_ucode >= V90_INFO1A_MIN_TRAINING_UCODE &&
	       handoff->training_ucode <= V90_INFO1A_MAX_TRAINING_UCODE &&
	       handoff->last_sign <= 1;
}

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
