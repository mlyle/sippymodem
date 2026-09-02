// SPDX-License-Identifier: BSD-3-Clause

#include <v90_pcm_mapping.h>
#include <v92_cpt.h>
#include <v92_cpu.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

enum {
	MAX_MODULUS_BITS = 39,
};

static void add_ucode(
	struct pcm_constellation *constellation, uint8_t ucode)
{
	constellation->ucode_mask[ucode / 8u] |=
		(uint8_t)(1u << (ucode % 8u));
}

static void fill_constellation(
	struct pcm_constellation *constellation,
	uint8_t first, uint8_t count)
{
	for (unsigned int ordinal = 0; ordinal < count; ++ordinal)
		add_ucode(
			constellation,
			(uint8_t)((first + ordinal) %
				  V90_CP_CONSTELLATION_UCODES));
}

static struct v90_cp_parameters parameters(
	enum v90_cp_kind kind, uint8_t selected_rate_index,
	uint8_t sign_redundancy, uint8_t constellation_size,
	bool codec_plane)
{
	struct v90_cp_parameters value = {
		.kind = kind,
		.law = G711_LAW_PCMA,
		.upstream_rate_mask = 0x1555u,
		.trn1d_ratio_q3_13 = 0x2345u,
		.shaping_a1_q1_6 = -17,
		.shaping_a2_q1_6 = 9,
		.shaping_b1_q1_6 = -3,
		.shaping_b2_q1_6 = 31,
		.selected_rate_index = selected_rate_index,
		.sign_redundancy = sign_redundancy,
		.spectral_lookahead = 2,
		.constellation_count = 2,
		.codec_constellations_differ = codec_plane,
	};

	fill_constellation(
		&value.transmit_constellations[0], 3,
		constellation_size);
	fill_constellation(
		&value.transmit_constellations[1], 19,
		constellation_size);
	if (codec_plane) {
		fill_constellation(
			&value.codec_constellations[0], 64,
			constellation_size);
		fill_constellation(
			&value.codec_constellations[1], 80,
			constellation_size);
	}
	for (size_t interval = 0;
	     interval < V90_CP_FRAME_INTERVALS; ++interval)
		value.frame_constellation[interval] =
			(uint8_t)(interval % 2u);
	return value;
}

static struct v90_cp_parameters mixed_parameters(void)
{
	static const uint8_t sizes[V90_CP_FRAME_INTERVALS] = {
		2, 3, 5, 7, 11, 13,
	};
	struct v90_cp_parameters value =
		parameters(V90_CP_TRAINING, 11, 1, 2, true);

	memset(value.transmit_constellations, 0,
	       sizeof(value.transmit_constellations));
	memset(value.codec_constellations, 0,
	       sizeof(value.codec_constellations));
	value.constellation_count = V90_CP_FRAME_INTERVALS;
	for (size_t interval = 0;
	     interval < V90_CP_FRAME_INTERVALS; ++interval) {
		fill_constellation(
			&value.transmit_constellations[interval],
			(uint8_t)(interval * 16u), sizes[interval]);
		fill_constellation(
			&value.codec_constellations[interval],
			(uint8_t)(7u + interval * 16u), sizes[interval]);
		value.frame_constellation[interval] = (uint8_t)interval;
	}
	return value;
}

static void bits_from_value(
	uint64_t value, uint8_t *bits, size_t bit_count)
{
	for (size_t bit = 0; bit < bit_count; ++bit)
		bits[bit] = (uint8_t)((value >> bit) & 1u);
}

static uint64_t labels_value(
	const struct v90_pcm_mapping *mapping,
	const uint8_t labels[V90_CP_FRAME_INTERVALS])
{
	uint64_t value = 0;
	uint64_t place = 1;

	for (size_t interval = 0;
	     interval < V90_CP_FRAME_INTERVALS; ++interval) {
		value += place * labels[interval];
		place *= mapping->modulus[interval];
	}
	return value;
}

