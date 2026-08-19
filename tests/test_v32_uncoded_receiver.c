// SPDX-License-Identifier: BSD-3-Clause

#include <uncoded_receiver.h>

#include <constellation.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

enum {
	TEST_PRIOR_SYMBOLS = 96,
	TEST_B1_SYMBOLS = 128,
	TEST_DATA_SYMBOLS = 1024,
	TEST_LANES = 2 * QPSK_QUADRANT_COUNT,
};

struct test_lane {
	struct v32_symbol_encoder encoder;
	struct v32_uncoded_receiver receiver;
	enum v32_transmitter_role role;
	uint32_t random;
	uint64_t epoch;
	uint64_t lineage;
};

union test_overlap {
	struct v32_uncoded_receiver receiver;
	struct v32_scrambler descrambler;
	struct v32_equalizer_observation observation;
	struct v32_uncoded_symbol symbol;
};

static uint32_t fixture_random(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static bool equalizer_decisions_match(
	const struct v32_equalizer_decision *first,
	const struct v32_equalizer_decision *second)
{
	return memcmp(
		       &first->observation, &second->observation,
		       sizeof(first->observation)) == 0 &&
	       first->ideal_phase == second->ideal_phase &&
	       first->ideal_magnitude == second->ideal_magnitude &&
	       first->adapt_on_error == second->adapt_on_error;
}

static bool scramblers_match(
	const struct v32_scrambler *first,
	const struct v32_scrambler *second)
{
	return first->history == second->history &&
	       first->transmitter_role == second->transmitter_role;
}

static bool receiver_states_match(
	const struct v32_uncoded_receiver *first,
	const struct v32_uncoded_receiver *second)
{
	return first->decoder.phase_quadrant ==
		       second->decoder.phase_quadrant &&
	       scramblers_match(
		       &first->decoder.descrambler,
		       &second->decoder.descrambler);
}

static bool make_prior_framing(
	enum v32_transmitter_role role,
	struct v32_symbol_encoder *encoder,
	struct v32_symbol_decoder *decoder)
{
	uint32_t random = UINT32_C(0x96003211) ^ (uint32_t)role;

	CHECK(v32_symbol_encoder_init(encoder, role, 0) == V32_FRAMING_OK &&
	      v32_symbol_decoder_init(decoder, role, 0) == V32_FRAMING_OK,
	      "prior framing for role %u did not initialize", role);
	for (size_t index = 0; index < TEST_PRIOR_SYMBOLS; ++index) {
		uint8_t input = (uint8_t)(fixture_random(&random) & 3u);
		uint8_t output;
		uint8_t quadrant;

		CHECK(v32_4800_symbol_encode(encoder, input, &quadrant) ==
			      V32_FRAMING_OK &&
		      v32_4800_symbol_decode(decoder, quadrant, &output) ==
			      V32_FRAMING_OK &&
		      output == input,
		      "prior framing for role %u diverged at %zu", role, index);
	}
	CHECK(encoder->scrambler.history != 0 &&
	      encoder->scrambler.history == decoder->descrambler.history,
	      "prior framing for role %u did not leave continuous history",
	      role);
	return true;
}

static bool init_lanes(struct test_lane lanes[TEST_LANES])
{
	for (unsigned int role = 0; role < 2; ++role) {
		struct v32_symbol_encoder prior_encoder;
		struct v32_symbol_decoder prior_decoder;

		CHECK(make_prior_framing(
			      (enum v32_transmitter_role)role,
			      &prior_encoder, &prior_decoder),
		      "lane prior framing failed for role %u", role);
		for (uint8_t old_phase = 0;
		     old_phase < QPSK_QUADRANT_COUNT; ++old_phase) {
			size_t index = role * QPSK_QUADRANT_COUNT + old_phase;
			struct v32_symbol_decoder old_rate = prior_decoder;
			struct test_lane *lane = &lanes[index];

			old_rate.phase_quadrant = old_phase;
			lane->encoder = (struct v32_symbol_encoder){
				.scrambler = prior_encoder.scrambler,
				.phase_quadrant = 0,
			};
			lane->role = (enum v32_transmitter_role)role;
			lane->random = UINT32_C(0x32c19600) ^
				(uint32_t)(role * 0x100u + old_phase);
			lane->epoch = UINT64_C(0x100000) + index * 0x10000u;
			lane->lineage = UINT64_C(0x9600000000000000) |
				(index + 1u);
			CHECK(v32_uncoded_receiver_init(
				      &lane->receiver,
				      &old_rate.descrambler) ==
			      V32_UNCODED_RECEIVER_OK &&
			      lane->receiver.decoder.phase_quadrant == 0 &&
			      scramblers_match(
				      &lane->receiver.decoder.descrambler,
				      &old_rate.descrambler),
			      "lane %zu carried old rate phase %u", index,
			      old_phase);
			if (old_phase != 0)
				CHECK(receiver_states_match(
					      &lane->receiver,
					      &lanes[role * QPSK_QUADRANT_COUNT]
						       .receiver),
				      "old rate phase %u changed role %u seed",
				      old_phase, role);
		}
	}
	return true;
}

static bool receive_group(
	struct test_lane *lane, uint8_t input, bool adapt_on_error)
{
	struct v32_equalizer_observation observation;
	struct v32_uncoded_decision expected;
	struct v32_uncoded_symbol symbol;
	struct v32_fixed_point point;
	uint8_t signal;

	CHECK(v32_9600_symbol_encode(&lane->encoder, input, &signal) ==
		      V32_FRAMING_OK &&
	      v32_constellation_fixed_point(signal, &point) ==
		      V32_CONSTELLATION_OK,
	      "uncoded transmitter rejected group %u", input);
	lane->epoch++;
	observation = (struct v32_equalizer_observation){
		.i = point.i,
		.q = point.q,
		.phase = (uint16_t)((lane->epoch * 17u) & INT16_MAX),
		.magnitude = (uint16_t)(0x1000u + (lane->epoch & 0x0fffu)),
		.epoch = lane->epoch,
		.lineage = lane->lineage,
	};
	CHECK(v32_decide_uncoded_16(
		      &observation, adapt_on_error, &expected) ==
		      V32_DECISION_OK &&
	      expected.symbol == signal &&
	      v32_uncoded_receiver_observe(
		      &lane->receiver, &observation, adapt_on_error,
		      &symbol) == V32_UNCODED_RECEIVER_OK &&
	      symbol.group == input &&
	      equalizer_decisions_match(
		      &symbol.equalizer, &expected.equalizer),
	      "uncoded receive changed group %u at epoch %llu", input,
	      (unsigned long long)lane->epoch);
	return true;
}

static bool test_feedback_composition(
	const struct v32_uncoded_receiver *seed)
{
	for (uint8_t signal = 0; signal < V32_CONSTELLATION_SYMBOLS;
	     ++signal) {
		struct v32_uncoded_receiver receiver = *seed;
		struct v32_uncoded_receiver expected_receiver = *seed;
		struct v32_equalizer_observation observation;
		struct v32_uncoded_decision decision;
		struct v32_uncoded_symbol symbol;
		struct v32_fixed_point point;
		uint16_t absolute_i;
		uint16_t absolute_q;
		uint16_t expected_magnitude;
		uint8_t expected_group;
		bool adapt_on_error = (signal & 1u) != 0;

		CHECK(v32_constellation_fixed_point(signal, &point) ==
			      V32_CONSTELLATION_OK,
		      "feedback point %u did not generate", signal);
		observation = (struct v32_equalizer_observation){
			.i = point.i,
			.q = point.q,
			.phase = (uint16_t)(0x1000u + signal),
			.magnitude = (uint16_t)(0x2000u + signal),
			.epoch = (uint64_t)signal + 1u,
			.lineage = UINT64_C(0x9600f00000000001),
		};
		absolute_i = (uint16_t)(point.i < 0 ? -point.i : point.i);
		absolute_q = (uint16_t)(point.q < 0 ? -point.q : point.q);
		expected_magnitude = absolute_i + absolute_q == 0x4000u ?
			0x2820u : 0;
		CHECK(v32_decide_uncoded_16(
			      &observation, adapt_on_error, &decision) ==
			      V32_DECISION_OK &&
		      decision.symbol == signal &&
		      decision.equalizer.ideal_magnitude ==
			      expected_magnitude,
		      "feedback decision changed point %u", signal);
		CHECK(v32_9600_symbol_decode(
			      &expected_receiver.decoder, signal,
			      &expected_group) == V32_FRAMING_OK,
		      "feedback framing rejected point %u", signal);
		CHECK(v32_uncoded_receiver_observe(
			      &receiver, &observation, adapt_on_error,
			      &symbol) == V32_UNCODED_RECEIVER_OK,
		      "feedback receiver rejected point %u", signal);
		CHECK(symbol.group == expected_group &&
		      equalizer_decisions_match(
			      &symbol.equalizer, &decision.equalizer),
		      "feedback output changed point %u", signal);
		CHECK(receiver_states_match(&receiver, &expected_receiver),
		      "feedback state changed point %u", signal);
	}
	return true;
}

static bool test_b1_and_data_streams(void)
{
	struct test_lane lanes[TEST_LANES];

	CHECK(init_lanes(lanes) &&
	      test_feedback_composition(&lanes[0].receiver),
	      "uncoded lanes did not initialize");
	for (size_t symbol = 0; symbol < TEST_B1_SYMBOLS; ++symbol) {
		for (size_t lane = 0; lane < TEST_LANES; ++lane) {
			bool adapt_on_error =
				lanes[lane].role == V32_ANSWERING_TRANSMITTER;

			CHECK(receive_group(&lanes[lane], 15, adapt_on_error),
			      "B1 lane %zu failed at %zu", lane, symbol);
		}
	}
	for (size_t symbol = 0; symbol < TEST_DATA_SYMBOLS; ++symbol) {
		for (size_t lane = 0; lane < TEST_LANES; ++lane) {
			uint8_t input = (uint8_t)(
				fixture_random(&lanes[lane].random) & 15u);
			bool adapt_on_error = ((symbol + lane) & 1u) != 0;

			CHECK(receive_group(
				      &lanes[lane], input, adapt_on_error),
			      "data lane %zu failed at %zu", lane, symbol);
		}
	}
	for (size_t lane = 0; lane < TEST_LANES; ++lane)
		CHECK(lanes[lane].receiver.decoder.phase_quadrant ==
			      lanes[lane].encoder.phase_quadrant &&
		      scramblers_match(
			      &lanes[lane].receiver.decoder.descrambler,
			      &lanes[lane].encoder.scrambler),
		      "uncoded terminal framing diverged for lane %zu", lane);
	return true;
}

static bool test_contracts(void)
{
	struct v32_scrambler descrambler;
	struct v32_scrambler invalid;
	struct v32_uncoded_receiver receiver;
	struct v32_uncoded_receiver saved_receiver;
	struct v32_uncoded_symbol symbol;
	struct v32_uncoded_symbol saved_symbol;
	struct v32_equalizer_observation observation = {
		.i = -4096,
		.q = -4096,
		.phase = 0x1234,
		.magnitude = 0x2345,
		.epoch = 7,
		.lineage = UINT64_C(0x1122334455667788),
	};
	union test_overlap overlap = { 0 };
	union test_overlap saved_overlap;

	CHECK(v32_scrambler_init(
		      &descrambler, V32_CALLING_TRANSMITTER) == V32_FRAMING_OK,
	      "contract descrambler did not initialize");
	descrambler.history = 0x123456;
	CHECK(v32_uncoded_receiver_init(&receiver, &descrambler) ==
		      V32_UNCODED_RECEIVER_OK &&
	      v32_uncoded_receiver_is_valid(&receiver) &&
	      !v32_uncoded_receiver_is_valid(NULL),
	      "contract receiver did not initialize");
	saved_receiver = receiver;
	invalid = descrambler;
	invalid.transmitter_role = (enum v32_transmitter_role)99;
	CHECK(v32_uncoded_receiver_init(NULL, &descrambler) ==
		      V32_UNCODED_RECEIVER_INVALID_ARGUMENT &&
	      v32_uncoded_receiver_init(&receiver, NULL) ==
		      V32_UNCODED_RECEIVER_INVALID_ARGUMENT &&
	      v32_uncoded_receiver_init(&receiver, &invalid) ==
		      V32_UNCODED_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved_receiver, sizeof(receiver)) == 0,
	      "invalid initialization changed its owner");
	overlap.descrambler = descrambler;
	saved_overlap = overlap;
	CHECK(v32_uncoded_receiver_init(
		      &overlap.receiver, &overlap.descrambler) ==
		      V32_UNCODED_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&overlap, &saved_overlap, sizeof(overlap)) == 0,
	      "overlapping initialization was not transactional");
	memset(&symbol, 0x3c, sizeof(symbol));
	saved_symbol = symbol;
	observation.epoch = 0;
	CHECK(v32_uncoded_receiver_observe(
		      &receiver, &observation, true, &symbol) ==
		      V32_UNCODED_RECEIVER_INVALID_ARGUMENT &&
	      v32_uncoded_receiver_observe(
		      &receiver, NULL, true, &symbol) ==
		      V32_UNCODED_RECEIVER_INVALID_ARGUMENT &&
	      v32_uncoded_receiver_observe(
		      &receiver, &observation, true, NULL) ==
		      V32_UNCODED_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved_receiver, sizeof(receiver)) == 0 &&
	      memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0,
	      "invalid observation changed receiver/output");
	observation.epoch = 8;
	overlap.receiver = saved_receiver;
	CHECK(v32_uncoded_receiver_observe(
		      &overlap.receiver, &overlap.observation,
		      true, &symbol) == V32_UNCODED_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&overlap.receiver, &saved_receiver,
		     sizeof(saved_receiver)) == 0 &&
	      memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0,
	      "owner/input overlap was not transactional");
	overlap.receiver = saved_receiver;
	CHECK(v32_uncoded_receiver_observe(
		      &overlap.receiver, &observation, true,
		      &overlap.symbol) ==
		      V32_UNCODED_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&overlap.receiver, &saved_receiver,
		     sizeof(saved_receiver)) == 0,
	      "owner/output overlap was not transactional");
	overlap.observation = observation;
	CHECK(v32_uncoded_receiver_observe(
		      &receiver, &overlap.observation, true,
		      &overlap.symbol) ==
		      V32_UNCODED_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved_receiver, sizeof(receiver)) == 0 &&
	      memcmp(&overlap.observation, &observation,
		     sizeof(observation)) == 0,
	      "input/output overlap was not transactional");
	receiver.decoder.phase_quadrant = QPSK_QUADRANT_COUNT;
	CHECK(!v32_uncoded_receiver_is_valid(&receiver) &&
	      v32_uncoded_receiver_observe(
		      &receiver, &observation, true, &symbol) ==
		      V32_UNCODED_RECEIVER_INVALID_STATE &&
	      v32_uncoded_receiver_observe(
		      NULL, &observation, true, &symbol) ==
		      V32_UNCODED_RECEIVER_INVALID_STATE &&
	      memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0,
	      "invalid receiver state was accepted");
	return true;
}

int main(void)
{
	if (!test_b1_and_data_streams() || !test_contracts())
		return 1;
	puts("V.32 uncoded receiver tests passed");
	return 0;
}
