// SPDX-License-Identifier: BSD-3-Clause

#include "pcm_rate.h"

#include <stddef.h>

enum {
	PCM_MAPPING_FRAME_INTERVALS = 6,
};

static uint32_t frame_bit_rate(uint32_t frame_bits, uint32_t symbols)
{
	return (frame_bits * 8000u + symbols / 2u) / symbols;
}

uint32_t pcm_downstream_bit_rate(uint8_t data_rate_number)
{
	uint32_t frame_bits;

	if (data_rate_number < PCM_DOWNSTREAM_MIN_DATA_RATE_NUMBER ||
	    data_rate_number > PCM_DOWNSTREAM_MAX_DATA_RATE_NUMBER)
		return 0;
	frame_bits = PCM_DOWNSTREAM_BASE_FRAME_BITS + data_rate_number;
	return frame_bit_rate(frame_bits, PCM_MAPPING_FRAME_INTERVALS);
}

uint32_t pcm_upstream_bit_rate(uint8_t data_rate_number)
{
	return frame_bit_rate(pcm_upstream_frame_bits(data_rate_number),
			      PCM_UPSTREAM_FRAME_SYMBOLS);
}

uint8_t pcm_upstream_data_rate_number(uint32_t bit_rate)
{
	for (uint8_t number = PCM_UPSTREAM_MIN_DATA_RATE_NUMBER;
	     number <= PCM_UPSTREAM_MAX_DATA_RATE_NUMBER; ++number) {
		if (pcm_upstream_bit_rate(number) == bit_rate)
			return number;
	}
	return 0;
}

bool pcm_downstream_rates_select(
	struct pcm_downstream_rate_selection *selection,
	unsigned int minimum_rate, unsigned int maximum_rate)
{
	struct pcm_downstream_rate_selection selected = { 0 };

	if (selection == NULL || minimum_rate > maximum_rate)
		return false;
	for (uint8_t number = PCM_DOWNSTREAM_MIN_DATA_RATE_NUMBER;
	     number <= PCM_DOWNSTREAM_MAX_DATA_RATE_NUMBER; ++number) {
		uint32_t bit_rate = pcm_downstream_bit_rate(number);

		if (bit_rate < minimum_rate || bit_rate > maximum_rate)
			continue;
		selected.enabled_mask |= UINT32_C(1) << (number - 1u);
		selected.selected_data_rate_number = number;
		selected.selected_bit_rate = bit_rate;
	}
	if (selected.enabled_mask == 0)
		return false;
	*selection = selected;
	return true;
}