static bool test_training_mapping(void)
{
	struct v90_cp_parameters source =
		parameters(V90_CP_TRAINING, 13, 1, 8, true);
	struct v90_pcm_mapping mapping;
	uint8_t bits[16];
	uint8_t decoded[16] = { 0 };
	uint8_t labels[V90_CP_FRAME_INTERVALS] = { 0 };
	static const uint8_t expected_labels[] = { 2, 3, 5, 2, 2, 1 };
	uint16_t input = 0xa55au;

	CHECK(v90_pcm_mapping_init(&mapping, &source) ==
		      V90_PCM_MAPPING_OK &&
	      v90_pcm_mapping_is_valid(&mapping),
	      "valid CPt mapping did not initialize");
	CHECK(mapping.kind == V90_CP_TRAINING &&
	      mapping.law == G711_LAW_PCMA &&
	      mapping.frame_bits == 21 && mapping.modulus_bits == 16 &&
	      mapping.sign_bits == 5 && mapping.sign_redundancy == 1 &&
	      mapping.spectral_lookahead == 2 &&
	      mapping.shaping_a1_q1_6 == -17 &&
	      mapping.shaping_a2_q1_6 == 9 &&
	      mapping.shaping_b1_q1_6 == -3 &&
	      mapping.shaping_b2_q1_6 == 31 &&
	      mapping.codec_plane_differs,
	      "CPt mapping did not retain its derived parameters");
	for (size_t interval = 0;
	     interval < V90_CP_FRAME_INTERVALS; ++interval) {
		uint8_t largest = 0;
		uint8_t smallest = 0;
		uint8_t codec_largest = 0;
		uint8_t offset = interval % 2u == 0 ? 0 : 16;

		CHECK(mapping.modulus[interval] == 8 &&
		      v90_pcm_mapping_ucode(
			      &mapping, V90_PCM_MAPPING_TRANSMIT,
			      interval, 0, &largest) ==
			      V90_PCM_MAPPING_OK &&
		      v90_pcm_mapping_ucode(
			      &mapping, V90_PCM_MAPPING_TRANSMIT,
			      interval, 7, &smallest) ==
			      V90_PCM_MAPPING_OK &&
		      v90_pcm_mapping_ucode(
			      &mapping, V90_PCM_MAPPING_CODEC,
			      interval, 0, &codec_largest) ==
			      V90_PCM_MAPPING_OK &&
		      largest == (uint8_t)(10 + offset) &&
		      smallest == (uint8_t)(3 + offset) &&
		      codec_largest == (uint8_t)(71 + offset),
		      "interval %zu changed descending-Ucode labeling",
		      interval);
	}
	for (size_t bit = 0; bit < ARRAY_LENGTH(bits); ++bit)
		bits[bit] = (uint8_t)((input >> bit) & 1u);
	CHECK(v90_pcm_mapping_encode_modulus(
		      &mapping, bits, ARRAY_LENGTH(bits), labels) ==
		      V90_PCM_MAPPING_OK &&
	      memcmp(labels, expected_labels, sizeof(labels)) == 0 &&
	      labels_value(&mapping, labels) == input &&
	      v90_pcm_mapping_decode_modulus(
		      &mapping, labels, decoded, ARRAY_LENGTH(decoded)) ==
		      V90_PCM_MAPPING_OK &&
	      memcmp(decoded, bits, sizeof(bits)) == 0,
	      "mixed-radix mapping changed for 0xa55a");
	return true;
}

static bool test_rate_renegotiation_mapping(void)
{
	struct v90_cp_parameters training_parameters =
		parameters(V90_CP_TRAINING, 13, 0, 8, true);
	struct v90_cp_parameters data_parameters =
		parameters(V90_CP_DATA, 1, 1, 8, false);
	struct v90_pcm_mapping training;
	struct v90_pcm_mapping data;
	struct v90_pcm_mapping derived;
	struct v90_pcm_mapping expected;
	struct v90_pcm_mapping saved;

	data_parameters.spectral_lookahead = 3;
	data_parameters.shaping_a1_q1_6 = 12;
	data_parameters.shaping_a2_q1_6 = -23;
	data_parameters.shaping_b1_q1_6 = 7;
	data_parameters.shaping_b2_q1_6 = -4;
	CHECK(v90_pcm_mapping_init(&training, &training_parameters) ==
		      V90_PCM_MAPPING_OK &&
	      v90_pcm_mapping_init(&data, &data_parameters) ==
		      V90_PCM_MAPPING_OK,
	      "rate-renegotiation fixtures did not initialize");
	expected = training;
	expected.sign_redundancy = data.sign_redundancy;
	expected.sign_bits = data.sign_bits;
	expected.frame_bits =
		(uint8_t)(expected.modulus_bits + expected.sign_bits);
	expected.spectral_lookahead = data.spectral_lookahead;
	expected.shaping_a1_q1_6 = data.shaping_a1_q1_6;
	expected.shaping_a2_q1_6 = data.shaping_a2_q1_6;
	expected.shaping_b1_q1_6 = data.shaping_b1_q1_6;
	expected.shaping_b2_q1_6 = data.shaping_b2_q1_6;
	CHECK(v90_pcm_mapping_init_rate_renegotiation(
		      &derived, &training, &data) == V90_PCM_MAPPING_OK &&
	      v90_pcm_mapping_equal(&derived, &expected) &&
	      derived.kind == V90_CP_TRAINING &&
	      derived.modulus_bits == training.modulus_bits &&
	      memcmp(derived.transmit, training.transmit,
		     sizeof(derived.transmit)) == 0 &&
	      memcmp(derived.codec, training.codec,
		     sizeof(derived.codec)) == 0,
	      "rate renegotiation changed CPt constellations or data shaping");

	memset(&derived, 0xa5, sizeof(derived));
	saved = derived;
	data.law = G711_LAW_PCMU;
	CHECK(v90_pcm_mapping_init_rate_renegotiation(
		      &derived, &training, &data) ==
		      V90_PCM_MAPPING_INVALID_PARAMETERS &&
	      v90_pcm_mapping_init_rate_renegotiation(
		      &derived, &training, &training) ==
		      V90_PCM_MAPPING_INVALID_PARAMETERS &&
	      v90_pcm_mapping_init_rate_renegotiation(
		      NULL, &training, &data) ==
		      V90_PCM_MAPPING_INVALID_ARGUMENT &&
	      v90_pcm_mapping_init_rate_renegotiation(
		      &derived, NULL, &data) ==
		      V90_PCM_MAPPING_INVALID_ARGUMENT &&
	      v90_pcm_mapping_init_rate_renegotiation(
		      &derived, &training, NULL) ==
		      V90_PCM_MAPPING_INVALID_ARGUMENT &&
	      memcmp(&derived, &saved, sizeof(derived)) == 0,
	      "rejected rate-renegotiation mapping changed its destination");
	return true;
}

