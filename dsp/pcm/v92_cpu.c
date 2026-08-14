// SPDX-License-Identifier: BSD-3-Clause

#include "v92_cpu.h"

static bool constellation_is_empty(
	const struct pcm_constellation *constellation)
{
	for (size_t byte = 0;
	     byte < PCM_CONSTELLATION_MASK_BYTES; ++byte) {
		if (constellation->ucode_mask[byte] != 0u)
			return false;
	}
	return true;
}

static bool constellations_are_canonical(
	const struct v92_cpu_parameters *parameters)
{
	uint8_t used = 0u;

	for (size_t interval = 0;
	     interval < V92_CPU_FRAME_INTERVALS; ++interval) {
		uint8_t index = parameters->frame_constellation[interval];

		if (index >= parameters->constellation_count)
			return false;
		used |= (uint8_t)(1u << index);
	}
	if (used != (uint8_t)((1u << parameters->constellation_count) - 1u))
		return false;
	for (size_t index = 0;
	     index < V92_CPU_MAX_CONSTELLATIONS; ++index) {
		bool active = index < parameters->constellation_count;
		bool transmit_empty = constellation_is_empty(
			&parameters->transmit_constellations[index]);
		bool codec_empty = constellation_is_empty(
			&parameters->codec_constellations[index]);

		if (active == transmit_empty)
			return false;
		if (parameters->codec_constellations_differ) {
			if (active == codec_empty)
				return false;
		} else if (!codec_empty) {
			return false;
		}
	}
	return true;
}

bool v92_cpu_parameters_are_valid(
	const struct v92_cpu_parameters *parameters)
{
	return parameters != NULL && g711_law_is_valid(parameters->law) &&
	       parameters->data_rate_number <=
		       V92_CPU_MAX_DATA_RATE_NUMBER &&
	       parameters->sign_redundancy <= 3u &&
	       parameters->spectral_lookahead_frames <= 3u &&
	       parameters->constellation_count >= 1u &&
	       parameters->constellation_count <=
		       V92_CPU_MAX_CONSTELLATIONS &&
	       constellations_are_canonical(parameters);
}

static bool bits_per_symbol_is_valid(unsigned int bits_per_symbol)
{
	return bits_per_symbol >= V92_CPU_MIN_BITS_PER_SYMBOL &&
	       bits_per_symbol <= V92_CPU_MAX_BITS_PER_SYMBOL;
}

size_t v92_cpu_frame_bits(
	const struct v92_cpu_parameters *parameters,
	unsigned int bits_per_symbol)
{
	size_t constellation_sets;
	size_t unfilled;
	size_t multiple;

	if (!v92_cpu_parameters_are_valid(parameters) ||
	    !bits_per_symbol_is_valid(bits_per_symbol))
		return 0u;
	constellation_sets = parameters->codec_constellations_differ ? 2u : 1u;
	unfilled = V92_CPU_HEADER_BITS +
		   constellation_sets * parameters->constellation_count *
			   V92_CPU_CONSTELLATION_BITS +
		   V92_CPU_MIN_TRAILER_BITS;
	multiple = (size_t)bits_per_symbol * V92_CPU_SYMBOLS_PER_FRAME;
	return (unfilled + multiple - 1u) / multiple * multiple;
}

size_t v92_cpu_frame_bytes(
	const struct v92_cpu_parameters *parameters,
	unsigned int bits_per_symbol)
{
	size_t bits = v92_cpu_frame_bits(parameters, bits_per_symbol);

	return bits == 0u ? 0u : (bits + 7u) / 8u;
}
