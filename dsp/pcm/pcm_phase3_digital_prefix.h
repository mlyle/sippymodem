// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_PHASE3_DIGITAL_PREFIX_H
#define PCM_PHASE3_DIGITAL_PREFIX_H

#include "v90_phase3_sd.h"
#include "v90_phase3_signs.h"

#include <stdbool.h>
#include <stdint.h>

enum pcm_phase3_digital_prefix_stage {
	PCM_PHASE3_DIGITAL_PREFIX_SD,
	PCM_PHASE3_DIGITAL_PREFIX_SD_BAR,
	PCM_PHASE3_DIGITAL_PREFIX_TRN1D,
	PCM_PHASE3_DIGITAL_PREFIX_COMPLETE,
};

enum pcm_phase3_digital_prefix_result {
	PCM_PHASE3_DIGITAL_PREFIX_OK,
	PCM_PHASE3_DIGITAL_PREFIX_DONE,
	PCM_PHASE3_DIGITAL_PREFIX_INVALID_ARGUMENT,
	PCM_PHASE3_DIGITAL_PREFIX_INVALID_STATE,
};

union pcm_phase3_digital_prefix_sequence {
	struct v90_phase3_sd sd;
	struct v90_phase3_signs signs;
};

/* Own the V.90-defined digital Phase 3 prefix shared by V.90 and V.92:
 * Sd, Sd-bar, and a bounded aligned TRN1d. The successor owns the record
 * format and continues the GPC scrambler and final TRN1d sign. */
struct pcm_phase3_digital_prefix {
	union pcm_phase3_digital_prefix_sequence sequence;
	uint32_t trn1d_target_symbols;
	enum pcm_phase3_digital_prefix_stage stage;
};

enum pcm_phase3_digital_prefix_result pcm_phase3_digital_prefix_init(
	struct pcm_phase3_digital_prefix *prefix, enum g711_law law,
	uint8_t training_ucode, uint32_t trn1d_target_symbols);
bool pcm_phase3_digital_prefix_duration_is_valid(uint32_t target);
bool pcm_phase3_digital_prefix_is_valid(
	const struct pcm_phase3_digital_prefix *prefix);
bool pcm_phase3_digital_prefix_is_complete(
	const struct pcm_phase3_digital_prefix *prefix);

/* Emit one 8000-symbol/s network character. The call that emits the final
 * TRN1d character also moves the prefix to COMPLETE. DONE consumes nothing
 * and leaves the destination unchanged. */
enum pcm_phase3_digital_prefix_result pcm_phase3_digital_prefix_next(
	struct pcm_phase3_digital_prefix *prefix,
	struct v90_pcm_symbol *symbol);

/* Copy the live TRN1d coding epoch. The caller then begins its protocol-
 * specific differentially encoded record. Failed calls leave signs alone. */
enum pcm_phase3_digital_prefix_result pcm_phase3_digital_prefix_handoff(
	const struct pcm_phase3_digital_prefix *prefix,
	struct v90_phase3_signs *signs);

const char *pcm_phase3_digital_prefix_result_name(
	enum pcm_phase3_digital_prefix_result result);

#endif /* PCM_PHASE3_DIGITAL_PREFIX_H */
