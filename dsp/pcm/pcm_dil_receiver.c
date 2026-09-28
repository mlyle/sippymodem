// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_dil_receiver.h"

static void add_code(struct pcm_constellation *set, uint8_t code)
{
	set->ucode_mask[code / 8u] |= (uint8_t)(1u << (code % 8u));
}

bool pcm_dil_receiver_init(struct pcm_dil_receiver *receiver,
	const struct v90_dil_parameters *parameters, enum g711_law law)
{
	struct pcm_dil_receiver next = { 0 };

	if (receiver == NULL ||
	    !v90_dil_training_symbol_count(parameters, &next.limit) ||
	    next.limit == 0 ||
	    v90_dil_sequence_init(&next.expected, parameters, law) != V90_DIL_SEQUENCE_OK)
		return false;
	*receiver = next;
	return true;
}

bool pcm_dil_receiver_receive(struct pcm_dil_receiver *receiver, uint8_t octet)
{
	struct v90_pcm_symbol expected, received;
	uint16_t segment_symbol;

	if (receiver == NULL || receiver->symbols >= receiver->limit ||
	    !v90_pcm_symbol_parse(&received, receiver->expected.law, octet))
		return false;
	segment_symbol = receiver->expected.segment_symbol;
	if (v90_dil_sequence_next(&receiver->expected, &expected) != V90_DIL_SEQUENCE_OK)
		return false;
	unsigned int interval = receiver->symbols++ % 6u;
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
		add_code(&receiver->rejected, expected.ucode);
	/* Signed correlation rejects reversed or absent signals without an
	 * absolute-value detector accidentally accepting the wrong DIL phase.
	 * The descriptor bound is 195840 symbols; these sums fit in 48 bits. */
	int64_t cross = (int64_t)expected.sample * received.sample;
	receiver->reference_energy += (uint64_t)((int64_t)expected.sample * expected.sample);
	if (cross > 0)
		receiver->received_cross += (uint64_t)cross;
	return true;
}

bool pcm_dil_receiver_finish(const struct pcm_dil_receiver *receiver,
	struct pcm_dil_result *result)
{
	struct pcm_dil_result next = { 0 };

	if (receiver == NULL || result == NULL || receiver->symbols == 0 ||
	    receiver->symbols > receiver->limit || receiver->reference_energy == 0)
		return false;
	next.symbols = receiver->symbols;
	uint64_t gain = (receiver->received_cross * 8192u + receiver->reference_energy / 2u) /
		receiver->reference_energy;
	next.gain_q3_13 = (uint16_t)(gain > UINT16_MAX ? UINT16_MAX : gain);
	for (uint8_t code = 1; code < PCM_CONSTELLATION_UCODES; ++code) {
		bool qualified = !pcm_constellation_contains(&receiver->rejected, code);

		for (unsigned int slot = 0; slot < 12u; ++slot)
			qualified = qualified && receiver->observations[code][slot] == 2u;
		if (qualified)
			add_code(&next.usable, code);
	}
	next.rejected_levels = pcm_constellation_count(&receiver->rejected);
	next.measured = true;
	*result = next;
	return true;
}

bool pcm_dil_select_training(const struct pcm_dil_result *result,
	struct pcm_constellation *constellation)
{
	struct pcm_constellation selected = { 0 };
	unsigned int ceiling = PCM_CONSTELLATION_UCODES;

	if (result == NULL || constellation == NULL || !result->measured)
		return false;
	for (unsigned int code = PCM_CONSTELLATION_UCODES; code-- > 0;) {
		if (!pcm_constellation_contains(constellation, (uint8_t)code))
			continue;
		if (ceiling > code + 1u)
			ceiling = code + 1u;
		while (ceiling != 0) {
			--ceiling;
			if (pcm_constellation_contains(&result->usable, (uint8_t)ceiling))
				break;
		}
		if (ceiling == 0)
			return false;
		add_code(&selected, (uint8_t)ceiling);
	}
	if (pcm_constellation_is_empty(&selected))
		return false;
	*constellation = selected;
	return true;
}