static struct v92_cpt_parameters v92_parameters(
	const struct v90_cp_parameters *source)
{
	struct v92_cpt_parameters parameters = {
		.law = source->law,
		.trn1d_ratio_q3_13 = source->trn1d_ratio_q3_13,
		.shaping_a1_q1_6 = source->shaping_a1_q1_6,
		.shaping_a2_q1_6 = source->shaping_a2_q1_6,
		.shaping_b1_q1_6 = source->shaping_b1_q1_6,
		.shaping_b2_q1_6 = source->shaping_b2_q1_6,
		.selected_rate_index = source->selected_rate_index,
		.sign_redundancy = source->sign_redundancy,
		.spectral_lookahead = source->spectral_lookahead,
		.constellation_count = source->constellation_count,
		.acknowledges_peer = true,
		.codec_constellations_differ =
			source->codec_constellations_differ,
	};

	memcpy(parameters.transmit_constellations,
	       source->transmit_constellations,
	       sizeof(parameters.transmit_constellations));
	memcpy(parameters.codec_constellations,
	       source->codec_constellations,
	       sizeof(parameters.codec_constellations));
	memcpy(parameters.frame_constellation, source->frame_constellation,
	       sizeof(parameters.frame_constellation));
	return parameters;
}

static bool test_v92_cpt_mapping(void)
{
	for (unsigned int law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		for (uint8_t rate = 1u; rate <= V92_CPT_RATE_INDEX_MAX; ++rate) {
			uint8_t constellation_size = rate <= 4u ? 4u :
						  rate <= 13u ? 8u : 16u;
			uint8_t sign_redundancy = rate < 4u ?
				(uint8_t)(4u - rate) :
				rate > 19u ? (uint8_t)(22u - rate) :
					     (uint8_t)(rate % 4u);
			struct v90_cp_parameters source = parameters(
				V90_CP_TRAINING, rate, sign_redundancy,
				constellation_size, (rate & 1u) != 0u);
			struct v92_cpt_parameters cpt;
			struct v90_pcm_mapping from_v90;
			struct v90_pcm_mapping from_v92;

			source.law = (enum g711_law)law;
			cpt = v92_parameters(&source);
			CHECK(v90_pcm_mapping_init(&from_v90, &source) ==
				      V90_PCM_MAPPING_OK &&
			      v90_pcm_mapping_init_from_v92_cpt(
				      &from_v92, &cpt) == V90_PCM_MAPPING_OK &&
			      v90_pcm_mapping_equal(&from_v90, &from_v92),
			      "V.92 CPt map differs at law %u rate %u",
			      law, rate);
		}
	}
	{
		struct v90_cp_parameters source = parameters(
			V90_CP_TRAINING, 13u, 1u, 8u, false);
		struct v92_cpt_parameters invalid = v92_parameters(&source);
		struct v90_pcm_mapping mapping;
		struct v90_pcm_mapping before;

		memset(&mapping, 0xa5, sizeof(mapping));
		before = mapping;
		invalid.selected_rate_index = 0u;
		CHECK(v90_pcm_mapping_init_from_v92_cpt(&mapping, &invalid) ==
			      V90_PCM_MAPPING_INVALID_PARAMETERS &&
		      memcmp(&mapping, &before, sizeof(mapping)) == 0 &&
		      v90_pcm_mapping_init_from_v92_cpt(NULL, &invalid) ==
			      V90_PCM_MAPPING_INVALID_ARGUMENT &&
		      v90_pcm_mapping_init_from_v92_cpt(&mapping, NULL) ==
			      V90_PCM_MAPPING_INVALID_ARGUMENT,
		      "rejected V.92 CPt changed the mapping");
	}
	return true;
}

