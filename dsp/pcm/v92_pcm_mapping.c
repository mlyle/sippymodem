// SPDX-License-Identifier: BSD-3-Clause

#include "v92_pcm_mapping.h"

#include "mixed_radix.h"

#include <string.h>

static const struct v92_cpd_constellation *symbol_constellation(
	const struct v92_pcm_mapping *mapping, size_t symbol)
{
	size_t phase = symbol % V92_PCM_MAPPING_PHASES;
	uint8_t index = mapping->cpd.frame_constellation[phase];

	return &mapping->cpd.constellation[index];
}

static bool symbol_geometry_is_valid(
	const struct v92_pcm_mapping *mapping, size_t symbol)
{
	const struct v92_cpd_constellation *constellation =
		symbol_constellation(mapping, symbol);
	unsigned int limit = constellation->count;
	uint8_t modulus = mapping->cpd.modulus[symbol];

	if (symbol % 4u != 3u)
		limit *= 2u;
	return modulus != 0u && modulus <= limit;
}

static bool capacity_is_valid(const struct v92_pcm_mapping *mapping)
{
	__uint128_t capacity = 1u;
	__uint128_t required =
		(__uint128_t)1u << mapping->data_bits_per_frame;

	for (size_t symbol = 0u;
	     symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
		if (__builtin_mul_overflow(
			    capacity,
			    (__uint128_t)mapping->cpd.modulus[symbol],
			    &capacity))
			return false;
	}
	return capacity >= required;
}

bool v92_pcm_mapping_is_valid(const struct v92_pcm_mapping *mapping)
{
	if (mapping == NULL || !g711_law_is_valid(mapping->law) ||
	    !v92_cpd_parameters_are_valid(&mapping->cpd) ||
	    !mapping->cpd.modulus_present ||
	    !mapping->cpd.filters_present ||
	    !mapping->cpd.constellations_present ||
	    mapping->data_bits_per_frame !=
		    pcm_upstream_frame_bits(mapping->cpd.data_rate_number) ||
	    mapping->data_bits_per_frame < V92_PCM_MAPPING_MIN_BITS ||
	    mapping->data_bits_per_frame > V92_PCM_MAPPING_MAX_BITS)
		return false;
	for (size_t symbol = 0u;
	     symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
		if (!symbol_geometry_is_valid(mapping, symbol))
			return false;
	}
	return capacity_is_valid(mapping);
}

bool v92_pcm_mapping_equal(
	const struct v92_pcm_mapping *left,
	const struct v92_pcm_mapping *right)
{
	if (!v92_pcm_mapping_is_valid(left) ||
	    !v92_pcm_mapping_is_valid(right) ||
	    left->law != right->law ||
	    left->data_bits_per_frame != right->data_bits_per_frame ||
	    left->cpd.four_times_gain_q0_16 !=
		    right->cpd.four_times_gain_q0_16 ||
	    left->cpd.data_rate_number != right->cpd.data_rate_number ||
	    left->cpd.trellis != right->cpd.trellis ||
	    left->cpd.extend_e2u != right->cpd.extend_e2u ||
	    left->cpd.acknowledges_cpu != right->cpd.acknowledges_cpu ||
	    left->cpd.modulus_present != right->cpd.modulus_present ||
	    left->cpd.filters_present != right->cpd.filters_present ||
	    left->cpd.constellations_present !=
		    right->cpd.constellations_present ||
	    memcmp(left->cpd.modulus, right->cpd.modulus,
		   sizeof(left->cpd.modulus)) != 0 ||
	    memcmp(left->cpd.frame_constellation,
		   right->cpd.frame_constellation,
		   sizeof(left->cpd.frame_constellation)) != 0)
		return false;
	for (size_t section = 0u;
	     section < V92_CPD_FILTER_SECTION_COUNT; ++section) {
		if (left->cpd.filter[section].count !=
			    right->cpd.filter[section].count ||
		    memcmp(left->cpd.filter[section].coefficient,
			   right->cpd.filter[section].coefficient,
			   sizeof(left->cpd.filter[section].coefficient)) != 0)
			return false;
	}
	for (size_t index = 0u;
	     index < V92_CPD_MAX_CONSTELLATIONS; ++index) {
		if (left->cpd.constellation[index].count !=
			    right->cpd.constellation[index].count ||
		    memcmp(left->cpd.constellation[index].positive_linear,
			   right->cpd.constellation[index].positive_linear,
			   sizeof(left->cpd.constellation[index]
					  .positive_linear)) != 0)
			return false;
	}
	return true;
}

