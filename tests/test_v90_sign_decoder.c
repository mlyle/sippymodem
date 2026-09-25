// SPDX-License-Identifier: BSD-3-Clause

#include <v90_sign_decoder.h>
#include <v90_sign_encoder.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

static bool rule_inverts(
	enum v90_spectral_rule rule, size_t position)
{
	return rule == V90_SPECTRAL_RULE_B ||
	       (rule == V90_SPECTRAL_RULE_C &&
		(position & 1u) == 0) ||
	       (rule == V90_SPECTRAL_RULE_D &&
		(position & 1u) != 0);
}

static enum v90_spectral_rule select_rule(bool state, uint8_t branch)
{
	return (enum v90_spectral_rule)(
		(state ? V90_SPECTRAL_RULE_C : V90_SPECTRAL_RULE_A) +
		branch);
}

static bool states_match(
	const struct v90_sign_encoder *encoder,
	const struct v90_sign_decoder *decoder, bool trellis_state)
{
	return decoder->sign_redundancy == encoder->sign_redundancy &&
	       decoder->previous_odd == encoder->previous_odd &&
	       decoder->previous_pcm_sign ==
		       encoder->previous_pcm_sign &&
	       memcmp(decoder->previous_shaping_signs,
		      encoder->previous_shaping_signs,
		      sizeof(decoder->previous_shaping_signs)) == 0 &&
	       decoder->trellis_state == trellis_state;
}

static bool output_is_canonical(
	const struct v90_sign_decode_output *decoded, size_t bit_count,
	size_t rule_count)
{
	if (decoded->bit_count != bit_count ||
	    decoded->rule_count != rule_count)
		return false;
	for (size_t bit = bit_count;
	     bit < ARRAY_LENGTH(decoded->bits); ++bit) {
		if (decoded->bits[bit] != 0)
			return false;
	}
	for (size_t rule = rule_count;
	     rule < ARRAY_LENGTH(decoded->rules); ++rule) {
		if (decoded->rules[rule] != V90_SPECTRAL_RULE_A)
			return false;
	}
	return true;
}

static bool test_generated_round_trip(void)
{
	static const uint8_t branches[] = { 0, 1, 1, 0 };

	for (uint8_t redundancy = 0; redundancy <= 3; ++redundancy) {
		struct v90_sign_encoder encoder;
		struct v90_sign_decoder decoder;
		size_t bit_count = V90_CP_FRAME_INTERVALS - redundancy;
		size_t shaping_index = 0;
		unsigned int rules_seen = 0;
		bool trellis_state = false;

		CHECK(v90_sign_encoder_init(&encoder, redundancy) ==
			      V90_SIGN_ENCODER_OK &&
		      v90_sign_decoder_init(&decoder, redundancy) ==
			      V90_SIGN_DECODER_OK,
		      "Sr=%u pair did not initialize", redundancy);
		for (unsigned int iteration = 0; iteration < 256; ++iteration) {
			struct v90_sign_decode_output decoded;
			uint8_t bits[V90_CP_FRAME_INTERVALS] = { 0 };
			uint8_t initial[V90_CP_FRAME_INTERVALS];
			uint8_t final[V90_CP_FRAME_INTERVALS];
			enum v90_spectral_rule expected_rules[3] = { 0 };
			unsigned int pattern =
				iteration * 29u + redundancy * 11u;

			for (size_t bit = 0; bit < bit_count; ++bit)
				bits[bit] = (uint8_t)((pattern >> bit) & 1u);
			CHECK(v90_sign_encoder_encode(
				      &encoder, bits, bit_count, initial) ==
				      V90_SIGN_ENCODER_OK,
			      "Sr=%u frame %u did not encode",
			      redundancy, iteration);
			memcpy(final, initial, sizeof(final));
			if (redundancy != 0) {
				size_t frame_length =
					V90_CP_FRAME_INTERVALS / redundancy;

				for (size_t frame = 0;
				     frame < redundancy; ++frame) {
					enum v90_spectral_rule rule = select_rule(
						trellis_state,
						branches[shaping_index %
							 ARRAY_LENGTH(branches)]);

					expected_rules[frame] = rule;
					rules_seen |= 1u << rule;
					for (size_t position = 0;
					     position < frame_length; ++position) {
						size_t symbol =
							frame * frame_length + position;

						final[symbol] ^=
							(uint8_t)rule_inverts(
								rule, position);
					}
					trellis_state = (rule & 1u) != 0;
					shaping_index++;
				}
			}
			CHECK(v90_sign_decoder_decode(
				      &decoder, final, ARRAY_LENGTH(final),
				      &decoded) == V90_SIGN_DECODER_OK &&
			      output_is_canonical(
				      &decoded, bit_count, redundancy) &&
			      memcmp(decoded.bits, bits, bit_count) == 0 &&
			      memcmp(decoded.rules, expected_rules,
				     redundancy *
					     sizeof(expected_rules[0])) == 0 &&
			      states_match(
				      &encoder, &decoder, trellis_state) &&
			      v90_sign_decoder_is_valid(&decoder),
			      "Sr=%u frame %u did not decode exactly",
			      redundancy, iteration);
		}
		CHECK(redundancy == 0 ? rules_seen == 0 : rules_seen == 0x0fu,
		      "Sr=%u did not exercise every allowed rule", redundancy);
	}
	return true;
}

