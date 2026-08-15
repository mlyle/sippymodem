// SPDX-License-Identifier: BSD-3-Clause

#include "v90_pcm_mapping.h"

#include "v92_cpt.h"

#include <string.h>

enum {
	V90_PHASE4_RATE_BASE = 8,
	V90_DATA_RATE_BASE = 20,
	V90_PHASE4_MIN_MODULUS_BITS = 6,
	V90_PHASE4_MAX_MODULUS_BITS = 24,
	V90_DATA_MIN_MODULUS_BITS = 15,
	V90_DATA_MAX_MODULUS_BITS = 39,
	V90_SHAPING_COEFFICIENT_LIMIT_Q1_6 = 64,
};

static uint8_t constellation_count(
	const struct pcm_constellation *constellation)
{
	unsigned int count = 0;

	if (constellation == NULL)
		return 0;
	for (size_t index = 0;
	     index < V90_CP_CONSTELLATION_MASK_BYTES; ++index) {
		uint8_t bits = constellation->ucode_mask[index];

		while (bits != 0) {
			count += bits & 1u;
			bits >>= 1;
		}
	}
	return (uint8_t)count;
}

static bool modulus_bits_are_valid(
	enum v90_cp_kind kind, uint8_t bits)
{
	if (kind == V90_CP_TRAINING)
		return bits >= V90_PHASE4_MIN_MODULUS_BITS &&
		       bits <= V90_PHASE4_MAX_MODULUS_BITS;
	return kind == V90_CP_DATA &&
	       bits >= V90_DATA_MIN_MODULUS_BITS &&
	       bits <= V90_DATA_MAX_MODULUS_BITS;
}

static bool mapping_capacity_is_valid(
	const struct v90_pcm_mapping *mapping)
{
	uint64_t product = 1;

	if (mapping->modulus_bits >= 64)
		return false;
	for (size_t interval = 0;
	     interval < V90_CP_FRAME_INTERVALS; ++interval) {
		if (mapping->modulus[interval] == 0 ||
		    product > UINT64_MAX / mapping->modulus[interval])
			return false;
		product *= mapping->modulus[interval];
	}
	return product >= (UINT64_C(1) << mapping->modulus_bits);
}

static bool mapping_planes_differ(
	const struct v90_pcm_mapping *mapping)
{
	return memcmp(mapping->transmit, mapping->codec,
		      sizeof(mapping->transmit)) != 0;
}

static bool shaping_coefficient_is_valid(int8_t coefficient)
{
	return coefficient >= -V90_SHAPING_COEFFICIENT_LIMIT_Q1_6 &&
	       coefficient <= V90_SHAPING_COEFFICIENT_LIMIT_Q1_6;
}

static bool shaping_coefficients_are_valid(
	const struct v90_pcm_mapping *mapping)
{
	return shaping_coefficient_is_valid(mapping->shaping_a1_q1_6) &&
	       shaping_coefficient_is_valid(mapping->shaping_a2_q1_6) &&
	       shaping_coefficient_is_valid(mapping->shaping_b1_q1_6) &&
	       shaping_coefficient_is_valid(mapping->shaping_b2_q1_6);
}

bool v90_pcm_mapping_is_valid(const struct v90_pcm_mapping *mapping)
{
	uint8_t base;

	if (mapping == NULL || !g711_law_is_valid(mapping->law) ||
	    (mapping->kind != V90_CP_TRAINING &&
	     mapping->kind != V90_CP_DATA) ||
	    mapping->sign_redundancy > 3 ||
	    mapping->sign_bits + mapping->sign_redundancy !=
		    V90_CP_FRAME_INTERVALS ||
	    mapping->spectral_lookahead > 3 ||
	    !shaping_coefficients_are_valid(mapping) ||
	    !modulus_bits_are_valid(
		    mapping->kind, mapping->modulus_bits))
		return false;
	base = mapping->kind == V90_CP_TRAINING ?
		       V90_PHASE4_RATE_BASE : V90_DATA_RATE_BASE;
	if (mapping->frame_bits !=
		    mapping->modulus_bits + mapping->sign_bits ||
	    mapping->frame_bits < base + 1u ||
	    mapping->frame_bits > base + V90_CP_RATE_INDEX_MAX)
		return false;
	for (size_t interval = 0;
	     interval < V90_CP_FRAME_INTERVALS; ++interval) {
		if (constellation_count(&mapping->transmit[interval]) !=
			    mapping->modulus[interval] ||
		    constellation_count(&mapping->codec[interval]) !=
			    mapping->modulus[interval])
			return false;
	}
	return mapping->codec_plane_differs ==
		       mapping_planes_differ(mapping) &&
	       mapping_capacity_is_valid(mapping);
}

