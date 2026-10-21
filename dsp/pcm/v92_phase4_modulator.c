// SPDX-License-Identifier: BSD-3-Clause

#include "v92_phase4_modulator.h"

#include "memory_region.h"
#include "sample_math.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

unsigned int v92_phase4_modulator_bits_per_symbol(
	enum v92_constellation_size constellation)
{
	switch (constellation) {
	case V92_CONSTELLATION_4_POINT:
		return 2;
	case V92_CONSTELLATION_8_POINT:
		return 3;
	}
	return 0;
}

static unsigned int magnitude_count(
	enum v92_constellation_size constellation)
{
	unsigned int bits =
		v92_phase4_modulator_bits_per_symbol(constellation);

	return bits == 0 ? 0 : 1u << (bits - 1u);
}

static inline bool raw_level(
	enum v92_constellation_size constellation, int16_t lu,
	uint8_t magnitude_index, uint8_t encoded_sign, int16_t *sample)
{
	double mean_square;
	double scaled;
	int16_t rounded;

	if (sample == NULL || lu <= 0 || encoded_sign > 1u ||
	    magnitude_index >= magnitude_count(constellation))
		return false;
	mean_square = ((double)constellation * (double)constellation - 1.0) /
		3.0;
	scaled = (double)(2u * magnitude_index + 1u) * (double)lu /
		sqrt(mean_square);
	if (!sample_round_checked_s16(scaled, &rounded) || rounded <= 0)
		return false;
	*sample = encoded_sign != 0u ?
		(int16_t)-rounded : (int16_t)rounded;
	return true;
}

static bool parameters_are_valid(
	enum v92_constellation_size constellation, int16_t lu)
{
	unsigned int count = magnitude_count(constellation);
	int16_t outer_level;

	return count != 0 && raw_level(
		constellation, lu, (uint8_t)(count - 1u), 0u,
		&outer_level);
}

enum v92_phase4_modulator_result v92_phase4_modulator_level(
	enum v92_constellation_size constellation, int16_t lu,
	uint8_t magnitude_index, uint8_t encoded_sign, int16_t *sample)
{
	int16_t generated;

	if (sample == NULL || !parameters_are_valid(constellation, lu) ||
	    !raw_level(
		    constellation, lu, magnitude_index, encoded_sign,
		    &generated))
		return V92_PHASE4_MODULATOR_INVALID_ARGUMENT;
	*sample = generated;
	return V92_PHASE4_MODULATOR_OK;
}

bool v92_phase4_modulator_is_valid(
	const struct v92_phase4_modulator *modulator)
{
	return modulator != NULL &&
	       parameters_are_valid(modulator->constellation, modulator->lu) &&
	       modulator->previous_encoded_sign <= 1u &&
	       v34_scrambler_is_valid(&modulator->scrambler) &&
	       modulator->scrambler.polynomial == V34_POLYNOMIAL_GPA;
}

enum v92_phase4_modulator_result v92_phase4_modulator_init(
	struct v92_phase4_modulator *modulator,
	enum v92_constellation_size constellation,
	uint8_t previous_encoded_sign, int16_t lu)
{
	struct v92_phase4_modulator initialized = {
		.constellation = constellation,
		.lu = lu,
		.previous_encoded_sign = previous_encoded_sign,
	};

	if (modulator == NULL || previous_encoded_sign > 1u ||
	    !parameters_are_valid(constellation, lu) ||
	    v34_scrambler_init(
		    &initialized.scrambler, V34_POLYNOMIAL_GPA) !=
		    V34_FRAMING_OK ||
	    !v92_phase4_modulator_is_valid(&initialized))
		return V92_PHASE4_MODULATOR_INVALID_ARGUMENT;
	*modulator = initialized;
	return V92_PHASE4_MODULATOR_OK;
}

/* The shared scrambler is MSB-first; Phase 4 groups are two or three bits,
 * with bit zero first. Reversing both sides preserves their wire order. */
static uint8_t reverse_group(uint8_t group, unsigned int bits)
{
	return (uint8_t)((((group & 1u) << 2) | (group & 2u) |
			 (group >> 2)) >> (3u - bits));
}

enum v92_phase4_modulator_result v92_phase4_modulator_next(
	struct v92_phase4_modulator *modulator,
	uint8_t input_group, int16_t *sample)
{
	struct v92_phase4_modulator next;
	unsigned int bits;
	uint8_t scrambled_group;
	uint8_t encoded_sign;
	int16_t generated;

	if (modulator == NULL || sample == NULL ||
	    memory_regions_overlap(
		    modulator, sizeof(*modulator), sample, sizeof(*sample)))
		return V92_PHASE4_MODULATOR_INVALID_ARGUMENT;
	if (!v92_phase4_modulator_is_valid(modulator))
		return V92_PHASE4_MODULATOR_INVALID_STATE;
	bits = v92_phase4_modulator_bits_per_symbol(
		modulator->constellation);
	if (input_group >= (1u << bits))
		return V92_PHASE4_MODULATOR_INVALID_ARGUMENT;
	next = *modulator;
	if (v34_scramble_group(
		    &next.scrambler, reverse_group(input_group, bits),
		    bits, &scrambled_group) != V34_FRAMING_OK)
		return V92_PHASE4_MODULATOR_INVALID_STATE;
	scrambled_group = reverse_group(scrambled_group, bits);
	encoded_sign = (uint8_t)(
		(scrambled_group >> (bits - 1u)) ^
		next.previous_encoded_sign);
	if (!raw_level(
		    next.constellation, next.lu,
		    (uint8_t)(scrambled_group &
			      ((1u << (bits - 1u)) - 1u)),
		    encoded_sign, &generated))
		return V92_PHASE4_MODULATOR_INVALID_STATE;
	next.previous_encoded_sign = encoded_sign;
	*modulator = next;
	*sample = generated;
	return V92_PHASE4_MODULATOR_OK;
}

