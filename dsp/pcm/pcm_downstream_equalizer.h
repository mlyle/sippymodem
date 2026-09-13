// SPDX-License-Identifier: BSD-3-Clause

#ifndef PCM_DOWNSTREAM_EQUALIZER_H
#define PCM_DOWNSTREAM_EQUALIZER_H

#include "g711.h"
#include "qam_equalizer.h"

/* The sampled downstream channel outlives its control/data coding epochs.
 * Zero initialization bypasses it until qualified Ri provides a level. */
struct pcm_downstream_equalizer {
	struct qam_equalizer filter;
	enum g711_law law;
	int16_t training_level;
};

bool pcm_downstream_equalizer_init(struct pcm_downstream_equalizer *equalizer,
	enum g711_law law, int16_t training_level);
bool pcm_downstream_equalizer_is_valid(const struct pcm_downstream_equalizer *equalizer);

/* Adapt only on already-qualified Ri/Ri-bar samples. Preserve the received
 * polarity; protocol framing owns its interpretation. Rejected calls do not
 * change the instance. */
bool pcm_downstream_equalizer_train(struct pcm_downstream_equalizer *equalizer,
	uint8_t octet);

/* Hold the learned taps and advance exactly once per input symbol, including
 * online control. Recover a G.711 decision without adding a symbol delay.
 * The caller retains the original physical sample for retrain detection. */
bool pcm_downstream_equalizer_receive(struct pcm_downstream_equalizer *equalizer,
	uint8_t octet, uint8_t *decision);

#endif