static struct v92_cpu_parameters v92_cpu_parameters(
	const struct v90_cp_parameters *source)
{
	struct v92_cpu_parameters parameters = {
		.law = source->law,
		.trn1d_ratio_q3_13 = source->trn1d_ratio_q3_13,
		.shaping_a1_q1_6 = source->shaping_a1_q1_6,
		.shaping_a2_q1_6 = source->shaping_a2_q1_6,
		.shaping_b1_q1_6 = source->shaping_b1_q1_6,
		.shaping_b2_q1_6 = source->shaping_b2_q1_6,
		.data_rate_number = source->selected_rate_index,
		.sign_redundancy = source->sign_redundancy,
		.spectral_lookahead_frames = source->spectral_lookahead,
		.constellation_count = source->constellation_count,
		.acknowledges_cpd = true,
		.codec_constellations_differ =
			source->codec_constellations_differ,
	};

	memcpy(parameters.transmit_constellations,
	       source->transmit_constellations,
	       sizeof(parameters.transmit_constellations));
	memcpy(parameters.codec_constellations,
	       source->codec_constellations,
	       sizeof(parameters.codec_constellations));
	memcpy(parameters.frame_constellation, source->frame_constellation,
	       sizeof(parameters.frame_constellation));
	return parameters;
}

static bool test_v92_cpu_mapping(void)
{
	for (unsigned int law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law) {
		for (uint8_t rate = 1u;
		     rate <= V92_CPU_MAX_DATA_RATE_NUMBER; ++rate) {
			uint8_t sign_redundancy = rate < 4u ?
				(uint8_t)(4u - rate) :
				rate > 19u ? (uint8_t)(22u - rate) :
					     (uint8_t)(rate % 4u);
			uint8_t modulus_bits = (uint8_t)(
				14u + rate + sign_redundancy);
			uint8_t constellation_size = (uint8_t)(
				1u << ((modulus_bits +
					 V90_CP_FRAME_INTERVALS - 1u) /
					V90_CP_FRAME_INTERVALS));
			struct v90_cp_parameters source = parameters(
				V90_CP_DATA, rate, sign_redundancy,
				constellation_size, (rate & 1u) != 0u);
			struct v92_cpu_parameters cpu;
			struct v90_pcm_mapping from_v90;
			struct v90_pcm_mapping from_v92;

			source.law = (enum g711_law)law;
			cpu = v92_cpu_parameters(&source);
			CHECK(v90_pcm_mapping_init(&from_v90, &source) ==
				      V90_PCM_MAPPING_OK &&
			      v90_pcm_mapping_init_from_v92_cpu(
				      &from_v92, &cpu) == V90_PCM_MAPPING_OK &&
			      v90_pcm_mapping_equal(&from_v90, &from_v92),
			      "V.92 CPu map differs at law %u rate %u",
			      law, rate);
		}
	}
	{
		struct v90_cp_parameters source = parameters(
			V90_CP_DATA, 13u, 1u, 16u, false);
		struct v92_cpu_parameters invalid = v92_cpu_parameters(&source);
		struct v90_pcm_mapping mapping;
		struct v90_pcm_mapping before;

		memset(&mapping, 0xa5, sizeof(mapping));
		before = mapping;
		invalid.data_rate_number = 0u;
		CHECK(v90_pcm_mapping_init_from_v92_cpu(&mapping, &invalid) ==
			      V90_PCM_MAPPING_INVALID_PARAMETERS &&
		      memcmp(&mapping, &before, sizeof(mapping)) == 0,
		      "V.92 CPu cleardown changed the mapping");
		invalid = v92_cpu_parameters(&source);
		add_ucode(&invalid.transmit_constellations[2], 3u);
		CHECK(v90_pcm_mapping_init_from_v92_cpu(&mapping, &invalid) ==
			      V90_PCM_MAPPING_INVALID_PARAMETERS &&
		      memcmp(&mapping, &before, sizeof(mapping)) == 0 &&
		      v90_pcm_mapping_init_from_v92_cpu(NULL, &invalid) ==
			      V90_PCM_MAPPING_INVALID_ARGUMENT &&
		      v90_pcm_mapping_init_from_v92_cpu(&mapping, NULL) ==
			      V90_PCM_MAPPING_INVALID_ARGUMENT,
		      "rejected V.92 CPu changed the mapping");
	}
	return true;
}

