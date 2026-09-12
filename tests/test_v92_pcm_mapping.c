// SPDX-License-Identifier: BSD-3-Clause

#include <v92_pcm_mapping.h>

#include "test.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

static struct v92_cpd_parameters parameters(uint8_t data_rate_number)
{
	struct v92_cpd_parameters value = {
		.four_times_gain_q0_16 = 165u,
		.data_rate_number = data_rate_number,
		.trellis = data_rate_number == 13u ?
			V92_CPD_TRELLIS_32_STATE :
			V92_CPD_TRELLIS_16_STATE,
		.modulus_present = true,
		.filters_present = true,
		.constellations_present = true,
	};
	unsigned int bits = 2u *
		(data_rate_number + V92_PCM_MAPPING_RATE_INDEX_BASE);
	uint8_t point_count = 0u;

	value.filter[V92_CPD_PREFILTER_FEED_FORWARD].count = 4u;
	value.filter[V92_CPD_PREFILTER_FEED_FORWARD].coefficient[0] =
		INT16_MAX;
	for (size_t symbol = 0u;
	     symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
		unsigned int exponent = bits / V92_PCM_MAPPING_FRAME_SYMBOLS;
		uint8_t required;

		if (symbol < bits % V92_PCM_MAPPING_FRAME_SYMBOLS)
			exponent++;
		value.modulus[symbol] = (uint8_t)(1u << exponent);
		required = symbol % 4u == 3u ?
			value.modulus[symbol] :
			(uint8_t)((value.modulus[symbol] + 1u) / 2u);
		if (point_count < required)
			point_count = required;
	}
	if (data_rate_number == 13u)
		point_count = 64u;
	value.constellation[0].count = point_count;
	for (uint8_t index = 0u; index < point_count; ++index)
		value.constellation[0].positive_linear[index] =
			(int16_t)((2u * index + 1u) * 256u);
	return value;
}

static void generated_bits(
	uint8_t *bits, size_t bit_count, unsigned int frame)
{
	uint32_t state = UINT32_C(0x9e3779b9) ^ frame;

	for (size_t bit = 0u; bit < bit_count; ++bit) {
		state = state * UINT32_C(1664525) + UINT32_C(1013904223);
		bits[bit] = (uint8_t)(state >> 31);
	}
}

static bool test_rate_matrix(void)
{
	CHECK(v92_pcm_mapping_data_bits(0u) == 0u &&
	      v92_pcm_mapping_data_bits(1u) == 36u &&
	      v92_pcm_mapping_data_bits(19u) == 72u &&
	      v92_pcm_mapping_data_bits(20u) == 0u,
	      "rate geometry boundaries changed");
	for (unsigned int law = G711_LAW_PCMU;
	     law <= G711_LAW_PCMA; ++law) {
		for (uint8_t rate = 1u;
		     rate <= V92_CPD_MAX_DATA_RATE_NUMBER; ++rate) {
			struct v92_cpd_parameters cpd = parameters(rate);
			struct v92_pcm_mapping mapping;
			struct v92_pcm_mapping copy;
			uint8_t encoded_state = 0u;
			uint8_t decoded_state = 0u;
			uint8_t owned_state = 0u;

			CHECK(v92_pcm_mapping_init(
				      &mapping, (enum g711_law)law, &cpd) ==
				      V92_PCM_MAPPING_OK &&
			      v92_pcm_mapping_is_valid(&mapping) &&
			      mapping.data_bits_per_frame ==
				      2u * (rate +
					    V92_PCM_MAPPING_RATE_INDEX_BASE),
			      "rate %u law %u did not initialize", rate, law);
			copy = mapping;
			CHECK(v92_pcm_mapping_equal(&mapping, &copy),
			      "rate %u law %u did not compare equal", rate, law);
			for (unsigned int frame = 0u; frame < 8u; ++frame) {
				uint8_t input[V92_PCM_MAPPING_MAX_BITS];
				uint8_t output[V92_PCM_MAPPING_MAX_BITS] = { 0 };
				uint8_t owned_output[V92_PCM_MAPPING_MAX_BITS] = { 0 };
				uint8_t digits[V92_PCM_MAPPING_FRAME_SYMBOLS];
				uint8_t sign = 0xffu;

				generated_bits(
					input, mapping.data_bits_per_frame,
					frame + rate * 17u + law * 131u);
				CHECK(v92_pcm_mapping_encode(
					      &mapping, input,
					      mapping.data_bits_per_frame,
					      &encoded_state, digits, &sign) ==
					      V92_PCM_MAPPING_OK &&
				      sign == encoded_state &&
				      v92_pcm_mapping_decode(
					      &mapping, digits, &decoded_state,
					      output,
					      mapping.data_bits_per_frame) ==
					      V92_PCM_MAPPING_OK &&
				      decoded_state == encoded_state &&
				      memcmp(input, output,
					     mapping.data_bits_per_frame) == 0,
				      "rate %u law %u frame %u changed bits",
				      rate, law, frame);
				CHECK(v92_pcm_mapping_decode_owned(
					      &mapping, digits, &owned_state, owned_output,
					      mapping.data_bits_per_frame) == V92_PCM_MAPPING_OK &&
				      owned_state == decoded_state &&
				      memcmp(owned_output, output, sizeof(output)) == 0,
				      "rate %u law %u frame %u changed owned decode",
				      rate, law, frame);
			}
		}
	}
	return true;
}