bool v92_phase4_demodulator_is_valid(
	const struct v92_phase4_demodulator *demodulator)
{
	return demodulator != NULL &&
	       parameters_are_valid(
		       demodulator->constellation, demodulator->lu) &&
	       demodulator->previous_encoded_sign <= 1u &&
	       v34_scrambler_is_valid(&demodulator->descrambler) &&
	       demodulator->descrambler.polynomial == V34_POLYNOMIAL_GPA;
}

enum v92_phase4_modulator_result v92_phase4_demodulator_init(
	struct v92_phase4_demodulator *demodulator,
	enum v92_constellation_size constellation,
	uint8_t previous_encoded_sign, int16_t lu)
{
	struct v92_phase4_demodulator initialized = {
		.constellation = constellation,
		.lu = lu,
		.previous_encoded_sign = previous_encoded_sign,
	};

	if (demodulator == NULL || previous_encoded_sign > 1u ||
	    !parameters_are_valid(constellation, lu) ||
	    v34_scrambler_init(
		    &initialized.descrambler, V34_POLYNOMIAL_GPA) !=
		    V34_FRAMING_OK ||
	    !v92_phase4_demodulator_is_valid(&initialized))
		return V92_PHASE4_MODULATOR_INVALID_ARGUMENT;
	*demodulator = initialized;
	return V92_PHASE4_MODULATOR_OK;
}

static bool nearest_magnitude(
	enum v92_constellation_size constellation, int16_t lu,
	double observed_level, uint8_t *magnitude_index,
	int16_t *nearest_level)
{
	double magnitude = fabs(observed_level);
	double best_error = INFINITY;
	uint8_t best_index = 0u;
	int16_t best_level = 0;
	unsigned int count = magnitude_count(constellation);

	if (!isfinite(observed_level) || magnitude_index == NULL ||
	    nearest_level == NULL || count == 0u)
		return false;
	for (unsigned int index = 0; index < count; ++index) {
		int16_t level;
		double error;

		if (!raw_level(
			    constellation, lu, (uint8_t)index, 0u, &level))
			return false;
		error = fabs(magnitude - level);
		if (error >= best_error)
			continue;
		best_error = error;
		best_index = (uint8_t)index;
		best_level = observed_level < 0.0 ? (int16_t)-level : level;
	}
	*magnitude_index = best_index;
	*nearest_level = best_level;
	return true;
}

enum v92_phase4_modulator_result v92_phase4_demodulator_next(
	struct v92_phase4_demodulator *demodulator, double observed_level,
	uint8_t *input_group, int16_t *nearest_level)
{
	struct v92_phase4_demodulator next;
	unsigned int bits;
	uint8_t magnitude_index;
	uint8_t encoded_sign;
	uint8_t scrambled_group;
	uint8_t recovered_group;
	int16_t selected;

	if (demodulator == NULL || input_group == NULL || nearest_level == NULL ||
	    !isfinite(observed_level) ||
	    memory_regions_overlap(
		    demodulator, sizeof(*demodulator),
		    input_group, sizeof(*input_group)) ||
	    memory_regions_overlap(
		    demodulator, sizeof(*demodulator),
		    nearest_level, sizeof(*nearest_level)) ||
	    memory_regions_overlap(
		    input_group, sizeof(*input_group),
		    nearest_level, sizeof(*nearest_level)))
		return V92_PHASE4_MODULATOR_INVALID_ARGUMENT;
	if (!v92_phase4_demodulator_is_valid(demodulator))
		return V92_PHASE4_MODULATOR_INVALID_STATE;
	next = *demodulator;
	bits = v92_phase4_modulator_bits_per_symbol(next.constellation);
	if (!nearest_magnitude(
		    next.constellation, next.lu, observed_level,
		    &magnitude_index, &selected))
		return V92_PHASE4_MODULATOR_INVALID_STATE;
	encoded_sign = observed_level < 0.0 ? 1u : 0u;
	scrambled_group = (uint8_t)(
		magnitude_index |
		((encoded_sign ^ next.previous_encoded_sign) << (bits - 1u)));
	if (v34_descramble_group(
		    &next.descrambler, reverse_group(scrambled_group, bits),
		    bits, &recovered_group) != V34_FRAMING_OK)
		return V92_PHASE4_MODULATOR_INVALID_STATE;
	recovered_group = reverse_group(recovered_group, bits);
	next.previous_encoded_sign = encoded_sign;
	*demodulator = next;
	*input_group = recovered_group;
	*nearest_level = selected;
	return V92_PHASE4_MODULATOR_OK;
}

const char *v92_phase4_modulator_result_name(
	enum v92_phase4_modulator_result result)
{
	switch (result) {
	case V92_PHASE4_MODULATOR_OK:
		return "ok";
	case V92_PHASE4_MODULATOR_INVALID_ARGUMENT:
		return "invalid argument";
	case V92_PHASE4_MODULATOR_INVALID_STATE:
		return "invalid state";
	}
	return "unknown";
}