static bool test_generated_inverse_mapping(void)
{
	struct v90_cp_parameters source = mixed_parameters();
	struct v90_pcm_mapping mapping;
	uint8_t bits[MAX_MODULUS_BITS];
	uint8_t decoded[MAX_MODULUS_BITS];
	uint8_t labels[V90_CP_FRAME_INTERVALS];
	uint8_t recovered[V90_CP_FRAME_INTERVALS];
	uint64_t limit;

	CHECK(v90_pcm_mapping_init(&mapping, &source) ==
		      V90_PCM_MAPPING_OK &&
	      mapping.modulus_bits == 14 &&
	      mapping.modulus[0] == 2 && mapping.modulus[1] == 3 &&
	      mapping.modulus[2] == 5 && mapping.modulus[3] == 7 &&
	      mapping.modulus[4] == 11 && mapping.modulus[5] == 13,
	      "uneven generated mapping did not initialize");
	limit = UINT64_C(1) << mapping.modulus_bits;
	for (uint64_t value = 0; value < limit; ++value) {
		bits_from_value(value, bits, mapping.modulus_bits);
		CHECK(v90_pcm_mapping_encode_modulus(
			      &mapping, bits, mapping.modulus_bits,
			      labels) == V90_PCM_MAPPING_OK &&
		      labels_value(&mapping, labels) == value &&
		      v90_pcm_mapping_decode_modulus(
			      &mapping, labels, decoded,
			      mapping.modulus_bits) ==
			      V90_PCM_MAPPING_OK &&
		      memcmp(decoded, bits, mapping.modulus_bits) == 0,
		      "uneven mixed-radix round trip failed for %llu",
		      (unsigned long long)value);
		for (size_t interval = 0;
		     interval < V90_CP_FRAME_INTERVALS; ++interval) {
			uint8_t transmit;
			uint8_t codec;

			CHECK(v90_pcm_mapping_ucode(
				      &mapping,
				      V90_PCM_MAPPING_TRANSMIT, interval,
				      labels[interval], &transmit) ==
				      V90_PCM_MAPPING_OK &&
			      v90_pcm_mapping_label(
				      &mapping,
				      V90_PCM_MAPPING_TRANSMIT, interval,
				      transmit, &recovered[interval]) ==
				      V90_PCM_MAPPING_OK &&
			      recovered[interval] == labels[interval] &&
			      v90_pcm_mapping_ucode(
				      &mapping, V90_PCM_MAPPING_CODEC,
				      interval, labels[interval], &codec) ==
				      V90_PCM_MAPPING_OK &&
			      v90_pcm_mapping_label(
				      &mapping, V90_PCM_MAPPING_CODEC,
				      interval, codec,
				      &recovered[interval]) ==
				      V90_PCM_MAPPING_OK &&
			      recovered[interval] == labels[interval],
			      "Ucode inverse failed for value %llu interval %zu",
			      (unsigned long long)value, interval);
		}
	}
	return true;
}

static bool test_inverse_rate_boundaries(void)
{
	static const struct {
		enum v90_cp_kind kind;
		uint8_t rate_index;
		uint8_t sign_redundancy;
		uint8_t constellation_size;
	} cases[] = {
		{ V90_CP_TRAINING, 1, 3, 2 },
		{ V90_CP_TRAINING, 22, 0, 16 },
		{ V90_CP_DATA, 1, 0, 8 },
		{ V90_CP_DATA, 22, 3, 128 },
	};
	uint8_t bits[MAX_MODULUS_BITS];
	uint8_t decoded[MAX_MODULUS_BITS];
	uint8_t labels[V90_CP_FRAME_INTERVALS];

	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index) {
		struct v90_cp_parameters source = parameters(
			cases[index].kind, cases[index].rate_index,
			cases[index].sign_redundancy,
			cases[index].constellation_size, false);
		struct v90_pcm_mapping mapping;
		uint64_t limit;
		uint64_t values[4];

		CHECK(v90_pcm_mapping_init(&mapping, &source) ==
			      V90_PCM_MAPPING_OK,
		      "inverse boundary %zu did not initialize", index);
		limit = UINT64_C(1) << mapping.modulus_bits;
		values[0] = 0;
		values[1] = 1;
		values[2] = limit / 3u;
		values[3] = limit - 1u;
		for (size_t pattern = 0;
		     pattern < ARRAY_LENGTH(values); ++pattern) {
			bits_from_value(
				values[pattern], bits,
				mapping.modulus_bits);
			CHECK(v90_pcm_mapping_encode_modulus(
				      &mapping, bits,
				      mapping.modulus_bits, labels) ==
				      V90_PCM_MAPPING_OK &&
			      v90_pcm_mapping_decode_modulus(
				      &mapping, labels, decoded,
				      mapping.modulus_bits) ==
				      V90_PCM_MAPPING_OK &&
			      memcmp(decoded, bits,
				     mapping.modulus_bits) == 0,
			      "inverse boundary %zu pattern %zu failed",
			      index, pattern);
		}
	}
	return true;
}