static bool test_zero_histories(void)
{
	for (uint8_t redundancy = 0; redundancy <= 3; ++redundancy) {
		struct v90_sign_encoder encoder;
		struct v90_sign_decoder decoder;
		struct v90_sign_decode_output decoded;
		uint8_t bits[V90_CP_FRAME_INTERVALS] = { 0 };
		uint8_t signs[V90_CP_FRAME_INTERVALS];
		size_t bit_count = V90_CP_FRAME_INTERVALS - redundancy;

		CHECK(v90_sign_encoder_init(&encoder, redundancy) ==
			      V90_SIGN_ENCODER_OK &&
		      v90_sign_decoder_init(&decoder, redundancy) ==
			      V90_SIGN_DECODER_OK &&
		      v90_sign_encoder_encode(
			      &encoder, bits, bit_count, signs) ==
			      V90_SIGN_ENCODER_OK &&
		      v90_sign_decoder_decode(
			      &decoder, signs, ARRAY_LENGTH(signs),
			      &decoded) == V90_SIGN_DECODER_OK &&
		      memcmp(decoded.bits, bits, bit_count) == 0,
		      "zero Sr=%u frame did not round trip", redundancy);
		for (size_t rule = 0; rule < decoded.rule_count; ++rule)
			CHECK(decoded.rules[rule] == V90_SPECTRAL_RULE_A,
			      "zero Sr=%u frame selected nonzero rule", redundancy);
	}
	return true;
}

static bool test_contracts(void)
{
	struct v90_sign_decoder decoder;
	struct v90_sign_decoder saved;
	union {
		struct v90_sign_decoder decoder;
		struct v90_sign_decode_output output;
	} aliased;
	struct v90_sign_decoder untouched;
	struct v90_sign_decoder untouched_saved;
	struct v90_sign_decode_output decoded;
	struct v90_sign_decode_output saved_decoded;
	uint8_t signs[V90_CP_FRAME_INTERVALS] = { 0 };

	memset(&untouched, 0xa5, sizeof(untouched));
	untouched_saved = untouched;
	CHECK(v90_sign_decoder_init(NULL, 0) ==
		      V90_SIGN_DECODER_INVALID_ARGUMENT &&
	      v90_sign_decoder_init(&untouched, 4) ==
		      V90_SIGN_DECODER_INVALID_ARGUMENT &&
	      memcmp(&untouched, &untouched_saved, sizeof(untouched)) == 0,
	      "rejected initialization changed its destination");
	CHECK(v90_sign_decoder_init(&decoder, 2) ==
		      V90_SIGN_DECODER_OK,
	      "contract fixture did not initialize");
	memset(&decoded, 0x5a, sizeof(decoded));
	saved_decoded = decoded;
	saved = decoder;
	aliased.decoder = decoder;
	CHECK(v90_sign_decoder_decode(
		      &aliased.decoder, signs, ARRAY_LENGTH(signs),
		      &aliased.output) == V90_SIGN_DECODER_INVALID_ARGUMENT &&
	      memcmp(&aliased.decoder, &decoder, sizeof(decoder)) == 0,
	      "overlapping output changed sign decoder state");
	signs[4] = 2;
	CHECK(v90_sign_decoder_decode(
		      &decoder, signs, ARRAY_LENGTH(signs), &decoded) ==
		      V90_SIGN_DECODER_INVALID_SIGNS &&
	      memcmp(&decoder, &saved, sizeof(decoder)) == 0 &&
	      memcmp(&decoded, &saved_decoded, sizeof(decoded)) == 0,
	      "non-binary sign changed state or output");
	signs[4] = 0;
	CHECK(v90_sign_decoder_decode(
		      &decoder, signs, ARRAY_LENGTH(signs) - 1u,
		      &decoded) == V90_SIGN_DECODER_INVALID_SIGNS &&
	      v90_sign_decoder_decode(
		      NULL, signs, ARRAY_LENGTH(signs), &decoded) ==
		      V90_SIGN_DECODER_INVALID_ARGUMENT &&
	      v90_sign_decoder_decode(
		      &decoder, NULL, ARRAY_LENGTH(signs), &decoded) ==
		      V90_SIGN_DECODER_INVALID_ARGUMENT &&
	      v90_sign_decoder_decode(
		      &decoder, signs, ARRAY_LENGTH(signs), NULL) ==
		      V90_SIGN_DECODER_INVALID_ARGUMENT &&
	      memcmp(&decoder, &saved, sizeof(decoder)) == 0 &&
	      memcmp(&decoded, &saved_decoded, sizeof(decoded)) == 0,
	      "rejected decode call was not transactional");

	decoder.previous_shaping_signs[3] = 1;
	CHECK(!v90_sign_decoder_is_valid(&decoder) &&
	      v90_sign_decoder_decode(
		      &decoder, signs, ARRAY_LENGTH(signs), &decoded) ==
		      V90_SIGN_DECODER_INVALID_STATE,
	      "decoder accepted inactive shaping history");
	decoder = saved;
	decoder.previous_odd = 2;
	CHECK(!v90_sign_decoder_is_valid(&decoder) &&
	      !v90_sign_decoder_is_valid(NULL),
	      "decoder accepted invalid differential history");
	CHECK(v90_sign_decoder_init(&decoder, 0) ==
		      V90_SIGN_DECODER_OK,
	      "unshaped contract state did not initialize");
	decoder.trellis_state = true;
	CHECK(!v90_sign_decoder_is_valid(&decoder) &&
	      strcmp(v90_sign_decoder_result_name(
		     V90_SIGN_DECODER_INVALID_SIGNS),
		     "invalid signs") == 0 &&
	      strcmp(v90_sign_decoder_result_name(
		     (enum v90_sign_decoder_result)99),
		     "unknown") == 0,
	      "unshaped validation or result names changed");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "generated round trip", test_generated_round_trip },
		{ "zero histories", test_zero_histories },
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
