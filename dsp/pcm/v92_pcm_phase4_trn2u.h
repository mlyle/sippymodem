// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_PCM_PHASE4_TRN2U_H
#define V92_PCM_PHASE4_TRN2U_H

#include "v34_framing.h"
#include "v92_j.h"

#include <stdbool.h>
#include <stdint.h>

enum {
	V92_PCM_PHASE4_TRN2U_INITIAL_MIN_SYMBOLS = 12000,
	V92_PCM_PHASE4_TRN2U_ALIGNMENT = 12,
};

enum v92_pcm_phase4_trn2u_result {
	V92_PCM_PHASE4_TRN2U_OK,
	V92_PCM_PHASE4_TRN2U_INVALID_ARGUMENT,
	V92_PCM_PHASE4_TRN2U_INVALID_STATE,
};

/* Generate the initial Phase 4 TRN2u epoch. The GPA scrambler starts from
 * zero, while differential sign encoding continues from the final E1u sign.
 * Rate-renegotiation epochs have different initialization and are intentionally
 * outside this owner. */
struct v92_pcm_phase4_trn2u {
	struct v34_scrambler scrambler;
	enum v92_constellation_size constellation;
	uint32_t emitted_symbols;
	int16_t lu;
	uint8_t previous_encoded_sign;
};

unsigned int v92_pcm_phase4_trn2u_bits_per_symbol(
	enum v92_constellation_size constellation);

/* Generate one of Tables 28 and 29's normalized signed levels. The formula
 * uses sqrt((M*M - 1)/3), where M is four or eight, instead of a level table.
 * LU must keep every level in the selected constellation within int16_t.
 * Rejected calls leave sample unchanged. */
enum v92_pcm_phase4_trn2u_result v92_pcm_phase4_trn2u_level(
	enum v92_constellation_size constellation, int16_t lu,
	uint8_t magnitude_index, uint8_t encoded_sign, int16_t *sample);

enum v92_pcm_phase4_trn2u_result v92_pcm_phase4_trn2u_init(
	struct v92_pcm_phase4_trn2u *trn2u,
	enum v92_constellation_size constellation,
	uint8_t previous_encoded_sign, int16_t lu);
bool v92_pcm_phase4_trn2u_is_valid(
	const struct v92_pcm_phase4_trn2u *trn2u);
bool v92_pcm_phase4_trn2u_can_handoff(
	const struct v92_pcm_phase4_trn2u *trn2u);

/* Consume scrambled binary ones in transmission order. Rejected calls leave
 * both the generator and sample unchanged. */
enum v92_pcm_phase4_trn2u_result v92_pcm_phase4_trn2u_next(
	struct v92_pcm_phase4_trn2u *trn2u, int16_t *sample);

const char *v92_pcm_phase4_trn2u_result_name(
	enum v92_pcm_phase4_trn2u_result result);

#endif /* V92_PCM_PHASE4_TRN2U_H */
