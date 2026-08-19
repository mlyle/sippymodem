// SPDX-License-Identifier: BSD-3-Clause

#include <trellis_decoder.h>

#include <constellation.h>

#include "test.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
	ORACLE_OBSERVATIONS = 1040,
	KNOWN_OBSERVATIONS = 272,
};

struct oracle_fixture {
	uint8_t data_bits;
	uint64_t output_hash[4];
	uint64_t state_hash;
};

static const struct oracle_fixture ORACLES[] = {
	{ 3,
	  { UINT64_C(0x7e464a75cc0c5a58), UINT64_C(0xe4378beb6911e606),
	    UINT64_C(0x6fd68365727756bc), UINT64_C(0xf86361dd99b50f8a) },
	  UINT64_C(0x95f4552146a929a9) },
	{ 4,
	  { UINT64_C(0xc0c4d2197efceebc), UINT64_C(0x695f39e0f0205f68),
	    UINT64_C(0xe5358de4630c0534), UINT64_C(0xf465bee59f83c920) },
	  UINT64_C(0x57c34646b45949a7) },
	{ 5,
	  { UINT64_C(0xfba264547e9823aa), UINT64_C(0x6f2f8cac10394062),
	    UINT64_C(0xc05980103de7a6ba), UINT64_C(0x90df758f48e46672) },
	  UINT64_C(0x3bc4fad1cfbfcb8f) },
	{ 6,
	  { UINT64_C(0x20a42106b948ed43), UINT64_C(0x3bdbe67743e245f3),
	    UINT64_C(0xb0caea534ffdcbd3), UINT64_C(0x5a977d4cd5a31223) },
	  UINT64_C(0x5c6aa22ff73e8039) },
};

static uint64_t hash_byte(uint64_t hash, uint8_t value)
{
	return (hash ^ value) * UINT64_C(1099511628211);
}

static uint64_t hash_u16(uint64_t hash, uint16_t value)
{
	hash = hash_byte(hash, (uint8_t)value);
	return hash_byte(hash, (uint8_t)(value >> 8));
}

static uint64_t decoder_state_hash(
	const struct v32_trellis_decoder *decoder)
{
	uint64_t hash = UINT64_C(14695981039346656037);

	for (size_t state = 0; state < V32_TRELLIS_STATE_COUNT; ++state)
		hash = hash_u16(hash, (uint16_t)decoder->path_metrics[state]);
	hash = hash_u16(hash, decoder->active_slot);
	hash = hash_u16(hash, decoder->previous_differential_output);
	for (size_t slot = 0;
	     slot < V32_TRELLIS_DECODER_TRACEBACK_SYMBOLS; ++slot) {
		for (size_t state = 0;
		     state < V32_TRELLIS_STATE_COUNT; ++state) {
			hash = hash_u16(
				hash, decoder->paths[slot][state].signal_symbol);
			hash = hash_u16(
				hash, decoder->paths[slot][state].predecessor);
		}
	}
	return hash;
}

