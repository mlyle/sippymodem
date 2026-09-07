// SPDX-License-Identifier: BSD-3-Clause

#include <v90_pcm_frame_encoder.h>

#include "test.h"

#include <float.h>
#include <stdint.h>
#include <string.h>

struct component_encoder {
	struct v90_pcm_mapping mapping;
	struct v90_sign_encoder signs;
	struct v90_spectral_shaper shaper;
};

static void add_ucode(
	struct pcm_constellation *constellation, uint8_t ucode)
{
	constellation->ucode_mask[ucode / 8u] |=
		(uint8_t)(1u << (ucode % 8u));
}

static bool make_mapping(
	struct v90_pcm_mapping *mapping, enum v90_cp_kind kind,
	enum g711_law law, uint8_t redundancy, uint8_t lookahead)
{
	uint8_t size = kind == V90_CP_TRAINING ? 4 : 8;
	struct v90_cp_parameters parameters = {
		.kind = kind,
		.law = law,
		.shaping_a1_q1_6 = 17,
		.shaping_a2_q1_6 = -23,
		.shaping_b1_q1_6 = 39,
		.shaping_b2_q1_6 = -7,
		.selected_rate_index = kind == V90_CP_TRAINING ? 4 : 1,
		.sign_redundancy = redundancy,
		.spectral_lookahead = lookahead,
		.constellation_count = 1,
	};

	for (uint8_t ordinal = 0; ordinal < size; ++ordinal)
		add_ucode(
			&parameters.transmit_constellations[0],
			(uint8_t)(10 + ordinal));
	return v90_pcm_mapping_init(mapping, &parameters) ==
	       V90_PCM_MAPPING_OK;
}

static bool symbol_equal(
	const struct v90_pcm_symbol *left,
	const struct v90_pcm_symbol *right)
{
	return left->ucode == right->ucode &&
	       left->positive == right->positive &&
	       left->octet == right->octet &&
	       left->sample == right->sample;
}

static bool output_equal(
	const struct v90_pcm_frame_output *left,
	const struct v90_pcm_frame_output *right)
{
	if (left->symbol_count != right->symbol_count ||
	    left->rule_count != right->rule_count)
		return false;
	for (size_t index = 0;
	     index < V90_CP_FRAME_INTERVALS; ++index) {
		if (!symbol_equal(&left->symbols[index],
				  &right->symbols[index]))
			return false;
	}
	for (size_t index = 0; index < 3; ++index) {
		if (left->rules[index] != right->rules[index])
			return false;
	}
	return true;
}

static bool component_encoder_init(
	struct component_encoder *encoder,
	const struct v90_pcm_mapping *mapping)
{
	struct component_encoder initialized = {
		.mapping = *mapping,
	};

	if (v90_sign_encoder_init(
		    &initialized.signs, mapping->sign_redundancy) !=
	    V90_SIGN_ENCODER_OK)
		return false;
	if (mapping->sign_redundancy != 0 &&
	    v90_spectral_shaper_init(&initialized.shaper, mapping) !=
		    V90_SPECTRAL_SHAPER_OK)
		return false;
	*encoder = initialized;
	return true;
}

static bool component_ucodes(
	const struct v90_pcm_mapping *mapping, const uint8_t *bits,
	uint8_t *ucodes)
{
	uint8_t labels[V90_CP_FRAME_INTERVALS];

	if (v90_pcm_mapping_encode_modulus(
		    mapping, bits + mapping->sign_bits,
		    mapping->modulus_bits, labels) != V90_PCM_MAPPING_OK)
		return false;
	for (size_t interval = 0;
	     interval < V90_CP_FRAME_INTERVALS; ++interval) {
		if (v90_pcm_mapping_ucode(
			    mapping, V90_PCM_MAPPING_TRANSMIT, interval,
			    labels[interval], &ucodes[interval]) !=
		    V90_PCM_MAPPING_OK)
			return false;
	}
	return true;
}

static enum v90_pcm_frame_encoder_result component_process(
	struct component_encoder *encoder, const uint8_t *bits,
	struct v90_pcm_frame_output *output)
{
	struct v90_pcm_frame_output generated = { 0 };
	uint8_t signs[V90_CP_FRAME_INTERVALS];
	uint8_t ucodes[V90_CP_FRAME_INTERVALS];
	size_t frame_count = encoder->mapping.sign_redundancy;