enum v92_pcm_mapping_result v92_pcm_mapping_init(
	struct v92_pcm_mapping *mapping, enum g711_law law,
	const struct v92_cpd_parameters *cpd)
{
	struct v92_pcm_mapping initialized;

	if (mapping == NULL || cpd == NULL)
		return V92_PCM_MAPPING_INVALID_ARGUMENT;
	if (!g711_law_is_valid(law) ||
	    !v92_cpd_parameters_are_valid(cpd) ||
	    cpd->data_rate_number == 0u ||
	    !cpd->modulus_present || !cpd->filters_present ||
	    !cpd->constellations_present)
		return V92_PCM_MAPPING_INVALID_PARAMETERS;
	memset(&initialized, 0, sizeof(initialized));
	initialized.cpd = *cpd;
	initialized.law = law;
	initialized.data_bits_per_frame = pcm_upstream_frame_bits(cpd->data_rate_number);
	if (!v92_pcm_mapping_is_valid(&initialized))
		return V92_PCM_MAPPING_INVALID_PARAMETERS;
	*mapping = initialized;
	return V92_PCM_MAPPING_OK;
}

static void radices(
	const struct v92_pcm_mapping *mapping,
	uint32_t output[V92_PCM_MAPPING_FRAME_SYMBOLS])
{
	for (size_t symbol = 0u;
	     symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol)
		output[symbol] = mapping->cpd.modulus[symbol];
}

static enum v92_pcm_mapping_result mixed_result(
	enum mixed_radix_result result)
{
	switch (result) {
	case MIXED_RADIX_OK:
		return V92_PCM_MAPPING_OK;
	case MIXED_RADIX_OUT_OF_RANGE:
		return V92_PCM_MAPPING_OUT_OF_RANGE;
	case MIXED_RADIX_INVALID_ARGUMENT:
		return V92_PCM_MAPPING_INVALID_ARGUMENT;
	case MIXED_RADIX_INVALID_CONFIGURATION:
		return V92_PCM_MAPPING_INVALID_PARAMETERS;
	}
	return V92_PCM_MAPPING_INVALID_PARAMETERS;
}

enum v92_pcm_mapping_result v92_pcm_mapping_encode(
	const struct v92_pcm_mapping *mapping,
	const uint8_t *bits, size_t bit_count,
	uint8_t *differential_state,
	uint8_t digits[V92_PCM_MAPPING_FRAME_SYMBOLS],
	uint8_t *frame_sign)
{
	if (!v92_pcm_mapping_is_valid(mapping))
		return V92_PCM_MAPPING_INVALID_ARGUMENT;
	return v92_pcm_mapping_encode_owned(
		mapping, bits, bit_count, differential_state, digits, frame_sign);
}