bool v90_pcm_mapping_equal(
	const struct v90_pcm_mapping *left,
	const struct v90_pcm_mapping *right)
{
	return v90_pcm_mapping_is_valid(left) &&
	       v90_pcm_mapping_is_valid(right) &&
	       left->kind == right->kind && left->law == right->law &&
	       left->frame_bits == right->frame_bits &&
	       left->modulus_bits == right->modulus_bits &&
	       left->sign_bits == right->sign_bits &&
	       left->sign_redundancy == right->sign_redundancy &&
	       left->spectral_lookahead == right->spectral_lookahead &&
	       left->shaping_a1_q1_6 == right->shaping_a1_q1_6 &&
	       left->shaping_a2_q1_6 == right->shaping_a2_q1_6 &&
	       left->shaping_b1_q1_6 == right->shaping_b1_q1_6 &&
	       left->shaping_b2_q1_6 == right->shaping_b2_q1_6 &&
	       left->codec_plane_differs == right->codec_plane_differs &&
	       memcmp(left->transmit, right->transmit,
		      sizeof(left->transmit)) == 0 &&
	       memcmp(left->codec, right->codec,
		      sizeof(left->codec)) == 0 &&
	       memcmp(left->modulus, right->modulus,
		      sizeof(left->modulus)) == 0;
}

enum v90_pcm_mapping_result v90_pcm_mapping_init(
	struct v90_pcm_mapping *mapping,
	const struct v90_cp_parameters *parameters)
{
	struct v90_pcm_mapping initialized = { 0 };
	uint8_t base;
	bool codec_counts_match = true;

	if (mapping == NULL || parameters == NULL)
		return V90_PCM_MAPPING_INVALID_ARGUMENT;
	if (!v90_cp_parameters_are_valid(parameters) ||
	    parameters->selected_rate_index == 0)
		return V90_PCM_MAPPING_INVALID_PARAMETERS;
	base = parameters->kind == V90_CP_TRAINING ?
		       V90_PHASE4_RATE_BASE : V90_DATA_RATE_BASE;
	initialized.kind = parameters->kind;
	initialized.law = parameters->law;
	initialized.frame_bits =
		(uint8_t)(base + parameters->selected_rate_index);
	initialized.sign_redundancy = parameters->sign_redundancy;
	initialized.sign_bits =
		(uint8_t)(V90_CP_FRAME_INTERVALS -
			  parameters->sign_redundancy);
	initialized.modulus_bits =
		(uint8_t)(initialized.frame_bits - initialized.sign_bits);
	initialized.spectral_lookahead = parameters->spectral_lookahead;
	initialized.shaping_a1_q1_6 = parameters->shaping_a1_q1_6;
	initialized.shaping_a2_q1_6 = parameters->shaping_a2_q1_6;
	initialized.shaping_b1_q1_6 = parameters->shaping_b1_q1_6;
	initialized.shaping_b2_q1_6 = parameters->shaping_b2_q1_6;
	for (size_t interval = 0;
	     interval < V90_CP_FRAME_INTERVALS; ++interval) {
		uint8_t index = parameters->frame_constellation[interval];
		uint8_t codec_count;

		initialized.transmit[interval] =
			parameters->transmit_constellations[index];
		initialized.codec[interval] =
			parameters->codec_constellations_differ ?
				parameters->codec_constellations[index] :
				initialized.transmit[interval];
		initialized.modulus[interval] =
			constellation_count(&initialized.transmit[interval]);
		codec_count = constellation_count(&initialized.codec[interval]);
		if (codec_count != initialized.modulus[interval])
			codec_counts_match = false;
	}
	if (!parameters->codec_constellations_differ ||
	    !codec_counts_match)
		memcpy(initialized.codec, initialized.transmit,
		       sizeof(initialized.codec));
	initialized.codec_plane_differs =
		mapping_planes_differ(&initialized);
	if (!v90_pcm_mapping_is_valid(&initialized))
		return V90_PCM_MAPPING_INVALID_PARAMETERS;
	*mapping = initialized;
	return V90_PCM_MAPPING_OK;
}