	if (v90_sign_encoder_encode(
		    &encoder->signs, bits, encoder->mapping.sign_bits,
		    signs) != V90_SIGN_ENCODER_OK ||
	    !component_ucodes(&encoder->mapping, bits, ucodes))
		return V90_PCM_FRAME_ENCODER_INTERNAL_ERROR;
	if (frame_count == 0) {
		generated.symbol_count = V90_CP_FRAME_INTERVALS;
		for (size_t interval = 0;
		     interval < V90_CP_FRAME_INTERVALS; ++interval) {
			if (!v90_pcm_symbol_make(
				    &generated.symbols[interval],
				    encoder->mapping.law, ucodes[interval],
				    signs[interval] != 0))
				return V90_PCM_FRAME_ENCODER_INTERNAL_ERROR;
		}
		*output = generated;
		return V90_PCM_FRAME_ENCODER_SYMBOLS_READY;
	}
	for (size_t frame = 0; frame < frame_count; ++frame) {
		struct v90_spectral_output shaped;
		size_t frame_length = V90_CP_FRAME_INTERVALS / frame_count;
		enum v90_spectral_shaper_result result =
			v90_spectral_shaper_push(
				&encoder->shaper,
				ucodes + frame * frame_length,
				signs + frame * frame_length,
				frame_length, &shaped);

		if (result == V90_SPECTRAL_SHAPER_BUFFERING)
			continue;
		if (result != V90_SPECTRAL_SHAPER_FRAME_READY)
			return V90_PCM_FRAME_ENCODER_INTERNAL_ERROR;
		memcpy(&generated.symbols[generated.symbol_count],
		       shaped.symbols,
		       shaped.symbol_count * sizeof(shaped.symbols[0]));
		generated.symbol_count += shaped.symbol_count;
		generated.rules[generated.rule_count++] = shaped.rule;
	}
	if (generated.symbol_count == 0)
		return V90_PCM_FRAME_ENCODER_BUFFERING;
	*output = generated;
	return V90_PCM_FRAME_ENCODER_SYMBOLS_READY;
}

static enum v90_pcm_frame_encoder_result component_finish(
	struct component_encoder *encoder,
	struct v90_pcm_frame_output *output)
{
	struct v90_spectral_output shaped;
	struct v90_pcm_frame_output generated = { 0 };
	enum v90_spectral_shaper_result result;

	if (encoder->mapping.sign_redundancy == 0)
		return V90_PCM_FRAME_ENCODER_COMPLETE;
	result = v90_spectral_shaper_finish(&encoder->shaper, &shaped);
	if (result == V90_SPECTRAL_SHAPER_COMPLETE)
		return V90_PCM_FRAME_ENCODER_COMPLETE;
	if (result != V90_SPECTRAL_SHAPER_FRAME_READY)
		return V90_PCM_FRAME_ENCODER_INTERNAL_ERROR;
	memcpy(generated.symbols, shaped.symbols,
	       shaped.symbol_count * sizeof(shaped.symbols[0]));
	generated.symbol_count = shaped.symbol_count;
	generated.rules[0] = shaped.rule;
	generated.rule_count = 1;
	*output = generated;
	return V90_PCM_FRAME_ENCODER_SYMBOLS_READY;
}

static bool test_unshaped_exact_frame(void)
{
	struct v90_pcm_mapping mapping;
	struct v90_pcm_frame_encoder encoder;
	struct v90_pcm_frame_output output;
	uint8_t bits[42] = { 1, 0, 1, 1, 0, 1 };
	uint32_t modulus_value = 0x5a5au;
	uint8_t previous_sign = 0;

	CHECK(make_mapping(
		      &mapping, V90_CP_DATA, G711_LAW_PCMU, 0, 0) &&
	      mapping.frame_bits == 21 && mapping.sign_bits == 6 &&
	      mapping.modulus_bits == 15 &&
	      v90_pcm_frame_encoder_init(&encoder, &mapping) ==
		      V90_PCM_FRAME_ENCODER_OK,
	      "unshaped fixture did not initialize");
	for (size_t bit = 0; bit < mapping.modulus_bits; ++bit)
		bits[mapping.sign_bits + bit] =
			(uint8_t)((modulus_value >> bit) & 1u);
	CHECK(v90_pcm_frame_encoder_process(
		      &encoder, bits, mapping.frame_bits, &output) ==
		      V90_PCM_FRAME_ENCODER_SYMBOLS_READY &&
	      output.symbol_count == 6 && output.rule_count == 0,
	      "unshaped frame did not produce six symbols");
	for (size_t interval = 0; interval < 6; ++interval) {
		uint8_t label = (uint8_t)(modulus_value & 7u);
		uint8_t ucode = (uint8_t)(17u - label);

		previous_sign ^= bits[interval];
		CHECK(output.symbols[interval].ucode == ucode &&
		      output.symbols[interval].positive ==
			      (previous_sign != 0),
		      "unshaped interval %zu parsed bits incorrectly",
		      interval);
		modulus_value >>= 3;
	}
	return true;
}