static bool test_inverse_rejection(void)
{
	struct v90_cp_parameters source = mixed_parameters();
	struct v90_pcm_mapping mapping;
	uint8_t labels[V90_CP_FRAME_INTERVALS];
	uint8_t bits[MAX_MODULUS_BITS];
	uint8_t saved_bits[MAX_MODULUS_BITS];
	uint8_t label = 0xa5;

	CHECK(v90_pcm_mapping_init(&mapping, &source) ==
		      V90_PCM_MAPPING_OK,
	      "inverse rejection fixture did not initialize");
	for (size_t interval = 0;
	     interval < V90_CP_FRAME_INTERVALS; ++interval)
		labels[interval] = (uint8_t)(mapping.modulus[interval] - 1u);
	memset(bits, 0xa5, sizeof(bits));
	memcpy(saved_bits, bits, sizeof(bits));
	CHECK(labels_value(&mapping, labels) >=
		      (UINT64_C(1) << mapping.modulus_bits) &&
	      v90_pcm_mapping_decode_modulus(
		      &mapping, labels, bits, mapping.modulus_bits) ==
		      V90_PCM_MAPPING_OUT_OF_RANGE &&
	      memcmp(bits, saved_bits, sizeof(bits)) == 0,
	      "unused mixed-radix tuple was not rejected transactionally");
	labels[0] = mapping.modulus[0];
	CHECK(v90_pcm_mapping_decode_modulus(
		      &mapping, labels, bits, mapping.modulus_bits) ==
		      V90_PCM_MAPPING_OUT_OF_RANGE &&
	      v90_pcm_mapping_decode_modulus(
		      &mapping, labels, bits,
		      mapping.modulus_bits - 1u) ==
		      V90_PCM_MAPPING_OUT_OF_RANGE &&
	      v90_pcm_mapping_decode_modulus(
		      NULL, labels, bits, mapping.modulus_bits) ==
		      V90_PCM_MAPPING_INVALID_ARGUMENT &&
	      v90_pcm_mapping_decode_modulus(
		      &mapping, NULL, bits, mapping.modulus_bits) ==
		      V90_PCM_MAPPING_INVALID_ARGUMENT &&
	      v90_pcm_mapping_decode_modulus(
		      &mapping, labels, NULL, mapping.modulus_bits) ==
		      V90_PCM_MAPPING_INVALID_ARGUMENT &&
	      memcmp(bits, saved_bits, sizeof(bits)) == 0,
	      "invalid inverse call changed its destination");

	CHECK(v90_pcm_mapping_label(
		      &mapping, V90_PCM_MAPPING_TRANSMIT, 0,
		      127, &label) == V90_PCM_MAPPING_OUT_OF_RANGE &&
	      v90_pcm_mapping_label(
		      &mapping, V90_PCM_MAPPING_TRANSMIT,
		      V90_CP_FRAME_INTERVALS, 0, &label) ==
		      V90_PCM_MAPPING_OUT_OF_RANGE &&
	      v90_pcm_mapping_label(
		      &mapping, (enum v90_pcm_mapping_plane)99,
		      0, 0, &label) ==
		      V90_PCM_MAPPING_INVALID_ARGUMENT &&
	      v90_pcm_mapping_label(
		      NULL, V90_PCM_MAPPING_TRANSMIT,
		      0, 0, &label) ==
		      V90_PCM_MAPPING_INVALID_ARGUMENT &&
	      v90_pcm_mapping_label(
		      &mapping, V90_PCM_MAPPING_TRANSMIT,
		      0, 0, NULL) ==
		      V90_PCM_MAPPING_INVALID_ARGUMENT &&
	      label == 0xa5,
	      "rejected inverse Ucode lookup changed its output");
	return true;
}

