// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_downstream_equalizer.h"

#include "memory_region.h"

#include <limits.h>
#include <math.h>

bool pcm_downstream_equalizer_is_valid(const struct pcm_downstream_equalizer *equalizer)
{
	return equalizer != NULL && (equalizer->training_level == 0 ||
		(equalizer->training_level > 0 && g711_law_is_valid(equalizer->law) &&
		 equalizer->filter.tap_count == 3 && equalizer->filter.decision_delay == 0 &&
		 !equalizer->filter.carrier_tracking && qam_equalizer_is_valid(&equalizer->filter)));
}

bool pcm_downstream_equalizer_init(struct pcm_downstream_equalizer *equalizer,
	enum g711_law law, int16_t training_level)
{
	struct pcm_downstream_equalizer next = { .law = law, .training_level = training_level };
	const struct qam_point unity = { 1.0, 0.0 };

	if (equalizer == NULL || !g711_law_is_valid(law) || training_level <= 0 ||
	    qam_equalizer_init_geometry(&next.filter, &unity, &unity, 3, 0) != QAM_EQUALIZER_OK)
		return false;
	/* Ri's six-symbol binary period identifies three real taps. A causal
	 * span avoids changing its established Ri-bar/first-TRN2d boundary. */
	*equalizer = next;
	return true;
}

static int16_t decode(enum g711_law law, uint8_t octet)
{
	return law == G711_LAW_PCMU ? g711_decode_pcmu(octet) : g711_decode_pcma(octet);
}

bool pcm_downstream_equalizer_train(struct pcm_downstream_equalizer *equalizer,
	uint8_t octet)
{
	struct pcm_downstream_equalizer next;
	struct qam_point output;
	bool ready;

	if (!pcm_downstream_equalizer_is_valid(equalizer) || !equalizer->training_level)
		return false;
	int16_t sample = decode(equalizer->law, octet);
	if (sample == 0)
		return false;
	next = *equalizer;
	struct qam_point input = { sample / 32768.0, 0.0 };
	struct qam_point target = {
		(sample > 0 ? next.training_level : -next.training_level) / 32768.0, 0.0
	};
	if (qam_equalizer_push(&next.filter, &input, &output, &ready) != QAM_EQUALIZER_OK || !ready ||
	    qam_equalizer_adapt(&next.filter, &target, 0.1) != QAM_EQUALIZER_OK)
		return false;
	*equalizer = next;
	return true;
}

bool pcm_downstream_equalizer_receive(struct pcm_downstream_equalizer *equalizer,
	uint8_t octet, uint8_t *decision)
{
	struct qam_point output;
	bool ready;

	if (!pcm_downstream_equalizer_is_valid(equalizer) || decision == NULL ||
	    memory_regions_overlap(equalizer, sizeof(*equalizer), decision, sizeof(*decision)))
		return false;
	if (!equalizer->training_level) {
		*decision = octet;
		return true;
	}
	int16_t sample = decode(equalizer->law, octet);
	struct qam_point input = { sample / 32768.0, 0.0 };
	if (qam_equalizer_push(&equalizer->filter, &input, &output, &ready) != QAM_EQUALIZER_OK || !ready)
		return false;
	double linear = output.in_phase * 32768.0;
	/* Preserve exact input codewords, including signed PCM zero, on an
	 * unchanged channel. Otherwise quantize only the equalized sample. */
	if (linear == sample) {
		*decision = octet;
		return true;
	}
	if (linear > INT16_MAX)
		linear = INT16_MAX;
	if (linear < INT16_MIN)
		linear = INT16_MIN;
	sample = (int16_t)lround(linear);
	*decision = equalizer->law == G711_LAW_PCMU ? g711_encode_pcmu(sample) : g711_encode_pcma(sample);
	return true;
}