static void generate_bits(
	uint8_t *bits, size_t count, size_t frame,
	uint8_t redundancy)
{
	for (size_t bit = 0; bit < count; ++bit)
		bits[bit] = (uint8_t)(
			(frame * 13u + bit * 7u + redundancy * 3u +
			 (frame >> (bit % 3u))) & 1u);
}

static bool check_distinct_codec_plane(
	enum v90_cp_kind kind, enum g711_law law,
	uint8_t redundancy, uint8_t lookahead)
{
	struct v90_pcm_mapping mapping;
	struct v90_pcm_frame_encoder transmit_only;
	struct v90_pcm_frame_encoder with_codec;
	size_t emitted = 0;
	const size_t frames = 8;
	bool complete = false;

	CHECK(make_mapping(&mapping, kind, law, redundancy, lookahead) &&
	      v90_pcm_frame_encoder_init(&transmit_only, &mapping) ==
		      V90_PCM_FRAME_ENCODER_OK,
	      "transmit-plane fixture did not initialize");
	/* CP may describe different levels at the peer's D/A converter. They
	 * are not the Ucodes to transmit. V.90 5.4.5.6 also bases shaping on
	 * the transmitted codes, so neither the signs nor octets may change. */
	memset(mapping.codec, 0, sizeof(mapping.codec));
	for (size_t interval = 0; interval < V90_CP_FRAME_INTERVALS; ++interval)
		for (uint8_t label = 0; label < mapping.modulus[interval]; ++label)
			add_ucode(&mapping.codec[interval],
				  (uint8_t)(64u + interval * 8u + label));
	mapping.codec_plane_differs = true;
	CHECK(v90_pcm_mapping_is_valid(&mapping) &&
	      v90_pcm_frame_encoder_init(&with_codec, &mapping) ==
		      V90_PCM_FRAME_ENCODER_OK,
	      "distinct codec-plane fixture did not initialize");
	for (size_t frame = 0; frame < frames + lookahead + 1u; ++frame) {
		struct v90_pcm_frame_output expected, actual;
		enum v90_pcm_frame_encoder_result expected_result, actual_result;

		if (frame < frames) {
			uint8_t bits[V90_PCM_FRAME_MAX_BITS];

			generate_bits(bits, mapping.frame_bits, frame, redundancy);
			expected_result = v90_pcm_frame_encoder_process(
				&transmit_only, bits, mapping.frame_bits, &expected);
			actual_result = v90_pcm_frame_encoder_process(
				&with_codec, bits, mapping.frame_bits, &actual);
		} else {
			expected_result = v90_pcm_frame_encoder_finish(
				&transmit_only, &expected);
			actual_result = v90_pcm_frame_encoder_finish(&with_codec, &actual);
		}
		CHECK(actual_result == expected_result,
		      "codec-plane metadata changed encoder progress");
		if (actual_result == V90_PCM_FRAME_ENCODER_COMPLETE) {
			complete = true;
			break;
		}
		if (actual_result == V90_PCM_FRAME_ENCODER_BUFFERING)
			continue;
		CHECK(actual_result == V90_PCM_FRAME_ENCODER_SYMBOLS_READY &&
		      output_equal(&actual, &expected),
		      "codec-plane metadata changed transmitted signs or octets");
		for (size_t symbol = 0; symbol < actual.symbol_count; ++symbol)
			CHECK(actual.symbols[symbol].ucode >= 10u &&
			      actual.symbols[symbol].ucode < 10u + mapping.modulus[0],
			      "encoder transmitted a codec-output level");
		emitted += actual.symbol_count;
	}
	CHECK(complete && emitted == frames * V90_CP_FRAME_INTERVALS &&
	      v90_pcm_frame_encoder_is_valid(&with_codec),
	      "distinct codec-plane encoding lost buffered symbols");
	return true;
}

static bool test_distinct_codec_plane(void)
{
	for (enum v90_cp_kind kind = V90_CP_TRAINING; kind <= V90_CP_DATA; ++kind)
		for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
			for (uint8_t redundancy = 0; redundancy <= 3; ++redundancy)
				for (uint8_t lookahead = 0; lookahead <= 3; ++lookahead)
					CHECK(check_distinct_codec_plane(
						      kind, law, redundancy, lookahead),
					      "codec-plane isolation failed for %u/%u/%u/%u",
					      kind, law, redundancy, lookahead);
	return true;
}