static bool test_phase_constellations(void)
{
	struct v92_cpd_parameters cpd = parameters(4u);
	struct v92_pcm_mapping mapping;

	for (uint8_t index = 0u; index < 2u; ++index) {
		cpd.constellation[index].count = 16u;
		for (uint8_t point = 0u; point < 16u; ++point)
			cpd.constellation[index].positive_linear[point] =
				(int16_t)(1000u * (index + 1u) + point + 1u);
	}
	for (size_t phase = 0u; phase < V92_PCM_MAPPING_PHASES; ++phase)
		cpd.frame_constellation[phase] = (uint8_t)(phase & 1u);
	CHECK(v92_pcm_mapping_init(&mapping, G711_LAW_PCMU, &cpd) ==
		      V92_PCM_MAPPING_OK,
	      "alternating phase mapping did not initialize");
	{
		struct v92_pcm_mapping changed = mapping;

		changed.cpd.acknowledges_cpu = true;
		CHECK(v92_pcm_mapping_is_valid(&changed) &&
		      !v92_pcm_mapping_equal(&mapping, &changed),
		      "mapping equality ignored a CPd selection field");
	}
	for (size_t symbol = 0u;
	     symbol < V92_PCM_MAPPING_FRAME_SYMBOLS; ++symbol) {
		int16_t level = 0;
		int16_t expected =
			(int16_t)(1000u * ((symbol % 2u) + 1u) + 4u);

		CHECK(v92_pcm_mapping_positive_level(
			      &mapping, symbol, 3u, &level) ==
			      V92_PCM_MAPPING_OK &&
		      level == expected,
		      "symbol %zu selected level %d instead of %d",
		      symbol, level, expected);
	}
	return true;
}