enum v90_pcm_mapping_result v90_pcm_mapping_init_from_v92_cpt(
	struct v90_pcm_mapping *mapping,
	const struct v92_cpt_parameters *parameters)
{
	struct v90_cp_parameters common = { 0 };

	if (mapping == NULL || parameters == NULL)
		return V90_PCM_MAPPING_INVALID_ARGUMENT;
	common = (struct v90_cp_parameters){
		.kind = V90_CP_TRAINING,
		.law = parameters->law,
		.trn1d_ratio_q3_13 = parameters->trn1d_ratio_q3_13,
		.shaping_a1_q1_6 = parameters->shaping_a1_q1_6,
		.shaping_a2_q1_6 = parameters->shaping_a2_q1_6,
		.shaping_b1_q1_6 = parameters->shaping_b1_q1_6,
		.shaping_b2_q1_6 = parameters->shaping_b2_q1_6,
		.selected_rate_index = parameters->selected_rate_index,
		.sign_redundancy = parameters->sign_redundancy,
		.spectral_lookahead = parameters->spectral_lookahead,
		.constellation_count = parameters->constellation_count,
		.codec_constellations_differ =
			parameters->codec_constellations_differ,
	};
	memcpy(common.transmit_constellations,
	       parameters->transmit_constellations,
	       sizeof(common.transmit_constellations));
	memcpy(common.codec_constellations,
	       parameters->codec_constellations,
	       sizeof(common.codec_constellations));
	memcpy(common.frame_constellation, parameters->frame_constellation,
	       sizeof(common.frame_constellation));
	return v90_pcm_mapping_init(mapping, &common);
}

enum v90_pcm_mapping_result v90_pcm_mapping_encode_modulus(
	const struct v90_pcm_mapping *mapping,
	const uint8_t *bits, size_t bit_count,
	uint8_t labels[V90_CP_FRAME_INTERVALS])
{
	uint8_t encoded[V90_CP_FRAME_INTERVALS];
	uint64_t value = 0;

	if (!v90_pcm_mapping_is_valid(mapping) || bits == NULL ||
	    labels == NULL)
		return V90_PCM_MAPPING_INVALID_ARGUMENT;
	if (bit_count != mapping->modulus_bits)
		return V90_PCM_MAPPING_OUT_OF_RANGE;
	for (size_t bit = 0; bit < bit_count; ++bit) {
		if (bits[bit] > 1)
			return V90_PCM_MAPPING_OUT_OF_RANGE;
		value |= (uint64_t)bits[bit] << bit;
	}
	for (size_t interval = 0;
	     interval < V90_CP_FRAME_INTERVALS; ++interval) {
		encoded[interval] =
			(uint8_t)(value % mapping->modulus[interval]);
		value /= mapping->modulus[interval];
	}
	if (value != 0)
		return V90_PCM_MAPPING_OUT_OF_RANGE;
	memcpy(labels, encoded, sizeof(encoded));
	return V90_PCM_MAPPING_OK;
}

