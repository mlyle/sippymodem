// SPDX-License-Identifier: BSD-3-Clause

#include "v90_pcm_mapping.h"

#include "mixed_radix.h"
#include "v92_cpt.h"
#include "v92_cpu.h"

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
		if (pcm_constellation_count(&mapping->transmit[interval]) !=
			    mapping->modulus[interval] ||
		    pcm_constellation_count(&mapping->codec[interval]) !=
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

enum v90_pcm_mapping_result v90_pcm_mapping_power(
	const struct v90_pcm_mapping *mapping,
	enum v90_pcm_mapping_plane plane, enum g711_law law,
	uint32_t *floor_power, uint32_t *ceiling_power)
{
	uint64_t words, remainder, stride = 1;
	uint64_t whole = 0, fraction = 0;
	uint32_t floor;

	if (mapping == NULL || floor_power == NULL || ceiling_power == NULL ||
	    floor_power == ceiling_power || !g711_law_is_valid(law) ||
	    (plane != V90_PCM_MAPPING_TRANSMIT && plane != V90_PCM_MAPPING_CODEC))
		return V90_PCM_MAPPING_INVALID_ARGUMENT;
	if (!v90_pcm_mapping_is_valid(mapping))
		return V90_PCM_MAPPING_INVALID_PARAMETERS;
	words = UINT64_C(1) << mapping->modulus_bits;
	remainder = words - 1u;
	for (size_t interval = 0; interval < V90_CP_FRAME_INTERVALS; ++interval) {
		const struct pcm_constellation *constellation =
			plane == V90_PCM_MAPPING_CODEC ? &mapping->codec[interval] :
				&mapping->transmit[interval];
		uint64_t next = remainder / mapping->modulus[interval];
		uint8_t boundary = remainder % mapping->modulus[interval];
		uint8_t label = 0;

		/* Clause 5.4.4 labels descend in Ucode. The occurrence formula in
		 * 8.5.2 accounts for unused tuples above the 2^K input range. */
		for (int ucode = PCM_CONSTELLATION_UCODES - 1; ucode >= 0; --ucode) {
			uint64_t occurrences, upper, lower;
			uint32_t square;
			int16_t level;

			if (!pcm_constellation_contains(constellation, (uint8_t)ucode))
				continue;
			if (!g711_ucode_sample(law, (uint8_t)ucode, true, &level))
				return V90_PCM_MAPPING_INVALID_PARAMETERS;
			if (label < boundary)
				occurrences = stride * (next + 1u);
			else if (label == boundary)
				occurrences = words - stride * (remainder - next);
			else
				occurrences = stride * next;
			label++;
			square = (uint32_t)((int32_t)level * level);
			/* K <= 39, square < 2^30: their product may exceed 64
			 * bits. Split square into 16-bit limbs and divide by 2^K
			 * before accumulating. Upper < 2^53, lower < 2^56,
			 * total fraction < 6*128*2^39; no precision is lost. */
			upper = occurrences * (square >> 16);
			lower = occurrences * (square & UINT16_MAX) +
				((upper & (words - 1u)) << 16);
			whole += ((upper >> mapping->modulus_bits) << 16) +
				 (lower >> mapping->modulus_bits);
			fraction += lower & (words - 1u);
		}
		stride *= mapping->modulus[interval];
		remainder = next;
	}
	whole += fraction >> mapping->modulus_bits;
	fraction &= words - 1u;
	floor = (uint32_t)(whole / V90_CP_FRAME_INTERVALS);
	*floor_power = floor;
	*ceiling_power = floor +
		(whole % V90_CP_FRAME_INTERVALS != 0u || fraction != 0u);
	return V90_PCM_MAPPING_OK;
}

enum v90_pcm_mapping_result v90_pcm_mapping_init_transmit(
	struct v90_pcm_mapping *mapping,
	const struct v90_cp_parameters *parameters, enum g711_law law)
{
	struct v90_pcm_mapping initialized = { 0 };
	uint8_t base;
	bool codec_counts_match = true;

	if (mapping == NULL || parameters == NULL)
		return V90_PCM_MAPPING_INVALID_ARGUMENT;
	if (!g711_law_is_valid(law) ||
	    !v90_cp_parameters_are_valid(parameters) ||
	    parameters->selected_rate_index == 0)
		return V90_PCM_MAPPING_INVALID_PARAMETERS;
	base = parameters->kind == V90_CP_TRAINING ?
		       V90_PHASE4_RATE_BASE : V90_DATA_RATE_BASE;
	initialized.kind = parameters->kind;
	initialized.law = law;
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
			pcm_constellation_count(&initialized.transmit[interval]);
		codec_count = pcm_constellation_count(&initialized.codec[interval]);
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

enum v90_pcm_mapping_result v90_pcm_mapping_init(
	struct v90_pcm_mapping *mapping,
	const struct v90_cp_parameters *parameters)
{
	if (parameters == NULL)
		return V90_PCM_MAPPING_INVALID_ARGUMENT;
	return v90_pcm_mapping_init_transmit(mapping, parameters, parameters->law);
}

enum v90_pcm_mapping_result
v90_pcm_mapping_init_rate_renegotiation(
	struct v90_pcm_mapping *mapping,
	const struct v90_pcm_mapping *training,
	const struct v90_pcm_mapping *preceding_data)
{
	struct v90_pcm_mapping initialized;

	if (mapping == NULL || training == NULL || preceding_data == NULL)
		return V90_PCM_MAPPING_INVALID_ARGUMENT;
	if (!v90_pcm_mapping_is_valid(training) ||
	    training->kind != V90_CP_TRAINING ||
	    !v90_pcm_mapping_is_valid(preceding_data) ||
	    preceding_data->kind != V90_CP_DATA ||
	    training->law != preceding_data->law)
		return V90_PCM_MAPPING_INVALID_PARAMETERS;

	initialized = *training;
	initialized.sign_redundancy = preceding_data->sign_redundancy;
	initialized.sign_bits =
		(uint8_t)(V90_CP_FRAME_INTERVALS -
			  initialized.sign_redundancy);
	initialized.frame_bits =
		(uint8_t)(initialized.modulus_bits + initialized.sign_bits);
	initialized.spectral_lookahead = preceding_data->spectral_lookahead;
	initialized.shaping_a1_q1_6 = preceding_data->shaping_a1_q1_6;
	initialized.shaping_a2_q1_6 = preceding_data->shaping_a2_q1_6;
	initialized.shaping_b1_q1_6 = preceding_data->shaping_b1_q1_6;
	initialized.shaping_b2_q1_6 = preceding_data->shaping_b2_q1_6;
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

enum v90_pcm_mapping_result v90_pcm_mapping_init_from_v92_cpu(
	struct v90_pcm_mapping *mapping,
	const struct v92_cpu_parameters *parameters)
{
	struct v90_cp_parameters common = { 0 };

	if (mapping == NULL || parameters == NULL)
		return V90_PCM_MAPPING_INVALID_ARGUMENT;
	common = (struct v90_cp_parameters){
		.kind = V90_CP_DATA,
		.law = parameters->law,
		.trn1d_ratio_q3_13 = parameters->trn1d_ratio_q3_13,
		.shaping_a1_q1_6 = parameters->shaping_a1_q1_6,
		.shaping_a2_q1_6 = parameters->shaping_a2_q1_6,
		.shaping_b1_q1_6 = parameters->shaping_b1_q1_6,
		.shaping_b2_q1_6 = parameters->shaping_b2_q1_6,
		.selected_rate_index = parameters->data_rate_number,
		.sign_redundancy = parameters->sign_redundancy,
		.spectral_lookahead = parameters->spectral_lookahead_frames,
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
	uint32_t radices[V90_CP_FRAME_INTERVALS];
	uint32_t digits[V90_CP_FRAME_INTERVALS];
	enum mixed_radix_result result;

	if (!v90_pcm_mapping_is_valid(mapping) || bits == NULL ||
	    labels == NULL)
		return V90_PCM_MAPPING_INVALID_ARGUMENT;
	if (bit_count != mapping->modulus_bits)
		return V90_PCM_MAPPING_OUT_OF_RANGE;
	for (size_t interval = 0;
	     interval < V90_CP_FRAME_INTERVALS; ++interval)
		radices[interval] = mapping->modulus[interval];
	result = mixed_radix_encode(
		radices, V90_CP_FRAME_INTERVALS, bits, bit_count, digits);
	if (result == MIXED_RADIX_OUT_OF_RANGE)
		return V90_PCM_MAPPING_OUT_OF_RANGE;
	if (result != MIXED_RADIX_OK)
		return V90_PCM_MAPPING_INVALID_PARAMETERS;
	for (size_t interval = 0;
	     interval < V90_CP_FRAME_INTERVALS; ++interval)
		encoded[interval] = (uint8_t)digits[interval];
	memcpy(labels, encoded, sizeof(encoded));
	return V90_PCM_MAPPING_OK;
}

enum v90_pcm_mapping_result v90_pcm_mapping_decode_modulus(
	const struct v90_pcm_mapping *mapping,
	const uint8_t labels[V90_CP_FRAME_INTERVALS],
	uint8_t *bits, size_t bit_count)
{
	uint8_t decoded[V90_DATA_MAX_MODULUS_BITS];
	uint32_t radices[V90_CP_FRAME_INTERVALS];
	uint32_t digits[V90_CP_FRAME_INTERVALS];
	enum mixed_radix_result result;

	if (!v90_pcm_mapping_is_valid(mapping) || labels == NULL ||
	    bits == NULL)
		return V90_PCM_MAPPING_INVALID_ARGUMENT;
	if (bit_count != mapping->modulus_bits)
		return V90_PCM_MAPPING_OUT_OF_RANGE;
	for (size_t interval = 0;
	     interval < V90_CP_FRAME_INTERVALS; ++interval) {
		radices[interval] = mapping->modulus[interval];
		digits[interval] = labels[interval];
	}
	result = mixed_radix_decode_low_bits(
		radices, V90_CP_FRAME_INTERVALS, digits,
		decoded, bit_count);
	if (result == MIXED_RADIX_OUT_OF_RANGE)
		return V90_PCM_MAPPING_OUT_OF_RANGE;
	if (result != MIXED_RADIX_OK)
		return V90_PCM_MAPPING_INVALID_PARAMETERS;
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