static bool test_rejection_and_transactionality(void)
{
	struct v92_cpd_parameters valid = parameters(19u);
	struct v92_cpd_parameters changed;
	struct v92_pcm_mapping mapping;
	struct v92_pcm_mapping before;
	uint8_t bits[V92_PCM_MAPPING_MAX_BITS] = { 0 };
	uint8_t digits[V92_PCM_MAPPING_FRAME_SYMBOLS];
	uint8_t digits_before[V92_PCM_MAPPING_FRAME_SYMBOLS];
	uint8_t decoded[V92_PCM_MAPPING_MAX_BITS];
	uint8_t decoded_before[V92_PCM_MAPPING_MAX_BITS];
	uint8_t state = 0u;
	uint8_t sign = 0xa5u;
	int16_t level = 1234;

	memset(&mapping, 0x5a, sizeof(mapping));
	before = mapping;
	changed = valid;
	changed.modulus_present = false;
	memset(changed.modulus, 0, sizeof(changed.modulus));
	CHECK(v92_pcm_mapping_init(&mapping, G711_LAW_PCMU, &changed) ==
		      V92_PCM_MAPPING_INVALID_PARAMETERS &&
	      memcmp(&mapping, &before, sizeof(mapping)) == 0,
	      "missing modulus section changed the mapping");
	changed = valid;
	changed.filters_present = false;
	memset(changed.filter, 0, sizeof(changed.filter));
	CHECK(v92_pcm_mapping_init(&mapping, G711_LAW_PCMU, &changed) ==
		      V92_PCM_MAPPING_INVALID_PARAMETERS &&
	      memcmp(&mapping, &before, sizeof(mapping)) == 0,
	      "missing filter section changed the mapping");
	changed = valid;
	changed.constellations_present = false;
	memset(changed.constellation, 0, sizeof(changed.constellation));
	CHECK(v92_pcm_mapping_init(&mapping, G711_LAW_PCMU, &changed) ==
		      V92_PCM_MAPPING_INVALID_PARAMETERS &&
	      memcmp(&mapping, &before, sizeof(mapping)) == 0,
	      "missing constellation section changed the mapping");
	changed = valid;
	changed.data_rate_number = 0u;
	CHECK(v92_pcm_mapping_init(&mapping, G711_LAW_PCMU, &changed) ==
		      V92_PCM_MAPPING_INVALID_PARAMETERS,
	      "cleardown initialized a data mapping");
	changed = valid;
	changed.modulus[3] = 65u;
	CHECK(v92_pcm_mapping_init(&mapping, G711_LAW_PCMU, &changed) ==
		      V92_PCM_MAPPING_INVALID_PARAMETERS,
	      "out-of-constellation modulus initialized");
	changed = valid;
	changed.modulus[0] = 32u;
	CHECK(v92_pcm_mapping_init(&mapping, G711_LAW_PCMU, &changed) ==
		      V92_PCM_MAPPING_INVALID_PARAMETERS,
	      "under-capacity mapping initialized");
	CHECK(v92_pcm_mapping_init(&mapping, G711_LAW_PCMU, &valid) ==
		      V92_PCM_MAPPING_OK,
	      "valid mapping did not initialize for contract tests");
	memset(digits, 0x6b, sizeof(digits));
	memcpy(digits_before, digits, sizeof(digits));
	CHECK(v92_pcm_mapping_encode(
		      &mapping, bits, mapping.data_bits_per_frame - 1u,
		      &state, digits, &sign) == V92_PCM_MAPPING_OUT_OF_RANGE &&
	      state == 0u && sign == 0xa5u &&
	      memcmp(digits, digits_before, sizeof(digits)) == 0,
	      "rejected encode changed state or digits");
	state = 2u;
	CHECK(v92_pcm_mapping_encode(
		      &mapping, bits, mapping.data_bits_per_frame,
		      &state, digits, &sign) == V92_PCM_MAPPING_OUT_OF_RANGE &&
	      state == 2u && sign == 0xa5u &&
	      memcmp(digits, digits_before, sizeof(digits)) == 0,
	      "invalid differential state changed outputs");
	state = 0u;
	memset(digits, 0xff, sizeof(digits));
	memset(decoded, 0x3c, sizeof(decoded));
	memcpy(decoded_before, decoded, sizeof(decoded));
	CHECK(v92_pcm_mapping_decode(
		      &mapping, digits, &state, decoded,
		      mapping.data_bits_per_frame) ==
		      V92_PCM_MAPPING_OUT_OF_RANGE &&
	      state == 0u &&
	      memcmp(decoded, decoded_before, sizeof(decoded)) == 0,
	      "rejected decode changed state or bits");
	CHECK(v92_pcm_mapping_positive_level(
		      &mapping, V92_PCM_MAPPING_FRAME_SYMBOLS, 0u,
		      &level) == V92_PCM_MAPPING_OUT_OF_RANGE &&
	      level == 1234,
	      "rejected level lookup changed its output");
	CHECK(v92_pcm_mapping_init(NULL, G711_LAW_PCMU, &valid) ==
		      V92_PCM_MAPPING_INVALID_ARGUMENT &&
	      v92_pcm_mapping_init(&mapping, G711_LAW_PCMU, NULL) ==
		      V92_PCM_MAPPING_INVALID_ARGUMENT &&
	      v92_pcm_mapping_init(
		      &mapping, (enum g711_law)99, &valid) ==
		      V92_PCM_MAPPING_INVALID_PARAMETERS &&
	      !v92_pcm_mapping_is_valid(NULL) &&
	      !v92_pcm_mapping_equal(NULL, &mapping),
	      "invalid API arguments were accepted");
	return true;
}

