// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_dil_receiver.h"

#include <math.h>

static void add_code(struct pcm_constellation *set, uint8_t code)
{
	set->ucode_mask[code / 8u] |= (uint8_t)(1u << (code % 8u));
}

bool pcm_dil_receiver_init(struct pcm_dil_receiver *receiver,
	const struct v90_dil_parameters *parameters, enum g711_law law,
	uint8_t training_ucode)
{
	struct pcm_dil_receiver next = { .training_ucode = training_ucode };
	int16_t level;

	if (receiver == NULL ||
	    !v90_dil_training_symbol_count(parameters, &next.limit) ||
	    next.limit == 0 ||
	    !g711_ucode_sample(law, training_ucode, true, &level) || level <= 0 ||
	    v90_dil_sequence_init(&next.expected, parameters, law) != V90_DIL_SEQUENCE_OK)
		return false;
	for (unsigned interval = 0; interval < PCM_CONSTELLATION_FRAME_INTERVALS; ++interval)
		next.lsb_models[interval] = 7u;
	*receiver = next;
	return true;
}

static uint8_t substitute_lsb(uint8_t octet, unsigned model)
{
	if (model == 0)
		return octet;
	return model == 1 ? (uint8_t)(octet & ~1u) : (uint8_t)(octet | 1u);
}

bool pcm_dil_receiver_receive(struct pcm_dil_receiver *receiver,
	uint8_t octet, uint8_t unconditioned_octet)
{
	struct v90_pcm_symbol expected, received;
	uint16_t segment_symbol;

	if (receiver == NULL || receiver->symbols >= receiver->limit ||
	    !v90_pcm_symbol_parse(&received, receiver->expected.law, octet))
		return false;
	segment_symbol = receiver->expected.segment_symbol;
	if (v90_dil_sequence_next(&receiver->expected, &expected) != V90_DIL_SEQUENCE_OK)
		return false;
	unsigned int interval = receiver->symbols++ % PCM_CONSTELLATION_FRAME_INTERVALS;
	/* Leave the start of each segment for filter settling. This also keeps
	 * the transition from the preceding binary signal out of the fit. */
	if (segment_symbol < 18u)
		return true;
	unsigned int slot = 2u * interval + (expected.sample > 0);
	uint8_t *count = &receiver->observations[expected.ucode][slot];
	if (*count < 2u)
		++*count;
	if (expected.ucode != received.ucode ||
	    (expected.sample > 0) != (received.sample > 0))
		add_code(&receiver->rejected[interval], expected.ucode);
	/* Fit substitutions from the unconditioned wire evidence. Equalizer
	 * corrections may move a received cell without a digital substitution. */
	if (expected.ucode != 0)
		for (unsigned model = 0; model < 3u; ++model)
			if (substitute_lsb(expected.octet, model) != unconditioned_octet)
				receiver->lsb_models[interval] &= (uint8_t)~(1u << model);
	/* Signed correlation rejects reversed or absent signals without an
	 * absolute-value detector accidentally accepting the wrong DIL phase.
	 * The descriptor bound is 195840 symbols; these sums fit in 48 bits. */
	int64_t cross = (int64_t)expected.sample * received.sample;
	receiver->reference_energy += (uint64_t)((int64_t)expected.sample * expected.sample);
	if (cross > 0)
		receiver->received_cross += (uint64_t)cross;
	return true;
}

/* V.90 Table 14 / V.92 Table 23 specify transmitter / codec D/A TRN1d RMS.
 * Evaluate the fitted substitutions at UINFO, rather than treating equalizer
 * or host gain as digital attenuation. Ambiguous models must agree at UINFO. */