static bool test_rate_boundaries(void)
{
	static const struct {
		enum v90_cp_kind kind;
		uint8_t rate_index;
		uint8_t sign_redundancy;
		uint8_t constellation_size;
		uint8_t frame_bits;
		uint8_t modulus_bits;
	} cases[] = {
		{ V90_CP_TRAINING, 1, 3, 2, 9, 6 },
		{ V90_CP_TRAINING, 22, 0, 16, 30, 24 },
		{ V90_CP_DATA, 1, 0, 8, 21, 15 },
		{ V90_CP_DATA, 22, 3, 128, 42, 39 },
	};

	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index) {
		struct v90_cp_parameters source = parameters(
			cases[index].kind, cases[index].rate_index,
			cases[index].sign_redundancy,
			cases[index].constellation_size, false);
		struct v90_pcm_mapping mapping;

		CHECK(v90_pcm_mapping_init(&mapping, &source) ==
			      V90_PCM_MAPPING_OK &&
		      mapping.frame_bits == cases[index].frame_bits &&
		      mapping.modulus_bits == cases[index].modulus_bits &&
		      mapping.sign_bits ==
			      (uint8_t)(V90_CP_FRAME_INTERVALS -
					cases[index].sign_redundancy),
		      "mapping boundary %zu was not accepted exactly",
		      index);
	}
	return true;
}

static bool rejected_init_is_transactional(
	const struct v90_cp_parameters *source)
{
	struct v90_pcm_mapping mapping;
	struct v90_pcm_mapping saved;

	memset(&mapping, 0xa5, sizeof(mapping));
	saved = mapping;
	return v90_pcm_mapping_init(&mapping, source) ==
		       V90_PCM_MAPPING_INVALID_PARAMETERS &&
	       memcmp(&mapping, &saved, sizeof(mapping)) == 0;
}

static bool test_capacity_and_codec_fallback(void)
{
	struct v90_cp_parameters source =
		parameters(V90_CP_TRAINING, 13, 1, 3, false);
	struct v90_pcm_mapping mapping;

	CHECK(rejected_init_is_transactional(&source),
	      "CPt accepted fewer than 2^K mapping combinations");
	source = parameters(V90_CP_TRAINING, 1, 0, 8, false);
	CHECK(rejected_init_is_transactional(&source),
	      "CPt accepted K below the Phase 4 range");
	source = parameters(V90_CP_DATA, 0, 0, 8, false);
	CHECK(rejected_init_is_transactional(&source),
	      "mapping accepted a cleardown CP");

	source = parameters(V90_CP_TRAINING, 13, 1, 8, true);
	source.codec_constellations[0].ucode_mask[64u / 8u] &=
		(uint8_t)~(1u << (64u % 8u));
	CHECK(v90_cp_parameters_are_valid(&source) &&
	      v90_pcm_mapping_init(&mapping, &source) ==
		      V90_PCM_MAPPING_OK &&
	      !mapping.codec_plane_differs &&
	      memcmp(mapping.transmit, mapping.codec,
		     sizeof(mapping.transmit)) == 0,
	      "incompatible codec cardinality did not fall back atomically");

	source = parameters(V90_CP_TRAINING, 13, 1, 8, true);
	memcpy(source.codec_constellations,
	       source.transmit_constellations,
	       sizeof(source.codec_constellations));
	CHECK(v90_pcm_mapping_init(&mapping, &source) ==
		      V90_PCM_MAPPING_OK &&
	      !mapping.codec_plane_differs,
	      "identical codec mappings were marked different");
	return true;
}

static bool test_shaping_coefficient_boundaries(void)
{
	struct v90_cp_parameters source =
		parameters(V90_CP_TRAINING, 13, 1, 8, false);
	struct v90_pcm_mapping mapping;

	source.shaping_a1_q1_6 = -64;
	source.shaping_a2_q1_6 = 64;
	source.shaping_b1_q1_6 = -64;
	source.shaping_b2_q1_6 = 64;
	CHECK(v90_pcm_mapping_init(&mapping, &source) ==
		      V90_PCM_MAPPING_OK,
	      "mapping rejected coefficient magnitudes of exactly 1.0");
	source.shaping_a1_q1_6 = -65;
	CHECK(rejected_init_is_transactional(&source),
	      "mapping accepted a shaping coefficient below -1.0");
	source.shaping_a1_q1_6 = -64;
	source.shaping_b2_q1_6 = 65;
	CHECK(rejected_init_is_transactional(&source),
	      "mapping accepted a shaping coefficient above 1.0");

	mapping.shaping_b2_q1_6 = 65;
	CHECK(!v90_pcm_mapping_is_valid(&mapping),
	      "mapping validator accepted an out-of-range coefficient");
	return true;
}

