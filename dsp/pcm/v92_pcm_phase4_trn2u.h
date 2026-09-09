// SPDX-License-Identifier: BSD-3-Clause

#ifndef V92_PCM_PHASE4_TRN2U_H
#define V92_PCM_PHASE4_TRN2U_H

#include "v92_phase4_modulator.h"

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

/* Generate a fresh Phase 4 TRN2u epoch. The GPA scrambler starts from zero.
 * Initial training continues the final E1u differential sign; the first
 * rate-renegotiation TRN2u uses zero instead (V.92 Amendment 1, 8.7.6).
 * The procedure owner chooses the training duration. */
struct v92_pcm_phase4_trn2u {
	struct v92_phase4_modulator modulator;
	uint32_t emitted_symbols;
};

enum v92_pcm_phase4_trn2u_result v92_pcm_phase4_trn2u_init(
	struct v92_pcm_phase4_trn2u *trn2u,
	enum v92_constellation_size constellation,
	uint8_t previous_encoded_sign, int16_t lu);
bool v92_pcm_phase4_trn2u_is_valid(
	const struct v92_pcm_phase4_trn2u *trn2u);
/* Initial TRN2u may end at a 12-symbol boundary after either 12000 symbols
 * or receipt of a complete SUVd. At least one aligned group is required so a
 * caller cannot manufacture a zero-length training epoch. */
bool v92_pcm_phase4_trn2u_can_handoff(
	const struct v92_pcm_phase4_trn2u *trn2u, bool received_suvd);

/* Consume scrambled binary ones in transmission order. Rejected calls leave
 * both the generator and sample unchanged. */
enum v92_pcm_phase4_trn2u_result v92_pcm_phase4_trn2u_next(
	struct v92_pcm_phase4_trn2u *trn2u, int16_t *sample);

const char *v92_pcm_phase4_trn2u_result_name(
	enum v92_pcm_phase4_trn2u_result result);

#endif /* V92_PCM_PHASE4_TRN2U_H */
