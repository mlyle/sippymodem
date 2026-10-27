// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_DIL_RECEIVER_H
#define PCM_DIL_RECEIVER_H

#include "pcm_constellation.h"
#include "v90_dil_sequence.h"

/* Qualify levels in each six-symbol frame interval after choosing one line
 * polarity from signed DIL correlation.
 * A fitted constant-LSB substitution model supplies the codec-side TRN1d
 * ratio. Without that model, retain the common-level selection and configured
 * unity ratio; general digital pads and codec-law conversion remain unsupported. */
struct pcm_dil_result {
	struct pcm_constellation usable[PCM_CONSTELLATION_FRAME_INTERVALS];
	uint32_t symbols;
	uint16_t gain_q3_13;
	uint16_t trn1d_ratio_q3_13;
	uint8_t rejected_levels;
	bool interval_model;
	bool measured;
	bool polarity_inverted;
	/* Usable levels belong to the unconditioned stream; switch atomically. */
	bool use_unconditioned;
};

struct pcm_dil_receiver {
	struct v90_dil_sequence expected;
	/* Two independent observations of each sign in each interval. Counters
	 * saturate at two; memory and arithmetic are bounded for any descriptor. */
	uint8_t observations[PCM_CONSTELLATION_UCODES][12];
	/* Keep both line polarities until signed correlation chooses one. */
	struct pcm_constellation rejected[2][PCM_CONSTELLATION_FRAME_INTERVALS];
	/* Candidate bits: identity, force wire LSB zero, force wire LSB one. */
	uint8_t lsb_models[PCM_CONSTELLATION_FRAME_INTERVALS];
	uint8_t training_ucode;
	uint64_t reference_energy;
	int64_t received_cross, raw_received_cross;
	uint8_t raw_polarities;
	uint32_t symbols;
	uint32_t limit;
};

bool pcm_dil_receiver_init(struct pcm_dil_receiver *receiver,
	const struct v90_dil_parameters *parameters, enum g711_law law,
	uint8_t training_ucode);
/* Call once per symbol after Jd-prime/Jp-prime. The conditioned octet
 * qualifies usable levels; unconditioned evidence identifies substitutions. */
bool pcm_dil_receiver_receive(struct pcm_dil_receiver *receiver,
	uint8_t octet, uint8_t unconditioned_octet);
/* A deadline may end a traversal early. Unobserved levels remain unavailable. */
bool pcm_dil_receiver_finish(const struct pcm_dil_receiver *receiver,
	struct pcm_dil_result *result);

/* Propose the unconditioned codec model only after repeated signed evidence
 * across many levels identifies a fixed per-phase LSB transform. Its polarity
 * must agree with the conditioned epoch. The owner must switch its conditioner
 * with these masks before further reception; B1 remains independent validation.
 * Unsupported or insufficient evidence leaves result unchanged. */
bool pcm_dil_receiver_finish_unconditioned(const struct pcm_dil_receiver *receiver,
	struct pcm_dil_result *result);

#endif