static bool test_decode_contracts(void)
{
	struct v92_cpd_parameters cpd = parameters(1u);
	struct v92_pcm_mapping mapping;
	/* More capacity than 2^K provides genuine spare codewords. */
	cpd.modulus[0]++;
	CHECK(v92_pcm_mapping_init(&mapping, G711_LAW_PCMU, &cpd) ==
	      V92_PCM_MAPPING_OK, "spare mapping did not initialize");
	enum v92_pcm_mapping_result (*const decoders[])(
		const struct v92_pcm_mapping *, const uint8_t *, uint8_t *,
		uint8_t *, size_t) = {
		v92_pcm_mapping_decode, v92_pcm_mapping_decode_owned,
	};
	for (size_t decoder = 0u; decoder < ARRAY_LENGTH(decoders); decoder++) {
		for (unsigned scenario = 0u; scenario < 5u; scenario++) {
			uint8_t digits[V92_PCM_MAPPING_FRAME_SYMBOLS] = { 0 };
			uint8_t output[V92_PCM_MAPPING_MAX_BITS], before[sizeof(output)];
			uint8_t state = scenario == 1u ? 2u : 0u;
			uint8_t original_state = state;
			size_t count = mapping.data_bits_per_frame;

			if (scenario == 0u) count--;
			if (scenario == 2u) digits[0] = mapping.cpd.modulus[0];
			if (scenario == 3u)
				for (size_t i = 0u; i < ARRAY_LENGTH(digits); i++)
					digits[i] = mapping.cpd.modulus[i] - 1u;
			if (scenario == 4u) count = SIZE_MAX;
			memset(output, 0x5a, sizeof(output));
			memcpy(before, output, sizeof(output));
			CHECK(decoders[decoder](&mapping, digits, &state, output, count) ==
			      V92_PCM_MAPPING_OUT_OF_RANGE && state == original_state &&
			      memcmp(output, before, sizeof(output)) == 0,
			      "decoder %zu scenario %u changed rejected output", decoder, scenario);
			CHECK(decoders[decoder](NULL, digits, &state, output, count) ==
				      V92_PCM_MAPPING_INVALID_ARGUMENT &&
			      decoders[decoder](&mapping, NULL, &state, output, count) ==
				      V92_PCM_MAPPING_INVALID_ARGUMENT &&
			      decoders[decoder](&mapping, digits, NULL, output, count) ==
				      V92_PCM_MAPPING_INVALID_ARGUMENT &&
			      decoders[decoder](&mapping, digits, &state, NULL, count) ==
				      V92_PCM_MAPPING_INVALID_ARGUMENT &&
			      state == original_state && memcmp(output, before, sizeof(output)) == 0,
			      "decoder %zu changed rejected null output", decoder);
		}
	}
	/* The checked entry must still reject invalid immutable geometry. */
	uint8_t digits[V92_PCM_MAPPING_FRAME_SYMBOLS] = { 0 };
	uint8_t state = 0u, output[V92_PCM_MAPPING_MAX_BITS], before[sizeof(output)];
	memset(output, 0x5a, sizeof(output));
	memcpy(before, output, sizeof(output));
	mapping.cpd.filters_present = false;
	CHECK(v92_pcm_mapping_decode(&mapping, digits, &state, output,
		mapping.data_bits_per_frame) == V92_PCM_MAPPING_INVALID_ARGUMENT &&
		state == 0u && memcmp(output, before, sizeof(output)) == 0,
		"checked decoder accepted invalid mapping");
	return true;
}

static bool test_result_names(void)
{
	CHECK(strcmp(v92_pcm_mapping_result_name(V92_PCM_MAPPING_OK), "ok") ==
		      0 &&
	      strcmp(v92_pcm_mapping_result_name(
			     V92_PCM_MAPPING_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(v92_pcm_mapping_result_name(
			     V92_PCM_MAPPING_INVALID_PARAMETERS),
		     "invalid parameters") == 0 &&
	      strcmp(v92_pcm_mapping_result_name(
			     V92_PCM_MAPPING_OUT_OF_RANGE),
		     "out of range") == 0 &&
	      strcmp(v92_pcm_mapping_result_name(
			     (enum v92_pcm_mapping_result)99),
		     "unknown") == 0,
	      "mapping result names changed");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "rate matrix", test_rate_matrix },
		{ "phase constellations", test_phase_constellations },
		{ "rejection and transactionality",
		  test_rejection_and_transactionality },
		{ "result names", test_result_names },
		{ "decode contracts", test_decode_contracts },
	};
	unsigned int failures = 0u;

	for (size_t index = 0u; index < ARRAY_LENGTH(tests); ++index) {
		printf("[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			printf("[       OK ] %s\n", tests[index].name);
		else {
			printf("[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0u ? 0 : 1;
}