static bool test_shaped_pipeline(void)
{
	uint8_t first_ucodes[2][3][4][V90_CP_FRAME_INTERVALS] = { 0 };

	for (unsigned int law_index = 0; law_index < 2; ++law_index) {
		enum g711_law law = law_index == 0 ?
				    G711_LAW_PCMU : G711_LAW_PCMA;

		for (uint8_t redundancy = 1; redundancy <= 3; ++redundancy) {
			for (uint8_t lookahead = 0; lookahead <= 3; ++lookahead) {
				struct v90_pcm_mapping mapping;
				struct v90_pcm_frame_encoder encoder;
				struct component_encoder components;
				size_t total_symbols = 0;
				size_t total_rules = 0;

				CHECK(make_mapping(
					      &mapping, V90_CP_TRAINING, law,
					      redundancy, lookahead) &&
				      v90_pcm_frame_encoder_init(
					      &encoder, &mapping) ==
					      V90_PCM_FRAME_ENCODER_OK &&
				      component_encoder_init(
					      &components, &mapping),
				      "pipeline %u/%u/%u did not initialize",
				      law_index, redundancy, lookahead);
				for (size_t frame = 0; frame < 16; ++frame) {
					uint8_t bits[42];
					struct v90_pcm_frame_output actual;
					struct v90_pcm_frame_output expected;
					enum v90_pcm_frame_encoder_result actual_result;
					enum v90_pcm_frame_encoder_result expected_result;

					generate_bits(
						bits, mapping.frame_bits, frame,
						redundancy);
					actual_result = v90_pcm_frame_encoder_process(
						&encoder, bits, mapping.frame_bits,
						&actual);
					expected_result = component_process(
						&components, bits, &expected);
					CHECK(actual_result == expected_result,
					      "pipeline %u/%u/%u frame %zu returned %s",
					      law_index, redundancy, lookahead,
					      frame,
					      v90_pcm_frame_encoder_result_name(
						      actual_result));
					if (actual_result !=
					    V90_PCM_FRAME_ENCODER_SYMBOLS_READY)
						continue;
					CHECK(output_equal(&actual, &expected),
					      "pipeline %u/%u/%u frame %zu reordered symbols",
					      law_index, redundancy, lookahead,
					      frame);
					for (size_t symbol = 0;
					     symbol < actual.symbol_count; ++symbol) {
						if (total_symbols < 6)
							first_ucodes[law_index]
								[redundancy - 1u]
								[lookahead]
								[total_symbols] =
								actual.symbols[symbol].ucode;
						total_symbols++;
					}
					total_rules += actual.rule_count;
				}
				for (;;) {
					struct v90_pcm_frame_output actual;
					struct v90_pcm_frame_output expected;
					enum v90_pcm_frame_encoder_result actual_result =
						v90_pcm_frame_encoder_finish(
							&encoder, &actual);
					enum v90_pcm_frame_encoder_result expected_result =
						component_finish(
							&components, &expected);

					CHECK(actual_result == expected_result,
					      "pipeline %u/%u/%u tail result differed",
					      law_index, redundancy, lookahead);
					if (actual_result ==
					    V90_PCM_FRAME_ENCODER_COMPLETE)
						break;
					CHECK(output_equal(&actual, &expected),
					      "pipeline %u/%u/%u tail reordered symbols",
					      law_index, redundancy, lookahead);
					for (size_t symbol = 0;
					     symbol < actual.symbol_count; ++symbol) {
						if (total_symbols < 6)
							first_ucodes[law_index]
								[redundancy - 1u]
								[lookahead]
								[total_symbols] =
								actual.symbols[symbol].ucode;
						total_symbols++;
					}
					total_rules += actual.rule_count;
				}
				CHECK(total_symbols == 16u * 6u &&
				      total_rules == 16u * redundancy &&
				      v90_pcm_frame_encoder_is_valid(&encoder),
				      "pipeline %u/%u/%u lost symbols or rules",
				      law_index, redundancy, lookahead);
			}
			for (uint8_t lookahead = 1; lookahead <= 3; ++lookahead) {
				CHECK(memcmp(
					      first_ucodes[law_index][redundancy - 1u][0],
					      first_ucodes[law_index][redundancy - 1u]
						      [lookahead],
					      V90_CP_FRAME_INTERVALS) == 0,
				      "first magnitudes changed with look-ahead %u/%u/%u",
				      law_index, redundancy, lookahead);
			}
		}
	}
	return true;
}

