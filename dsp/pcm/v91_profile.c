// SPDX-License-Identifier: BSD-3-Clause
#include "v91_profile.h"
#include "v90_pcm_symbol.h"
#include <string.h>

bool v91_profile_dil(struct v90_dil_parameters *parameters)
{
	struct v90_dil_parameters next = {
		.segment_count = 128,
		.sign_pattern_bits = 48,
		.training_pattern_bits = 12,
		.training_pattern = { 0xff, 0x0f },
		.sign_pattern = { 0, 0, 0, 0xff, 0xff, 0xff },
	};

	if (parameters == NULL)
		return false;
	memset(next.segment_length_codes, 7, sizeof(next.segment_length_codes));
	for (unsigned i = 0; i < 128; ++i)
		next.training_ucodes[i] = i & 1u ? i / 2u : 127u - i / 2u;
	if (!v90_dil_parameters_are_valid(&next))
		return false;
	*parameters = next;
	return true;
}

bool v91_profile_learn(const struct pcm_octet_probe *probe, enum g711_law law,
	const struct v91_profile_limits *limits, struct v90_pcm_mapping *mapping,
	struct pcm_octet_channel *channel)
{
	struct pcm_constellation sets[6];
	struct pcm_octet_channel learned;
	struct v90_pcm_mapping next;

	if (mapping == NULL || channel == NULL || limits == NULL || limits->maximum_power == 0 ||
	    limits->maximum_frame_bits < 21 || limits->maximum_frame_bits > 48 ||
	    (limits->receiver_power && !g711_law_is_valid(limits->receive_law)) ||
	    !pcm_octet_probe_finish(probe, law, sets, &learned))
		return false;
	if (limits->receiver_power) {
		/* A received octet belongs to at most one retained transmit code.
		 * Peak bounding remains safe for either sign and all observed robbed
		 * bit outcomes, without assuming their probabilities during data. */
		for (unsigned i = 0; i < 6; ++i)
			for (unsigned octet = 0; octet < 256; ++octet) {
				if (!(learned.known[i][octet / 8] & (1u << (octet % 8))))
					continue;
				int32_t sample = g711_decode(limits->receive_law, octet);
				if ((uint32_t)(sample * sample) <= limits->maximum_power)
					continue;
				struct v90_pcm_symbol symbol;
				if (!v90_pcm_symbol_parse(&symbol, law, learned.decoded[i][octet]))
					return false;
				sets[i].ucode_mask[symbol.ucode / 8] &= ~(1u << (symbol.ucode % 8));
			}
	}
	for (;;) {
		uint64_t capacity = 1;
		unsigned frame_bits = 6;
		uint32_t floor, ceiling;

		for (unsigned i = 0; i < 6; ++i)
			capacity *= pcm_constellation_count(&sets[i]);
		while (capacity > 1) {
			capacity >>= 1;
			++frame_bits;
		}
		if (frame_bits > limits->maximum_frame_bits)
			frame_bits = limits->maximum_frame_bits;
		if (frame_bits < 21 || v90_pcm_mapping_init_v91(&next, law,
			frame_bits, sets) != V90_PCM_MAPPING_OK ||
		    v90_pcm_mapping_power(&next, V90_PCM_MAPPING_TRANSMIT,
			law, &floor, &ceiling) != V90_PCM_MAPPING_OK)
			return false;
		if (limits->receiver_power || ceiling <= limits->maximum_power) {
			*mapping = next;
			*channel = learned;
			return true;
		}
		/* Keep the low-power prefix in every interval. Cardinalities may
		 * differ after impairments; do not round them to one common size. */
		unsigned highest = 0;
		for (unsigned i = 0; i < 6; ++i) {
			uint8_t code;
			if (!pcm_constellation_descending_select(&sets[i], 0, &code))
				return false;
			if (code > highest)
				highest = code;
		}
		for (unsigned i = 0; i < 6; ++i)
			sets[i].ucode_mask[highest / 8] &= ~(1u << (highest % 8));
	}
}
