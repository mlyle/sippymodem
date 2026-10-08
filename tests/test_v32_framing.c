// SPDX-License-Identifier: BSD-3-Clause

#include <framing.h>

#include "test.h"

#include <stddef.h>
#include <string.h>

enum {
	TEST_STREAM_SYMBOLS = 2048,
	TEST_RESYNC_DIBITS = 96,
	TEST_TRN_DIBITS = 257,
	TEST_RATE_SEQUENCE_DIBITS = 64,
};

static uint32_t fixture_random(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static bool test_descrambler_polynomials(void)
{
	static const struct {
		enum v32_transmitter_role role;
		unsigned int role_tap;
	} fixtures[] = {
		{ V32_CALLING_TRANSMITTER, 18 },
		{ V32_ANSWERING_TRANSMITTER, 5 },
	};

	for (size_t fixture = 0; fixture < ARRAY_LENGTH(fixtures); ++fixture) {
		struct v32_scrambler descrambler;

		CHECK(v32_scrambler_init(
			      &descrambler, fixtures[fixture].role) ==
			      V32_FRAMING_OK,
		      "V.32 descrambler fixture did not initialize");
		for (unsigned int bit = 0; bit < 24; bit += 2) {
			uint8_t decoded;
			uint8_t received = bit == 0 ? 2u : 0u;
			uint8_t expected = 0;

			for (unsigned int offset = 0; offset < 2; ++offset) {
				unsigned int position = bit + offset;

				expected = (uint8_t)(expected << 1);
				if (position == 0 ||
				    position == fixtures[fixture].role_tap ||
				    position == V32_SCRAMBLER_BITS)
					expected |= 1u;
			}
			CHECK(v32_descramble_dibit(
				      &descrambler, received, &decoded) ==
				      V32_FRAMING_OK &&
				      decoded == expected,
			      "V.32 descrambler polynomial changed at bit %u",
			      bit);
		}
	}
	return true;
}

static bool test_normative_scrambler_starts(void)
{
	static const uint8_t trn_quadrant_by_dibit[] = { 0, 1, 3, 2 };
	static const uint8_t expected[][15] = {
		{ 3, 3, 3, 3, 3, 3, 3, 3, 3, 0, 0, 1, 3, 3, 3 },
		{ 3, 3, 2, 0, 0, 3, 3, 2, 0, 0, 3, 2, 1, 3, 3 },
	};
	static const uint8_t expected_9600[][7] = {
		{ 15, 11, 7, 3, 12, 1, 15 },
		{ 15, 4, 11, 6, 8, 6, 7 },
	};

	for (size_t role = 0; role < ARRAY_LENGTH(expected); ++role) {
		struct v32_symbol_encoder encoder;
		struct v32_symbol_encoder trn_encoder;
		struct v32_symbol_encoder encoder_9600;
		uint8_t previous = 0;

		CHECK(v32_symbol_encoder_init(
			      &encoder,
			      (enum v32_transmitter_role)role, 0) ==
			      V32_FRAMING_OK &&
		      v32_symbol_encoder_init(
			      &trn_encoder,
			      (enum v32_transmitter_role)role,
			      (uint8_t)(role + 1)) ==
			      V32_FRAMING_OK &&
		      v32_symbol_encoder_init(
			      &encoder_9600,
			      (enum v32_transmitter_role)role, 0) ==
			      V32_FRAMING_OK,
		      "V.32 normative encoder did not initialize");
		for (size_t index = 0; index < ARRAY_LENGTH(expected[role]);
		     ++index) {
			uint8_t quadrant;
			uint8_t scrambled;
			uint8_t trn_quadrant;
			uint8_t expected_trn_quadrant =
				trn_quadrant_by_dibit[expected[role][index]];

			CHECK(v32_4800_symbol_encode(
				      &encoder, 3, &quadrant) ==
				      V32_FRAMING_OK &&
			      v32_trn_symbol_encode(
				      &trn_encoder, 3, &trn_quadrant) ==
				      V32_FRAMING_OK &&
				      differential_qpsk_decode(
					      previous, quadrant, &scrambled) ==
					      DIFFERENTIAL_QPSK_OK &&
				      scrambled == expected[role][index] &&
				      trn_quadrant == expected_trn_quadrant,
			      "V.32 normative start changed at %zu/%zu", role,
			      index);
			previous = quadrant;
		}
		for (size_t index = 0;
		     index < ARRAY_LENGTH(expected_9600[role]); ++index) {
			uint8_t symbol;

			CHECK(v32_9600_symbol_encode(
				      &encoder_9600, 15, &symbol) ==
				      V32_FRAMING_OK &&
				      symbol == expected_9600[role][index],
			      "V.32 9600 normative start changed at %zu/%zu",
			      role, index);
		}
	}
	return true;
}

static bool test_symbol_round_trip(void)
{
	uint32_t random = UINT32_C(0x32c411a7);

	for (unsigned int role = 0; role < 2; ++role) {
		for (uint8_t initial = 0;
		     initial < QPSK_QUADRANT_COUNT; ++initial) {
			struct v32_symbol_encoder encoder;
			struct v32_symbol_decoder decoder;

			CHECK(v32_symbol_encoder_init(
				      &encoder,
				      (enum v32_transmitter_role)role,
				      initial) == V32_FRAMING_OK &&
				      v32_symbol_decoder_init(
					      &decoder,
					      (enum v32_transmitter_role)role,
					      initial) == V32_FRAMING_OK,
			      "V.32 symbol endpoints did not initialize");
			for (size_t index = 0; index < TEST_STREAM_SYMBOLS;
			     ++index) {
				uint8_t input =
					(uint8_t)(fixture_random(&random) & 3u);
				uint8_t output;
				uint8_t quadrant;

				CHECK(v32_4800_symbol_encode(
					      &encoder, input, &quadrant) ==
					      V32_FRAMING_OK &&
				      v32_4800_symbol_decode(
					      &decoder, quadrant, &output) ==
					      V32_FRAMING_OK &&
				      output == input,
				      "V.32 symbol round trip changed at %u/%u/%zu",
				      role, initial, index);
			}
			CHECK(encoder.phase_quadrant == decoder.phase_quadrant &&
				      encoder.scrambler.history ==
					      decoder.descrambler.history,
			      "V.32 symbol endpoints diverged");
		}
	}
	return true;
}

static bool test_trn_round_trip_and_transition(void)
{
	uint32_t random = UINT32_C(0x6d8eb713);

	for (unsigned int role = 0; role < 2; ++role) {
		for (uint8_t initial = 0;
		     initial < QPSK_QUADRANT_COUNT; ++initial) {
			struct v32_symbol_encoder encoder;
			struct v32_symbol_decoder decoder;

			CHECK(v32_symbol_encoder_init(
				      &encoder,
				      (enum v32_transmitter_role)role,
				      initial) == V32_FRAMING_OK &&
			      v32_symbol_decoder_init(
				      &decoder,
				      (enum v32_transmitter_role)role,
				      initial) == V32_FRAMING_OK,
			      "V.32 TRN endpoints did not initialize");
			for (size_t index = 0; index < TEST_TRN_DIBITS; ++index) {
				uint8_t input =
					(uint8_t)(fixture_random(&random) & 3u);
				uint8_t output;
				uint8_t quadrant;

				CHECK(v32_trn_symbol_encode(
					      &encoder, input, &quadrant) ==
					      V32_FRAMING_OK &&
				      v32_trn_symbol_decode(
					      &decoder, quadrant, &output) ==
					      V32_FRAMING_OK &&
				      output == input,
				      "V.32 TRN round trip changed at %u/%u/%zu",
				      role, initial, index);
			}
			for (size_t index = 0; index < TEST_STREAM_SYMBOLS;
			     ++index) {
				uint8_t input =
					(uint8_t)(fixture_random(&random) & 3u);
				uint8_t output;
				uint8_t quadrant;

				CHECK(v32_4800_symbol_encode(
					      &encoder, input, &quadrant) ==
					      V32_FRAMING_OK &&
				      v32_4800_symbol_decode(
					      &decoder, quadrant, &output) ==
					      V32_FRAMING_OK &&
				      output == input,
				      "V.32 TRN transition changed at %u/%u/%zu",
				      role, initial, index);
			}
			CHECK(encoder.phase_quadrant == decoder.phase_quadrant &&
				      encoder.scrambler.history ==
					      decoder.descrambler.history,
			      "V.32 TRN endpoints diverged");
		}
	}
	return true;
}

static bool test_9600_round_trip_and_transition(void)
{
	uint32_t random = UINT32_C(0xc9600321);

	for (unsigned int role = 0; role < 2; ++role) {
		for (uint8_t initial = 0;
		     initial < QPSK_QUADRANT_COUNT; ++initial) {
			struct v32_symbol_encoder encoder;
			struct v32_symbol_decoder decoder;

			CHECK(v32_symbol_encoder_init(
				      &encoder,
				      (enum v32_transmitter_role)role,
				      initial) == V32_FRAMING_OK &&
			      v32_symbol_decoder_init(
				      &decoder,
				      (enum v32_transmitter_role)role,
				      initial) == V32_FRAMING_OK,
			      "V.32 9600 endpoints did not initialize");
			for (size_t index = 0; index < TEST_RATE_SEQUENCE_DIBITS;
			     ++index) {
				uint8_t input =
					(uint8_t)(fixture_random(&random) & 3u);
				uint8_t output;
				uint8_t quadrant;

				CHECK(v32_4800_symbol_encode(
					      &encoder, input, &quadrant) ==
					      V32_FRAMING_OK &&
				      v32_4800_symbol_decode(
					      &decoder, quadrant, &output) ==
					      V32_FRAMING_OK &&
				      output == input,
				      "V.32 9600 rate transition changed at %u/%u/%zu",
				      role, initial, index);
			}
			for (size_t index = 0; index < TEST_STREAM_SYMBOLS; ++index) {
				uint8_t input =
					(uint8_t)(fixture_random(&random) & 15u);
				uint8_t output;
				uint8_t symbol;

				CHECK(v32_9600_symbol_encode(
					      &encoder, input, &symbol) ==
					      V32_FRAMING_OK &&
				      symbol < V32_9600_SYMBOL_COUNT &&
				      v32_9600_symbol_decode(
					      &decoder, symbol, &output) ==
					      V32_FRAMING_OK &&
				      output == input,
				      "V.32 9600 round trip changed at %u/%u/%zu",
				      role, initial, index);
			}
			CHECK(encoder.phase_quadrant == decoder.phase_quadrant &&
				      encoder.scrambler.history ==
					      decoder.descrambler.history,
			      "V.32 9600 endpoints diverged");
		}
	}
	return true;
}

static bool test_descrambler_resynchronization(void)
{
	struct v32_scrambler encoder;
	struct v32_scrambler decoder;
	uint32_t random = UINT32_C(0xb394e185);

	CHECK(v32_scrambler_init(
		      &encoder, V32_CALLING_TRANSMITTER) ==
		      V32_FRAMING_OK &&
		      v32_scrambler_init(
			      &decoder, V32_CALLING_TRANSMITTER) ==
			      V32_FRAMING_OK,
	      "V.32 resynchronization endpoints did not initialize");
	decoder.history = UINT32_C(0x5a4c31);
	for (size_t index = 0; index < TEST_RESYNC_DIBITS; ++index) {
		uint8_t input = (uint8_t)(fixture_random(&random) & 3u);
		uint8_t encoded;
		uint8_t output;

		CHECK(v32_scramble_dibit(
			      &encoder, input, &encoded) ==
			      V32_FRAMING_OK &&
			      v32_descramble_dibit(
				      &decoder, encoded, &output) ==
				      V32_FRAMING_OK &&
			      (index < 12 || output == input),
		      "V.32 descrambler did not synchronize at %zu", index);
	}
	CHECK(encoder.history == decoder.history,
	      "V.32 descrambler history did not converge");
	return true;
}

static bool test_three_bit_group_round_trip(void)
{
	uint32_t random = UINT32_C(0x72003211);

	for (unsigned int role = 0; role < 2; ++role) {
		struct v32_scrambler encoder;
		struct v32_scrambler decoder;

		CHECK(v32_scrambler_init(
			      &encoder,
			      (enum v32_transmitter_role)role) ==
			      V32_FRAMING_OK &&
		      v32_scrambler_init(
			      &decoder,
			      (enum v32_transmitter_role)role) ==
			      V32_FRAMING_OK,
		      "V.32 three-bit framing endpoints did not initialize");
		for (size_t index = 0; index < TEST_STREAM_SYMBOLS; ++index) {
			uint8_t input =
				(uint8_t)(fixture_random(&random) & 7u);
			uint8_t encoded;
			uint8_t output;

			CHECK(v32_scramble_group(
				      &encoder, input, 3, &encoded) ==
				      V32_FRAMING_OK &&
			      v32_descramble_group(
				      &decoder, encoded, 3, &output) ==
				      V32_FRAMING_OK &&
			      output == input,
			      "V.32 three-bit framing changed at %u/%zu", role,
			      index);
		}
		CHECK(encoder.history == decoder.history,
		      "V.32 three-bit framing histories diverged");
	}
	return true;
}

static bool test_group_state_rejection(void)
{
	for (unsigned int direction = 0; direction < 2; ++direction) {
		for (unsigned int bad = 0; bad < 3; ++bad) {
			struct v32_scrambler state = {
				.history = bad == 0 ? UINT32_C(1) << V32_SCRAMBLER_BITS : 0,
				.transmitter_role = bad == 1 ? (enum v32_transmitter_role)2 :
					V32_CALLING_TRANSMITTER,
			};
			struct v32_scrambler saved = state;
			struct v32_scrambler *owner = bad == 2 ? NULL : &state;
			uint8_t output = UINT8_MAX;
			enum v32_framing_result result = direction == 0 ?
				v32_scramble_group(owner, 1, 1, &output) :
				v32_descramble_group(owner, 1, 1, &output);

			CHECK(result == V32_FRAMING_INVALID_ARGUMENT &&
			      output == UINT8_MAX &&
			      memcmp(&state, &saved, sizeof(state)) == 0,
			      "V.32 group accepted invalid owner %u/%u", direction, bad);
		}
	}
	return true;
}

static bool test_symbol_rejection(void)
{
	static const struct {
		enum v32_framing_result (*encode)(
			struct v32_symbol_encoder *, uint8_t, uint8_t *);
		enum v32_framing_result (*decode)(
			struct v32_symbol_decoder *, uint8_t, uint8_t *);
		uint8_t limit;
	} modes[] = {
		{ v32_trn_symbol_encode, v32_trn_symbol_decode, 3 },
		{ v32_4800_symbol_encode, v32_4800_symbol_decode, 3 },
		{ v32_9600_symbol_encode, v32_9600_symbol_decode, 15 },
	};

	for (size_t mode = 0; mode < ARRAY_LENGTH(modes); ++mode) {
		for (unsigned int role = 0; role < 2; ++role) {
			for (unsigned int bad = 0; bad < 7; ++bad) {
				struct v32_scrambler scrambler = {
					.history = bad == 3 ?
						UINT32_C(1) << V32_SCRAMBLER_BITS :
						UINT32_C(0x5a1234),
					.transmitter_role = (enum v32_transmitter_role)(
						bad == 4 ? -1 : bad == 5 ? 2 : (int)role),
				};
				struct v32_symbol_encoder encoder;
				struct v32_symbol_decoder decoder;
				struct v32_symbol_encoder saved_encoder;
				struct v32_symbol_decoder saved_decoder;
				uint8_t output = UINT8_MAX;
				uint8_t input = bad == 0 ? modes[mode].limit + 1u : 0;
				uint8_t *destination = bad == 1 ? NULL : &output;

				/* Define owner padding, then preserve every rejection byte. */
				memset(&encoder, 0, sizeof(encoder));
				encoder.scrambler = scrambler;
				encoder.phase_quadrant = bad == 2 ? 4 : 2;
				memset(&decoder, 0, sizeof(decoder));
				decoder.descrambler = scrambler;
				decoder.phase_quadrant = encoder.phase_quadrant;
				memcpy(&saved_encoder, &encoder, sizeof(saved_encoder));
				memcpy(&saved_decoder, &decoder, sizeof(saved_decoder));

				CHECK(modes[mode].encode(
					      bad == 6 ? NULL : &encoder, input,
					      destination) == V32_FRAMING_INVALID_ARGUMENT &&
				      modes[mode].decode(
					      bad == 6 ? NULL : &decoder, input,
					      destination) == V32_FRAMING_INVALID_ARGUMENT &&
				      output == UINT8_MAX &&
				      memcmp(&encoder, &saved_encoder, sizeof(encoder)) == 0 &&
				      memcmp(&decoder, &saved_decoder, sizeof(decoder)) == 0,
				      "V.32 symbol rejection changed at %zu/%u/%u",
				      mode, role, bad);
			}
		}
	}
	return true;
}

static enum v32_framing_result transform_owned_group(
	struct v32_scrambler *state, unsigned int mode,
	uint8_t input, unsigned int width, uint8_t *output)
{
	switch (mode) {
	case 0: return v32_scramble_group(state, input, width, output);
	case 1: return v32_descramble_group(state, input, width, output);
	case 2: return v32_scramble_dibit(state, input, output);
	default: return v32_descramble_dibit(state, input, output);
	}
}

static bool test_framing_output_ownership(void)
{
	struct group_fixture {
		uint8_t prefix;
		struct v32_scrambler owner;
		uint8_t suffix;
	} group_initial, group_fixture, group_expected;
	struct encoder_fixture {
		uint8_t prefix;
		struct v32_symbol_encoder owner;
		uint8_t suffix;
	} encoder_initial, encoder_fixture, encoder_expected;
	struct decoder_fixture {
		uint8_t prefix;
		struct v32_symbol_decoder owner;
		uint8_t suffix;
	} decoder_initial, decoder_fixture, decoder_expected;
	static const struct {
		enum v32_framing_result (*encode)(
			struct v32_symbol_encoder *, uint8_t, uint8_t *);
		enum v32_framing_result (*decode)(
			struct v32_symbol_decoder *, uint8_t, uint8_t *);
		unsigned int limit;
	} modes[] = {
		{ v32_trn_symbol_encode, v32_trn_symbol_decode, 4 },
		{ v32_4800_symbol_encode, v32_4800_symbol_decode, 4 },
		{ v32_9600_symbol_encode, v32_9600_symbol_decode, 16 },
	};

	for (unsigned int role = 0; role < 2; ++role) {
		for (unsigned int mode = 0; mode < 4; ++mode) {
			for (unsigned int width = mode < 2 ? 1u : 2u;
			     width <= (mode < 2 ? 8u : 2u); ++width) {
				for (unsigned int input = 0; input < (1u << width); ++input) {
					uint8_t output;

					memset(&group_initial, 0xa5, sizeof(group_initial));
					group_initial.owner.history = UINT32_C(0x7fffff);
					group_initial.owner.transmitter_role = role;
					memcpy(&group_expected, &group_initial, sizeof(group_expected));
					CHECK(transform_owned_group(&group_expected.owner, mode,
						      (uint8_t)input, width, &output) == V32_FRAMING_OK,
					      "disjoint group output failed");
					for (size_t offset = 0; offset < sizeof(group_initial.owner); ++offset) {
						memcpy(&group_fixture, &group_initial, sizeof(group_fixture));
						CHECK(transform_owned_group(&group_fixture.owner, mode,
							      (uint8_t)input, width,
							      (uint8_t *)&group_fixture.owner + offset) ==
							      V32_FRAMING_INVALID_ARGUMENT &&
						      memcmp(&group_fixture, &group_initial, sizeof(group_fixture)) == 0,
						      "group overlap changed storage at %u/%u/%u/%u/%zu",
						      role, mode, width, input, offset);
					}
					for (unsigned int side = 0; side < 2; ++side) {
						size_t offset = offsetof(struct group_fixture, owner) +
							(side ? sizeof(group_fixture.owner) : 0) - (side ? 0 : 1);

						memcpy(&group_fixture, &group_initial, sizeof(group_fixture));
						((uint8_t *)&group_expected)[offset] = output;
						CHECK(transform_owned_group(&group_fixture.owner, mode,
							      (uint8_t)input, width, (uint8_t *)&group_fixture + offset) ==
							      V32_FRAMING_OK &&
						      memcmp(&group_fixture, &group_expected, sizeof(group_fixture)) == 0,
						      "adjacent group output changed storage");
						((uint8_t *)&group_expected)[offset] = 0xa5;
					}
				}
			}
		}
		for (size_t mode = 0; mode < ARRAY_LENGTH(modes); ++mode) {
			for (uint8_t phase = 0; phase < QPSK_QUADRANT_COUNT; ++phase) {
				for (unsigned int input = 0; input < modes[mode].limit; ++input) {
					uint8_t symbol, output;

					memset(&encoder_initial, 0xa5, sizeof(encoder_initial));
					encoder_initial.owner.scrambler.history = UINT32_C(0x7fffff);
					encoder_initial.owner.scrambler.transmitter_role = role;
					encoder_initial.owner.phase_quadrant = phase;
					memset(&decoder_initial, 0xa5, sizeof(decoder_initial));
					decoder_initial.owner.descrambler.history = UINT32_C(0x7fffff);
					decoder_initial.owner.descrambler.transmitter_role = role;
					decoder_initial.owner.phase_quadrant = phase;
					memcpy(&encoder_expected, &encoder_initial, sizeof(encoder_expected));
					memcpy(&decoder_expected, &decoder_initial, sizeof(decoder_expected));
					CHECK(modes[mode].encode(&encoder_expected.owner, (uint8_t)input,
						      &symbol) == V32_FRAMING_OK &&
					      modes[mode].decode(&decoder_expected.owner, (uint8_t)input,
						      &output) == V32_FRAMING_OK,
					      "disjoint symbol output failed");
					for (size_t offset = 0; offset < sizeof(encoder_initial.owner); ++offset) {
						memcpy(&encoder_fixture, &encoder_initial, sizeof(encoder_fixture));
						CHECK(modes[mode].encode(&encoder_fixture.owner, (uint8_t)input,
							      (uint8_t *)&encoder_fixture.owner + offset) ==
							      V32_FRAMING_INVALID_ARGUMENT &&
						      memcmp(&encoder_fixture, &encoder_initial, sizeof(encoder_fixture)) == 0,
						      "encoder overlap changed storage at %u/%zu/%u/%u/%zu",
						      role, mode, phase, input, offset);
					}
					for (size_t offset = 0; offset < sizeof(decoder_initial.owner); ++offset) {
						memcpy(&decoder_fixture, &decoder_initial, sizeof(decoder_fixture));
						CHECK(modes[mode].decode(&decoder_fixture.owner, (uint8_t)input,
							      (uint8_t *)&decoder_fixture.owner + offset) ==
							      V32_FRAMING_INVALID_ARGUMENT &&
						      memcmp(&decoder_fixture, &decoder_initial, sizeof(decoder_fixture)) == 0,
						      "decoder overlap changed storage at %u/%zu/%u/%u/%zu",
						      role, mode, phase, input, offset);
					}
					for (unsigned int side = 0; side < 2; ++side) {
						size_t eo = offsetof(struct encoder_fixture, owner) +
							(side ? sizeof(encoder_fixture.owner) : 0) - (side ? 0 : 1);
						size_t doff = offsetof(struct decoder_fixture, owner) +
							(side ? sizeof(decoder_fixture.owner) : 0) - (side ? 0 : 1);

						memcpy(&encoder_fixture, &encoder_initial, sizeof(encoder_fixture));
						memcpy(&decoder_fixture, &decoder_initial, sizeof(decoder_fixture));
						((uint8_t *)&encoder_expected)[eo] = symbol;
						((uint8_t *)&decoder_expected)[doff] = output;
						CHECK(modes[mode].encode(&encoder_fixture.owner, (uint8_t)input,
							      (uint8_t *)&encoder_fixture + eo) == V32_FRAMING_OK &&
						      modes[mode].decode(&decoder_fixture.owner, (uint8_t)input,
							      (uint8_t *)&decoder_fixture + doff) == V32_FRAMING_OK &&
						      memcmp(&encoder_fixture, &encoder_expected, sizeof(encoder_fixture)) == 0 &&
						      memcmp(&decoder_fixture, &decoder_expected, sizeof(decoder_fixture)) == 0,
						      "adjacent symbol output changed storage");
						((uint8_t *)&encoder_expected)[eo] = 0xa5;
						((uint8_t *)&decoder_expected)[doff] = 0xa5;
					}
				}
			}
		}
	}
	return true;
}


static bool test_validation(void)
{
	struct v32_scrambler scrambler;
	struct v32_scrambler saved_scrambler;
	uint8_t output = UINT8_MAX;

	CHECK(v32_scrambler_init(
		      &scrambler, V32_CALLING_TRANSMITTER) ==
		      V32_FRAMING_OK,
	      "V.32 validation fixtures did not initialize");
	saved_scrambler = scrambler;
	CHECK(v32_scrambler_init(
		      NULL, V32_CALLING_TRANSMITTER) ==
		      V32_FRAMING_INVALID_ARGUMENT &&
		      v32_scrambler_init(
			      &scrambler,
			      (enum v32_transmitter_role)2) ==
			      V32_FRAMING_INVALID_ARGUMENT &&
		      memcmp(&scrambler, &saved_scrambler,
			     sizeof(scrambler)) == 0 &&
		      v32_scramble_dibit(&scrambler, 4, &output) ==
			      V32_FRAMING_INVALID_ARGUMENT &&
		      memcmp(&scrambler, &saved_scrambler,
			     sizeof(scrambler)) == 0 &&
		      v32_scramble_group(
			      &scrambler, 0, 0, &output) ==
			      V32_FRAMING_INVALID_ARGUMENT &&
		      v32_scramble_group(
			      &scrambler, 0, 9, &output) ==
			      V32_FRAMING_INVALID_ARGUMENT &&
		      v32_scramble_group(
			      &scrambler, 8, 3, &output) ==
			      V32_FRAMING_INVALID_ARGUMENT &&
		      v32_descramble_group(
			      &scrambler, 0, 3, NULL) ==
			      V32_FRAMING_INVALID_ARGUMENT &&
		      memcmp(&scrambler, &saved_scrambler,
			     sizeof(scrambler)) == 0 &&
		      output == UINT8_MAX,
	      "V.32 framing accepted invalid input or mutated state");
	scrambler.history = UINT32_C(1) << V32_SCRAMBLER_BITS;
	CHECK(!v32_scrambler_is_valid(&scrambler),
	      "V.32 framing accepted corrupt history");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "descrambler polynomials", test_descrambler_polynomials },
		{ "normative scrambler starts", test_normative_scrambler_starts },
		{ "symbol round trip", test_symbol_round_trip },
		{ "TRN round trip and transition",
		  test_trn_round_trip_and_transition },
		{ "9600 round trip and transition",
		  test_9600_round_trip_and_transition },
		{ "descrambler resynchronization",
		  test_descrambler_resynchronization },
		{ "three-bit group round trip",
		  test_three_bit_group_round_trip },
		{ "group state rejection", test_group_state_rejection },
		{ "symbol rejection", test_symbol_rejection },
		{ "framing output ownership", test_framing_output_ownership },
		{ "validation", test_validation },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		bool passed;

		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		passed = tests[index].run();
		if (!passed) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0 ? 0 : 1;
}