enum v92_pcm_mapping_result v92_pcm_mapping_encode_owned(
	const struct v92_pcm_mapping *mapping,
	const uint8_t *bits, size_t bit_count,
	uint8_t *differential_state,
	uint8_t digits[V92_PCM_MAPPING_FRAME_SYMBOLS],
	uint8_t *frame_sign)
{
	uint32_t bases[V92_PCM_MAPPING_FRAME_SYMBOLS];
	uint32_t encoded[V92_PCM_MAPPING_FRAME_SYMBOLS];
	uint8_t next_state;
	uint8_t sign;
	enum mixed_radix_result result;

	if (mapping == NULL || bits == NULL ||
	    differential_state == NULL || digits == NULL ||
	    frame_sign == NULL)
		return V92_PCM_MAPPING_INVALID_ARGUMENT;
	if (bit_count != mapping->data_bits_per_frame)
		return V92_PCM_MAPPING_OUT_OF_RANGE;
	radices(mapping, bases);
	next_state = *differential_state;
	result = mixed_radix_encode_differential(
		bases, V92_PCM_MAPPING_FRAME_SYMBOLS,
		bits, bit_count, &next_state, encoded, &sign);
	if (result != MIXED_RADIX_OK)
		return mixed_result(result);
	for (size_t symbol = 0u;
	     symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol)
		digits[symbol] = (uint8_t)encoded[symbol];
	*differential_state = next_state;
	*frame_sign = sign;
	return V92_PCM_MAPPING_OK;
}

enum v92_pcm_mapping_result v92_pcm_mapping_decode(
	const struct v92_pcm_mapping *mapping,
	const uint8_t digits[V92_PCM_MAPPING_FRAME_SYMBOLS],
	uint8_t *differential_state,
	uint8_t *bits, size_t bit_count)
{
	if (!v92_pcm_mapping_is_valid(mapping))
		return V92_PCM_MAPPING_INVALID_ARGUMENT;
	return v92_pcm_mapping_decode_owned(
		mapping, digits, differential_state, bits, bit_count);
}

enum v92_pcm_mapping_result v92_pcm_mapping_decode_owned(
	const struct v92_pcm_mapping *mapping,
	const uint8_t digits[V92_PCM_MAPPING_FRAME_SYMBOLS],
	uint8_t *differential_state,
	uint8_t *bits, size_t bit_count)
{
	uint32_t bases[V92_PCM_MAPPING_FRAME_SYMBOLS];
	uint32_t encoded[V92_PCM_MAPPING_FRAME_SYMBOLS];
	uint8_t next_state;
	enum mixed_radix_result result;

	if (mapping == NULL || digits == NULL ||
	    differential_state == NULL || bits == NULL)
		return V92_PCM_MAPPING_INVALID_ARGUMENT;
	if (bit_count != mapping->data_bits_per_frame)
		return V92_PCM_MAPPING_OUT_OF_RANGE;
	radices(mapping, bases);
	for (size_t symbol = 0u;
	     symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol)
		encoded[symbol] = digits[symbol];
	next_state = *differential_state;
	result = mixed_radix_decode_differential(
		bases, V92_PCM_MAPPING_FRAME_SYMBOLS, encoded,
		&next_state, bits, bit_count);
	if (result != MIXED_RADIX_OK)
		return mixed_result(result);
	*differential_state = next_state;
	return V92_PCM_MAPPING_OK;
}

enum v92_pcm_mapping_result v92_pcm_mapping_positive_level(
	const struct v92_pcm_mapping *mapping, size_t symbol,
	uint8_t ordinal, int16_t *level)
{
	const struct v92_cpd_constellation *constellation;

	if (!v92_pcm_mapping_is_valid(mapping) || level == NULL)
		return V92_PCM_MAPPING_INVALID_ARGUMENT;
	if (symbol >= V92_PCM_MAPPING_FRAME_SYMBOLS)
		return V92_PCM_MAPPING_OUT_OF_RANGE;
	constellation = symbol_constellation(mapping, symbol);
	if (ordinal >= constellation->count)
		return V92_PCM_MAPPING_OUT_OF_RANGE;
	*level = constellation->positive_linear[ordinal];
	return V92_PCM_MAPPING_OK;
}

const char *v92_pcm_mapping_result_name(
	enum v92_pcm_mapping_result result)
{
	switch (result) {
	case V92_PCM_MAPPING_OK:
		return "ok";
	case V92_PCM_MAPPING_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_PCM_MAPPING_INVALID_PARAMETERS:
		return "invalid parameters";
	case V92_PCM_MAPPING_OUT_OF_RANGE:
		return "out of range";
	}
	return "unknown";
}