static uint32_t fixture_random(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static int16_t high_word_s16(uint32_t value)
{
	uint16_t word = (uint16_t)(value >> 16);

	return word <= INT16_MAX ? (int16_t)word :
	       (int16_t)((int32_t)word - 65536);
}

static struct v32_equalizer_observation observation(
	int16_t in_phase, int16_t quadrature, uint64_t epoch,
	uint64_t lineage)
{
	return (struct v32_equalizer_observation){
		.i = in_phase,
		.q = quadrature,
		.phase = (uint16_t)(epoch & INT16_MAX),
		.magnitude = (uint16_t)((epoch * 17u) & INT16_MAX),
		.epoch = epoch,
		.lineage = lineage,
	};
}

static bool test_known_sequences(void)
{
	for (uint8_t bits = V32_TRELLIS_MIN_DATA_BITS;
	     bits <= V32_TRELLIS_MAX_DATA_BITS; ++bits) {
		for (uint8_t prior = 0;
		     prior < V32_TRELLIS_QUADRANT_COUNT; ++prior) {
			struct v32_trellis_encoder encoder;
			struct v32_trellis_decoder decoder;
			uint8_t groups[KNOWN_OBSERVATIONS];
			uint32_t random = UINT32_C(0x6d320000) ^
				(UINT32_C(0x9e3779b9) * bits) ^ prior;
			uint8_t mask = (uint8_t)((UINT8_C(1) << bits) - 1u);

			CHECK(v32_trellis_encoder_init(&encoder, bits, prior) ==
				      V32_TRELLIS_OK &&
			      v32_trellis_decoder_init(&decoder, bits, prior) ==
				      V32_TRELLIS_DECODER_OK,
			      "%u-bit prior %u owners did not initialize", bits,
			      prior);
			for (size_t index = 0;
			     index < ARRAY_LENGTH(groups); ++index) {
				struct v32_fixed_point point;
				struct v32_equalizer_observation input;
				struct v32_trellis_decoded_symbol decoded = {
					.scrambled_group = 0xa5,
					.ready = true,
				};
				uint8_t signal_symbol;

				groups[index] = (uint8_t)(
					fixture_random(&random) & mask);
				CHECK(v32_trellis_encode(
					      &encoder, groups[index], &signal_symbol) ==
					      V32_TRELLIS_OK &&
				      v32_trellis_fixed_signal_point(
					      bits, signal_symbol, &point) ==
					      V32_CONSTELLATION_OK,
				      "%u-bit known symbol %zu did not encode", bits,
				      index);
				input = observation(
					point.i, point.q, index + 1u,
					UINT64_C(0x7620000000000000) |
						((uint64_t)bits << 8) | prior);
				CHECK(v32_trellis_decoder_decode(
					      &decoder, &input, &decoded) ==
					      V32_TRELLIS_DECODER_OK &&
				      v32_trellis_decoder_is_valid(&decoder),
				      "%u-bit prior %u rejected known symbol %zu",
				      bits, prior, index);
				if (index < V32_TRELLIS_DECODER_TRACEBACK_SYMBOLS) {
					CHECK(!decoded.ready &&
					      decoded.scrambled_group == 0,
					      "%u-bit traceback emitted at %zu", bits,
					      index);
				} else {
					CHECK(decoded.ready &&
					      decoded.scrambled_group ==
						      groups[index -
							     V32_TRELLIS_DECODER_TRACEBACK_SYMBOLS],
					      "%u-bit prior %u decoded %u at %zu, expected %u",
					      bits, prior, decoded.scrambled_group, index,
					      groups[index -
						     V32_TRELLIS_DECODER_TRACEBACK_SYMBOLS]);
				}
			}
		}
	}
	return true;
}

static bool test_fixed_oracle(void)
{
	for (size_t fixture_index = 0;
	     fixture_index < ARRAY_LENGTH(ORACLES); ++fixture_index) {
		const struct oracle_fixture *fixture = &ORACLES[fixture_index];

		for (uint8_t prior = 0;
		     prior < V32_TRELLIS_QUADRANT_COUNT; ++prior) {
			struct v32_trellis_decoder decoder;
			uint32_t random = UINT32_C(0x74320000) ^
				(UINT32_C(0x9e3779b9) * fixture->data_bits);
			uint64_t output_hash = UINT64_C(14695981039346656037);

			CHECK(v32_trellis_decoder_init(
				      &decoder, fixture->data_bits, prior) ==
				      V32_TRELLIS_DECODER_OK,
			      "%u-bit oracle owner did not initialize",
			      fixture->data_bits);
			for (size_t index = 0; index < ORACLE_OBSERVATIONS; ++index) {
				int16_t in_phase = high_word_s16(
					fixture_random(&random));
				int16_t quadrature = high_word_s16(
					fixture_random(&random));
				struct v32_equalizer_observation input = observation(
					in_phase, quadrature, index + 1u,
					UINT64_C(0x7432000000000000) |
						fixture->data_bits);
				struct v32_trellis_decoded_symbol decoded;

				CHECK(v32_trellis_decoder_decode(
					      &decoder, &input, &decoded) ==
					      V32_TRELLIS_DECODER_OK &&
				      decoded.ready ==
					      (index >=
					       V32_TRELLIS_DECODER_TRACEBACK_SYMBOLS),
				      "%u-bit oracle readiness changed at %zu",
				      fixture->data_bits, index);
				if (decoded.ready)
					output_hash = hash_byte(
						output_hash, decoded.scrambled_group);
			}
			CHECK(output_hash == fixture->output_hash[prior],
			      "%u-bit prior %u output hash was %016llx",
			      fixture->data_bits, prior,
			      (unsigned long long)output_hash);
			CHECK(decoder_state_hash(&decoder) == fixture->state_hash,
			      "%u-bit prior %u state hash was %016llx",
			      fixture->data_bits, prior,
			      (unsigned long long)decoder_state_hash(&decoder));
		}
	}
	return true;
}

static bool rejected_state_is_transactional(
	struct v32_trellis_decoder invalid,
	const struct v32_equalizer_observation *input)
{
	struct v32_trellis_decoder unchanged = invalid;
	struct v32_trellis_decoded_symbol output;
	struct v32_trellis_decoded_symbol output_unchanged;

	memset(&output, 0xa5, sizeof(output));
	output_unchanged = output;
	CHECK(!v32_trellis_decoder_is_valid(&invalid) &&
	      v32_trellis_decoder_decode(&invalid, input, &output) ==
		      V32_TRELLIS_DECODER_INVALID_STATE &&
	      memcmp(&invalid, &unchanged, sizeof(invalid)) == 0 &&
	      memcmp(&output, &output_unchanged, sizeof(output)) == 0,
	      "invalid owner was accepted or mutated");
	return true;
}

static bool test_contracts(void)
{
	struct v32_trellis_decoder decoder;
	struct v32_trellis_decoder unchanged;
	struct v32_trellis_decoder invalid;
	struct v32_equalizer_observation input = observation(
		1234, -5678, 1, UINT64_C(0x7632000000000001));
	struct v32_equalizer_observation bad_input;
	struct v32_trellis_decoded_symbol output;
	struct v32_trellis_decoded_symbol output_unchanged;

	memset(&decoder, 0xa5, sizeof(decoder));
	unchanged = decoder;
	CHECK(v32_trellis_decoder_init(NULL, 3, 0) ==
		      V32_TRELLIS_DECODER_INVALID_ARGUMENT &&
	      v32_trellis_decoder_init(&decoder, 2, 0) ==
		      V32_TRELLIS_DECODER_INVALID_ARGUMENT &&
	      memcmp(&decoder, &unchanged, sizeof(decoder)) == 0 &&
	      v32_trellis_decoder_init(&decoder, 7, 0) ==
		      V32_TRELLIS_DECODER_INVALID_ARGUMENT &&
	      memcmp(&decoder, &unchanged, sizeof(decoder)) == 0 &&
	      v32_trellis_decoder_init(&decoder, 3, 4) ==
		      V32_TRELLIS_DECODER_INVALID_ARGUMENT &&
	      memcmp(&decoder, &unchanged, sizeof(decoder)) == 0 &&
	      !v32_trellis_decoder_is_valid(NULL),
	      "invalid initialization changed the owner");
	CHECK(v32_trellis_decoder_init(&decoder, 3, 0) ==
		      V32_TRELLIS_DECODER_OK,
	      "contract owner did not initialize");
	invalid = decoder;
	invalid.data_bits_per_symbol = 2;
	CHECK(rejected_state_is_transactional(invalid, &input),
	      "invalid data bits were accepted");
	invalid = decoder;
	invalid.active_slot = V32_TRELLIS_DECODER_TRACEBACK_SYMBOLS;
	CHECK(rejected_state_is_transactional(invalid, &input),
	      "invalid active slot was accepted");
	invalid = decoder;
	invalid.history_fill = 1;
	CHECK(rejected_state_is_transactional(invalid, &input),
	      "invalid fill chronology was accepted");
	invalid = decoder;
	invalid.previous_differential_output = 4;
	CHECK(rejected_state_is_transactional(invalid, &input),
	      "invalid differential state was accepted");
	invalid = decoder;
	invalid.paths[0][0].predecessor = V32_TRELLIS_STATE_COUNT;
	CHECK(rejected_state_is_transactional(invalid, &input),
	      "invalid predecessor was accepted");
	invalid = decoder;
	invalid.paths[0][0].signal_symbol = 16;
	CHECK(rejected_state_is_transactional(invalid, &input),
	      "invalid signal label was accepted");

	unchanged = decoder;
	memset(&output, 0xa5, sizeof(output));
	output_unchanged = output;
	bad_input = input;
	bad_input.epoch = 0;
	CHECK(v32_trellis_decoder_decode(&decoder, NULL, &output) ==
		      V32_TRELLIS_DECODER_INVALID_ARGUMENT &&
	      v32_trellis_decoder_decode(&decoder, &input, NULL) ==
		      V32_TRELLIS_DECODER_INVALID_ARGUMENT &&
	      v32_trellis_decoder_decode(&decoder, &bad_input, &output) ==
		      V32_TRELLIS_DECODER_INVALID_ARGUMENT,
	      "invalid decode argument was accepted");
	bad_input = input;
	bad_input.phase = UINT16_C(0x8000);
	CHECK(v32_trellis_decoder_decode(&decoder, &bad_input, &output) ==
		      V32_TRELLIS_DECODER_INVALID_ARGUMENT,
	      "invalid observation phase was accepted");
	bad_input = input;
	bad_input.magnitude = UINT16_C(0x8000);
	CHECK(v32_trellis_decoder_decode(&decoder, &bad_input, &output) ==
		      V32_TRELLIS_DECODER_INVALID_ARGUMENT,
	      "invalid observation magnitude was accepted");
	CHECK(v32_trellis_decoder_decode(
		      &decoder,
		      (const struct v32_equalizer_observation *)(const void *)&decoder,
		      &output) == V32_TRELLIS_DECODER_INVALID_ARGUMENT &&
	      v32_trellis_decoder_decode(
		      &decoder, &input,
		      (struct v32_trellis_decoded_symbol *)(void *)&decoder) ==
		      V32_TRELLIS_DECODER_INVALID_ARGUMENT &&
	      v32_trellis_decoder_decode(
		      &decoder, &input,
		      (struct v32_trellis_decoded_symbol *)(void *)&input) ==
		      V32_TRELLIS_DECODER_INVALID_ARGUMENT &&
	      memcmp(&decoder, &unchanged, sizeof(decoder)) == 0 &&
	      memcmp(&output, &output_unchanged, sizeof(output)) == 0,
	      "decode rejection changed owner or output");
	return true;
}

int main(void)
{
	unsigned passed = 0;

	if (test_known_sequences())
		++passed;
	if (test_fixed_oracle())
		++passed;
	if (test_contracts())
		++passed;
	if (passed != 3)
		return 1;
	puts("V.32 trellis decoder tests passed");
	return 0;
}
