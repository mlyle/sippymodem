// SPDX-License-Identifier: BSD-3-Clause

#include "v90_cp.h"

static bool law_is_valid(enum g711_law law)
{
	return law == G711_LAW_PCMU || law == G711_LAW_PCMA;
}

static bool kind_is_valid(enum v90_cp_kind kind)
{
	return kind == V90_CP_TRAINING || kind == V90_CP_DATA;
}

bool v90_cp_constellation_contains(
	const struct v90_cp_constellation *constellation,
	uint8_t ucode)
{
	return constellation != NULL &&
	       ucode < V90_CP_CONSTELLATION_UCODES &&
	       (constellation->ucode_mask[ucode / 8u] &
		(uint8_t)(1u << (ucode % 8u))) != 0;
}

static bool constellation_is_empty(
	const struct v90_cp_constellation *constellation)
{
	for (size_t index = 0;
	     index < V90_CP_CONSTELLATION_MASK_BYTES; ++index) {
		if (constellation->ucode_mask[index] != 0)
			return false;
	}
	return true;
}

static bool constellations_are_canonical(
	const struct v90_cp_parameters *parameters)
{
	uint8_t used = 0;

	for (size_t interval = 0;
	     interval < V90_CP_FRAME_INTERVALS; ++interval) {
		uint8_t index = parameters->frame_constellation[interval];

		if (index >= parameters->constellation_count)
			return false;
		used |= (uint8_t)(1u << index);
	}
	if (used != (uint8_t)((1u << parameters->constellation_count) - 1u))
		return false;
	for (size_t index = 0;
	     index < V90_CP_MAX_CONSTELLATIONS; ++index) {
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

bool v90_cp_parameters_are_valid(
	const struct v90_cp_parameters *parameters)
{
	return parameters != NULL && kind_is_valid(parameters->kind) &&
	       law_is_valid(parameters->law) &&
	       parameters->selected_rate_index <= V90_CP_RATE_INDEX_MAX &&
	       parameters->sign_redundancy <= 3 &&
	       parameters->spectral_lookahead <= 3 &&
	       parameters->constellation_count >= 1 &&
	       parameters->constellation_count <=
		       V90_CP_MAX_CONSTELLATIONS &&
	       (parameters->upstream_rate_mask &
		~V90_CP_UPSTREAM_RATE_MASK) == 0 &&
	       constellations_are_canonical(parameters);
}

size_t v90_cp_frame_bits(const struct v90_cp_parameters *parameters)
{
	size_t constellation_sets;

	if (!v90_cp_parameters_are_valid(parameters))
		return 0;
	constellation_sets = parameters->codec_constellations_differ ? 2u : 1u;
	return V90_CP_HEADER_BITS +
	       constellation_sets * parameters->constellation_count *
		       V90_CP_CONSTELLATION_BITS +
	       V90_CP_TRAILER_BITS;
}

size_t v90_cp_frame_bytes(const struct v90_cp_parameters *parameters)
{
	size_t bits = v90_cp_frame_bits(parameters);

	return bits == 0 ? 0 : (bits + 7u) / 8u;
}