static bool test_contracts(void)
{
	struct v90_pcm_mapping mapping;
	struct v90_pcm_frame_encoder encoder;
	struct v90_pcm_frame_encoder saved_encoder;
	struct v90_pcm_frame_output output;
	struct v90_pcm_frame_output saved_output;
	uint8_t bits[42] = { 0 };

	CHECK(make_mapping(
		      &mapping, V90_CP_TRAINING, G711_LAW_PCMU, 1, 3) &&
	      v90_pcm_frame_encoder_init(&encoder, &mapping) ==
		      V90_PCM_FRAME_ENCODER_OK,
	      "contract fixture did not initialize");
	memset(&output, 0xa5, sizeof(output));
	saved_output = output;
	saved_encoder = encoder;
	bits[7] = 2;
	CHECK(v90_pcm_frame_encoder_process(
		      &encoder, bits, mapping.frame_bits, &output) ==
		      V90_PCM_FRAME_ENCODER_INVALID_BITS &&
	      memcmp(&encoder, &saved_encoder, sizeof(encoder)) == 0 &&
	      memcmp(&output, &saved_output, sizeof(output)) == 0,
	      "non-binary input changed state or output");
	bits[7] = 0;
	CHECK(v90_pcm_frame_encoder_process(
		      &encoder, bits, mapping.frame_bits - 1u, &output) ==
		      V90_PCM_FRAME_ENCODER_INVALID_BITS &&
	      v90_pcm_frame_encoder_process(
		      NULL, bits, mapping.frame_bits, &output) ==
		      V90_PCM_FRAME_ENCODER_INVALID_ARGUMENT,
	      "invalid process contract changed");
	CHECK(v90_pcm_frame_encoder_process(
		      &encoder, bits, mapping.frame_bits, &output) ==
		      V90_PCM_FRAME_ENCODER_BUFFERING &&
	      memcmp(&output, &saved_output, sizeof(output)) == 0,
	      "buffered frame changed output");
	CHECK(v90_pcm_frame_encoder_finish(&encoder, &output) ==
		      V90_PCM_FRAME_ENCODER_SYMBOLS_READY &&
	      v90_pcm_frame_encoder_process(
		      &encoder, bits, mapping.frame_bits, &output) ==
		      V90_PCM_FRAME_ENCODER_INVALID_STATE,
	      "finish did not latch the encoder lifecycle");
	while (v90_pcm_frame_encoder_finish(&encoder, &output) ==
	       V90_PCM_FRAME_ENCODER_SYMBOLS_READY)
		;
	memset(&output, 0xa5, sizeof(output));
	saved_output = output;
	CHECK(v90_pcm_frame_encoder_finish(&encoder, &output) ==
		      V90_PCM_FRAME_ENCODER_COMPLETE &&
	      memcmp(&output, &saved_output, sizeof(output)) == 0,
	      "completed finish changed output");

	CHECK(v90_pcm_frame_encoder_init(&encoder, &mapping) ==
		      V90_PCM_FRAME_ENCODER_OK,
	      "numeric fixture did not initialize");
	encoder.mapping.shaping_a2_q1_6 = 64;
	encoder.shaper.shaping_a2_q1_6 = 64;
	encoder.shaper.previous_v = DBL_MAX;
	encoder.mapping.spectral_lookahead = 0;
	encoder.shaper.lookahead = 0;
	saved_encoder = encoder;
	CHECK(v90_pcm_frame_encoder_is_valid(&encoder) &&
	      v90_pcm_frame_encoder_process(
		      &encoder, bits, mapping.frame_bits, &output) ==
		      V90_PCM_FRAME_ENCODER_NUMERIC_FAILURE &&
	      memcmp(&encoder, &saved_encoder, sizeof(encoder)) == 0 &&
	      memcmp(&output, &saved_output, sizeof(output)) == 0,
	      "numeric failure was not transactional");
	mapping.modulus[0] = 0;
	CHECK(v90_pcm_frame_encoder_init(&encoder, &mapping) ==
		      V90_PCM_FRAME_ENCODER_INVALID_PARAMETERS &&
	      v90_pcm_frame_encoder_init(NULL, &mapping) ==
		      V90_PCM_FRAME_ENCODER_INVALID_ARGUMENT &&
	      !v90_pcm_frame_encoder_is_valid(NULL) &&
	      strcmp(v90_pcm_frame_encoder_result_name(
			     V90_PCM_FRAME_ENCODER_SYMBOLS_READY),
		     "symbols ready") == 0 &&
	      strcmp(v90_pcm_frame_encoder_result_name(
			     (enum v90_pcm_frame_encoder_result)99),
		     "unknown") == 0,
	      "initialization or result-name contract changed");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "unshaped exact frame", test_unshaped_exact_frame },
		{ "distinct transmit and codec planes", test_distinct_codec_plane },
		{ "shaped pipeline", test_shaped_pipeline },
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