static bool test_contracts(void)
{
	struct v90_cp_parameters source =
		parameters(V90_CP_TRAINING, 13, 1, 8, true);
	struct v90_cp_parameters invalid = source;
	struct v90_pcm_mapping mapping;
	struct v90_pcm_mapping changed;
	uint8_t bits[16] = { 0 };
	uint8_t labels[V90_CP_FRAME_INTERVALS];
	uint8_t saved_labels[V90_CP_FRAME_INTERVALS];
	uint8_t ucode = 0xa5;

	CHECK(v90_pcm_mapping_init(&mapping, &source) ==
		      V90_PCM_MAPPING_OK,
	      "contract fixture did not initialize");
	memset(labels, 0xa5, sizeof(labels));
	memcpy(saved_labels, labels, sizeof(labels));
	bits[7] = 2;
	CHECK(v90_pcm_mapping_encode_modulus(
		      &mapping, bits, ARRAY_LENGTH(bits), labels) ==
		      V90_PCM_MAPPING_OUT_OF_RANGE &&
	      memcmp(labels, saved_labels, sizeof(labels)) == 0,
	      "invalid modulus bit changed the output");
	bits[7] = 0;
	CHECK(v90_pcm_mapping_encode_modulus(
		      &mapping, bits, ARRAY_LENGTH(bits) - 1u, labels) ==
		      V90_PCM_MAPPING_OUT_OF_RANGE &&
	      memcmp(labels, saved_labels, sizeof(labels)) == 0 &&
	      v90_pcm_mapping_encode_modulus(
		      NULL, bits, ARRAY_LENGTH(bits), labels) ==
		      V90_PCM_MAPPING_INVALID_ARGUMENT,
	      "invalid modulus call was not transactional");

	CHECK(v90_pcm_mapping_ucode(
		      &mapping, V90_PCM_MAPPING_TRANSMIT,
		      V90_CP_FRAME_INTERVALS, 0, &ucode) ==
		      V90_PCM_MAPPING_OUT_OF_RANGE &&
	      v90_pcm_mapping_ucode(
		      &mapping, V90_PCM_MAPPING_TRANSMIT, 0,
		      mapping.modulus[0], &ucode) ==
		      V90_PCM_MAPPING_OUT_OF_RANGE &&
	      v90_pcm_mapping_ucode(
		      &mapping, (enum v90_pcm_mapping_plane)99,
		      0, 0, &ucode) ==
		      V90_PCM_MAPPING_INVALID_ARGUMENT &&
	      ucode == 0xa5,
	      "rejected Ucode lookup changed its output");

	changed = mapping;
	changed.modulus[0]--;
	CHECK(!v90_pcm_mapping_is_valid(&changed),
	      "mapping accepted an inconsistent modulus");
	changed = mapping;
	changed.codec_plane_differs = false;
	CHECK(!v90_pcm_mapping_is_valid(&changed),
	      "mapping accepted an inconsistent codec-plane flag");
	changed = mapping;
	CHECK(v90_pcm_mapping_equal(&mapping, &changed),
	      "identical mappings did not compare equal");
	changed.law = G711_LAW_PCMU;
	CHECK(v90_pcm_mapping_is_valid(&changed) &&
	      !v90_pcm_mapping_equal(&mapping, &changed) &&
	      !v90_pcm_mapping_equal(NULL, &mapping),
	      "mapping equality ignored media law or invalid input");
	changed = mapping;
	changed.frame_bits++;
	CHECK(!v90_pcm_mapping_is_valid(&changed) &&
	      !v90_pcm_mapping_equal(&mapping, &changed) &&
	      !v90_pcm_mapping_is_valid(NULL),
	      "mapping accepted inconsistent derived state");

	invalid.selected_rate_index = V90_CP_RATE_INDEX_MAX + 1u;
	CHECK(rejected_init_is_transactional(&invalid) &&
	      v90_pcm_mapping_init(NULL, &source) ==
		      V90_PCM_MAPPING_INVALID_ARGUMENT &&
	      strcmp(v90_pcm_mapping_result_name(V90_PCM_MAPPING_OK),
		     "ok") == 0 &&
	      strcmp(v90_pcm_mapping_result_name(
			     (enum v90_pcm_mapping_result)99),
		     "unknown") == 0,
	      "mapping call contract changed");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "training mapping", test_training_mapping },
		{ "rate renegotiation mapping",
		  test_rate_renegotiation_mapping },
		{ "V.92 CPt mapping", test_v92_cpt_mapping },
		{ "V.92 CPu mapping", test_v92_cpu_mapping },
		{ "generated inverse mapping",
		  test_generated_inverse_mapping },
		{ "inverse rate boundaries",
		  test_inverse_rate_boundaries },
		{ "inverse rejection", test_inverse_rejection },
		{ "rate boundaries", test_rate_boundaries },
		{ "capacity and codec fallback",
		  test_capacity_and_codec_fallback },
		{ "shaping coefficient boundaries",
		  test_shaping_coefficient_boundaries },
		{ "contracts", test_contracts },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		printf("[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			printf("[       OK ] %s\n", tests[index].name);
		else {
			printf("[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0 ? 0 : 1;
}
