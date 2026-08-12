// SPDX-License-Identifier: BSD-3-Clause

#include <v90_pcm_mapping.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

static void add_ucode(
	struct v90_cp_constellation *constellation, uint8_t ucode)
{
	constellation->ucode_mask[ucode / 8u] |=
		(uint8_t)(1u << (ucode % 8u));
}

static void fill_constellation(
	struct v90_cp_constellation *constellation,
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

static bool test_training_mapping(void)
{
	struct v90_cp_parameters source =
		parameters(V90_CP_TRAINING, 13, 1, 8, true);
	struct v90_pcm_mapping mapping;
	uint8_t bits[16];
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
	      memcmp(labels, expected_labels, sizeof(labels)) == 0,
	      "mixed-radix mapping changed for 0xa55a");
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
	changed.frame_bits++;
	CHECK(!v90_pcm_mapping_is_valid(&changed) &&
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