static bool training_ratio(const struct pcm_dil_receiver *receiver, uint16_t *ratio)
{
	struct v90_pcm_symbol nominal;
	double energy = 0;

	if (!v90_pcm_symbol_make(&nominal, receiver->expected.law, receiver->training_ucode, true))
		return false;
	for (unsigned interval = 0; interval < PCM_CONSTELLATION_FRAME_INTERVALS; ++interval) {
		unsigned observed = 0;
		/* Require repeated evidence spanning an upper chord's worth of
		 * levels, rather than extrapolating from a short binary prefix. */
		for (unsigned code = 64u; code < PCM_CONSTELLATION_UCODES; ++code)
			observed += receiver->observations[code][2u * interval] == 2u &&
				receiver->observations[code][2u * interval + 1u] == 2u;

		if (observed < 16u)
			return false;
		int32_t level = -1;
		for (unsigned model = 0; model < 3u; ++model) {
			if ((receiver->lsb_models[interval] & (1u << model)) == 0)
				continue;
			int16_t candidate = g711_decode(receiver->expected.law, substitute_lsb(nominal.octet, model));
			if (candidate <= 0 || (level >= 0 && level != candidate))
				return false;
			level = candidate;
		}
		if (level < 0)
			return false;
		energy += (double)level * level;
	}
	double encoded = 8192.0 * nominal.sample * sqrt(PCM_CONSTELLATION_FRAME_INTERVALS / energy);
	if (!isfinite(encoded) || encoded < 0.5 || encoded > UINT16_MAX + 0.499)
		return false;
	*ratio = (uint16_t)lround(encoded);
	return true;
}

bool pcm_dil_receiver_finish(const struct pcm_dil_receiver *receiver,
	struct pcm_dil_result *result)
{
	struct pcm_dil_result next = { .trn1d_ratio_q3_13 = 8192u };
	struct pcm_constellation rejected = { 0 }, common;

	if (receiver == NULL || result == NULL || receiver->symbols == 0 ||
	    receiver->symbols > receiver->limit || receiver->reference_energy == 0)
		return false;
	next.symbols = receiver->symbols;
	uint64_t gain = (receiver->received_cross * 8192u + receiver->reference_energy / 2u) /
		receiver->reference_energy;
	next.gain_q3_13 = (uint16_t)(gain > UINT16_MAX ? UINT16_MAX : gain);
	next.interval_model = training_ratio(receiver, &next.trn1d_ratio_q3_13);
	for (uint8_t code = 1; code < PCM_CONSTELLATION_UCODES; ++code) {
		struct v90_pcm_symbol symbol;
		if (!v90_pcm_symbol_make(&symbol, receiver->expected.law, code, true))
			return false;
		for (unsigned interval = 0; interval < PCM_CONSTELLATION_FRAME_INTERVALS; ++interval) {
			bool changed = pcm_constellation_contains(&receiver->rejected[interval], code);
			bool observed = receiver->observations[code][2u * interval] == 2u &&
				receiver->observations[code][2u * interval + 1u] == 2u;
			/* Equalization may hide a substituted cell. The advertised
			 * identical transmit/codec planes also require the fitted
			 * digital model to preserve the actual D/A level. */
			if (next.interval_model && observed)
				for (unsigned model = 0; model < 3u; ++model)
					if ((receiver->lsb_models[interval] & (1u << model)) != 0 &&
					    substitute_lsb(symbol.octet, model) != symbol.octet)
						changed = true;
			if (changed)
				add_code(&rejected, code);
			if (!changed && observed)
				add_code(&next.usable[interval], code);
		}
	}
	if (!next.interval_model) {
		common = next.usable[0];
		for (unsigned interval = 1; interval < PCM_CONSTELLATION_FRAME_INTERVALS; ++interval)
			for (unsigned byte = 0; byte < sizeof(common.ucode_mask); ++byte)
				common.ucode_mask[byte] &= next.usable[interval].ucode_mask[byte];
		for (unsigned interval = 0; interval < PCM_CONSTELLATION_FRAME_INTERVALS; ++interval)
			next.usable[interval] = common;
	}
	next.rejected_levels = pcm_constellation_count(&rejected);
	next.measured = true;
	*result = next;
	return true;
}