enum v90_pcm_mapping_result v90_pcm_mapping_decode_modulus(
	const struct v90_pcm_mapping *mapping,
	const uint8_t labels[V90_CP_FRAME_INTERVALS],
	uint8_t *bits, size_t bit_count)
{
	uint8_t decoded[V90_DATA_MAX_MODULUS_BITS];
	uint64_t value = 0;
	uint64_t limit;

	if (!v90_pcm_mapping_is_valid(mapping) || labels == NULL ||
	    bits == NULL)
		return V90_PCM_MAPPING_INVALID_ARGUMENT;
	if (bit_count != mapping->modulus_bits)
		return V90_PCM_MAPPING_OUT_OF_RANGE;
	for (size_t interval = 0;
	     interval < V90_CP_FRAME_INTERVALS; ++interval) {
		if (labels[interval] >= mapping->modulus[interval])
			return V90_PCM_MAPPING_OUT_OF_RANGE;
	}
	for (size_t interval = V90_CP_FRAME_INTERVALS;
	     interval > 0; --interval) {
		size_t index = interval - 1u;
		uint8_t modulus = mapping->modulus[index];

		if (value >
		    (UINT64_MAX - labels[index]) / modulus)
			return V90_PCM_MAPPING_INVALID_PARAMETERS;
		value = value * modulus + labels[index];
	}
	limit = UINT64_C(1) << mapping->modulus_bits;
	if (value >= limit)
		return V90_PCM_MAPPING_OUT_OF_RANGE;
	for (size_t bit = 0; bit < bit_count; ++bit)
		decoded[bit] = (uint8_t)((value >> bit) & 1u);
	memcpy(bits, decoded, bit_count);
	return V90_PCM_MAPPING_OK;
}

enum v90_pcm_mapping_result v90_pcm_mapping_ucode(
	const struct v90_pcm_mapping *mapping,
	enum v90_pcm_mapping_plane plane, size_t interval,
	uint8_t label, uint8_t *ucode)
{
	const struct pcm_constellation *constellation;
	uint8_t ordinal = 0;

	if (!v90_pcm_mapping_is_valid(mapping) || ucode == NULL ||
	    (plane != V90_PCM_MAPPING_TRANSMIT &&
	     plane != V90_PCM_MAPPING_CODEC))
		return V90_PCM_MAPPING_INVALID_ARGUMENT;
	if (interval >= V90_CP_FRAME_INTERVALS ||
	    label >= mapping->modulus[interval])
		return V90_PCM_MAPPING_OUT_OF_RANGE;
	constellation = plane == V90_PCM_MAPPING_CODEC ?
				&mapping->codec[interval] :
				&mapping->transmit[interval];
	for (unsigned int candidate = V90_CP_CONSTELLATION_UCODES;
	     candidate > 0; --candidate) {
		uint8_t value = (uint8_t)(candidate - 1u);

		if (!pcm_constellation_contains(constellation, value))
			continue;
		if (ordinal++ == label) {
			*ucode = value;
			return V90_PCM_MAPPING_OK;
		}
	}
	return V90_PCM_MAPPING_INVALID_PARAMETERS;
}

enum v90_pcm_mapping_result v90_pcm_mapping_label(
	const struct v90_pcm_mapping *mapping,
	enum v90_pcm_mapping_plane plane, size_t interval,
	uint8_t ucode, uint8_t *label)
{
	const struct pcm_constellation *constellation;
	uint8_t ordinal = 0;

	if (!v90_pcm_mapping_is_valid(mapping) || label == NULL ||
	    (plane != V90_PCM_MAPPING_TRANSMIT &&
	     plane != V90_PCM_MAPPING_CODEC))
		return V90_PCM_MAPPING_INVALID_ARGUMENT;
	if (interval >= V90_CP_FRAME_INTERVALS ||
	    ucode >= V90_CP_CONSTELLATION_UCODES)
		return V90_PCM_MAPPING_OUT_OF_RANGE;
	constellation = plane == V90_PCM_MAPPING_CODEC ?
				&mapping->codec[interval] :
				&mapping->transmit[interval];
	for (unsigned int candidate = V90_CP_CONSTELLATION_UCODES;
	     candidate > 0; --candidate) {
		uint8_t value = (uint8_t)(candidate - 1u);

		if (!pcm_constellation_contains(constellation, value))
			continue;
		if (value == ucode) {
			*label = ordinal;
			return V90_PCM_MAPPING_OK;
		}
		ordinal++;
	}
	return V90_PCM_MAPPING_OUT_OF_RANGE;
}

const char *v90_pcm_mapping_result_name(
	enum v90_pcm_mapping_result result)
{
	switch (result) {
	case V90_PCM_MAPPING_OK:
		return "ok";
	case V90_PCM_MAPPING_INVALID_ARGUMENT:
		return "invalid argument";
	case V90_PCM_MAPPING_INVALID_PARAMETERS:
		return "invalid parameters";
	case V90_PCM_MAPPING_OUT_OF_RANGE:
		return "out of range";
	}
	return "unknown";
}
