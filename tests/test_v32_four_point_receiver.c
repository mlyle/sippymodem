// SPDX-License-Identifier: BSD-3-Clause

#include <uncoded_receiver.h>

#include <constellation.h>
#include <rate_signal.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

enum {
	TEST_TRN_SYMBOLS = 257,
	TEST_DATA_SYMBOLS = 256,
};

static uint32_t fixture_random(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static bool make_conditioned_seed(
	enum v32_transmitter_role role, uint8_t initial_quadrant,
	struct v32_symbol_encoder *encoder,
	struct v32_symbol_decoder *decoder, uint32_t *random)
{
	CHECK(v32_symbol_encoder_init(encoder, role, initial_quadrant) ==
		      V32_FRAMING_OK &&
	      v32_symbol_decoder_init(decoder, role, initial_quadrant) ==
		      V32_FRAMING_OK,
	      "conditioning seed did not initialize");
	for (size_t index = 0; index < TEST_TRN_SYMBOLS; ++index) {
		uint8_t input = (uint8_t)(fixture_random(random) & 3u);
		uint8_t output;
		uint8_t quadrant;

		CHECK(v32_trn_symbol_encode(encoder, input, &quadrant) ==
			      V32_FRAMING_OK &&
		      v32_trn_symbol_decode(decoder, quadrant, &output) ==
			      V32_FRAMING_OK &&
		      output == input,
		      "conditioning seed diverged at symbol %zu", index);
	}
	return true;
}

static struct v32_equalizer_observation observation_for_quadrant(
	uint8_t quadrant, uint64_t epoch, uint64_t lineage)
{
	struct v32_fixed_point point = { 0 };

	(void)v32_4800_fixed_point(quadrant, &point);
	return (struct v32_equalizer_observation){
		.i = point.i,
		.q = point.q,
		.phase = (uint16_t)(0x1000u + (epoch & 0x0fffu)),
		.magnitude = 0x2345,
		.epoch = epoch,
		.lineage = lineage,
	};
}

static bool symbol_is_exact(
	const struct v32_uncoded_symbol *symbol,
	const struct v32_equalizer_observation *observation,
	uint8_t expected_dibit, uint8_t expected_quadrant,
	bool expected_adaptation)
{
	static const uint16_t PHASE[4] = {
		0x468d, 0x668d, 0x068e, 0x268d,
	};

	return symbol->group == expected_dibit &&
	       memcmp(&symbol->equalizer.observation,
		      observation, sizeof(*observation)) == 0 &&
	       symbol->equalizer.ideal_phase == PHASE[expected_quadrant] &&
	       symbol->equalizer.ideal_magnitude == 0x3299 &&
	       symbol->equalizer.adapt_on_error == expected_adaptation;
}

static bool receive_dibit(
	struct v32_symbol_encoder *encoder,
	struct v32_uncoded_receiver *receiver,
	uint8_t input, bool adapt_on_error,
	uint64_t *epoch, uint64_t lineage,
	struct v32_uncoded_symbol *symbol)
{
	struct v32_equalizer_observation observation;
	uint8_t quadrant;

	CHECK(v32_4800_symbol_encode(encoder, input, &quadrant) ==
		      V32_FRAMING_OK,
	      "four-point transmitter rejected dibit %u", input);
	observation = observation_for_quadrant(quadrant, ++*epoch, lineage);
	CHECK(v32_uncoded_receiver_observe(
		      receiver, V32_4800_BITS_PER_SYMBOL,
		      &observation, adapt_on_error, symbol) ==
		      V32_UNCODED_RECEIVER_OK &&
	      symbol_is_exact(
		      symbol, &observation, input, quadrant, adapt_on_error),
	      "four-point receive decision changed at epoch %llu",
	      (unsigned long long)*epoch);
	return true;
}

static bool receive_word(
	struct v32_symbol_encoder *encoder,
	struct v32_uncoded_receiver *receiver,
	struct v32_rate_signal_detector *detector,
	uint16_t word, bool adapt_on_error,
	enum v32_rate_signal_event expected_event,
	uint64_t *epoch, uint64_t lineage)
{
	struct v32_rate_offer offer = { 0 };
	enum v32_rate_signal_event event = V32_RATE_SIGNAL_NO_EVENT;

	for (unsigned int index = 0;
	     index < V32_RATE_SIGNAL_DIBITS; ++index) {
		struct v32_uncoded_symbol symbol;
		uint8_t input;

		CHECK(v32_rate_signal_dibit(word, index, &input) ==
			      V32_RATE_SIGNAL_OK &&
		      receive_dibit(
			      encoder, receiver, input, adapt_on_error,
			      epoch, lineage, &symbol) &&
		      v32_rate_signal_detector_observe(
			      detector, symbol.group, &event, &offer) ==
			      V32_RATE_SIGNAL_OK,
		      "rate/E word 0x%04x failed at dibit %u", word, index);
		CHECK(index + 1 == V32_RATE_SIGNAL_DIBITS ||
			      event == V32_RATE_SIGNAL_NO_EVENT,
		      "rate/E word 0x%04x was reported early", word);
	}
	CHECK(event == expected_event,
	      "rate/E word 0x%04x reported event %u, expected %u",
	      word, (unsigned int)event, (unsigned int)expected_event);
	if (event != V32_RATE_SIGNAL_NO_EVENT)
		CHECK(offer.rates == V32_RATE_CAPABILITY_4800 &&
			      !offer.trellis_at_highest_rate,
		      "rate/E offer changed after fixed receive framing");
	return true;
}

static bool test_conditioning_rate_and_data_continuity(void)
{
	for (unsigned int role = 0; role < 2; ++role) {
		for (uint8_t initial = 0; initial < 4; ++initial) {
			struct v32_symbol_encoder encoder;
			struct v32_symbol_decoder seed;
			struct v32_uncoded_receiver receiver;
			struct v32_rate_signal_detector detector;
			uint32_t random = UINT32_C(0x6d8eb713) ^
					  (uint32_t)(role * 0x100u + initial);
			uint64_t epoch = (uint64_t)(role * 10000u +
						    initial * 1000u + 1u);
			uint64_t lineage = UINT64_C(0x32f04e11d9000000) |
					   (uint64_t)(role * 4u + initial);

			CHECK(make_conditioned_seed(
				      (enum v32_transmitter_role)role, initial,
				      &encoder, &seed, &random) &&
			      v32_uncoded_receiver_init(&receiver, &seed) ==
				      V32_UNCODED_RECEIVER_OK &&
			      v32_rate_signal_detector_init(&detector) ==
				      V32_RATE_SIGNAL_OK,
			      "four-point fixture %u/%u did not initialize",
			      role, initial);
			CHECK(receive_word(
				      &encoder, &receiver, &detector, 0x0511,
				      true, V32_RATE_SIGNAL_NO_EVENT,
				      &epoch, lineage) &&
			      receive_word(
				      &encoder, &receiver, &detector, 0x0511,
				      true, V32_RATE_SIGNAL_DETECTED,
				      &epoch, lineage) &&
			      receive_word(
				      &encoder, &receiver, &detector, 0xf511,
				      false, V32_END_SIGNAL_DETECTED,
				      &epoch, lineage),
			      "four-point rate/E continuity failed at %u/%u",
			      role, initial);
			for (size_t index = 0; index < TEST_DATA_SYMBOLS;
			     ++index) {
				struct v32_uncoded_symbol symbol;
				uint8_t input = (uint8_t)(
					fixture_random(&random) & 3u);

				CHECK(receive_dibit(
					      &encoder, &receiver, input,
					      (index & 1u) != 0,
					      &epoch, lineage, &symbol),
				      "four-point data failed at %u/%u/%zu",
				      role, initial, index);
			}
			CHECK(receiver.decoder.phase_quadrant ==
				      encoder.phase_quadrant &&
			      receiver.decoder.descrambler.history ==
				      encoder.scrambler.history &&
			      receiver.decoder.descrambler.transmitter_role ==
				      encoder.scrambler.transmitter_role,
			      "four-point endpoints diverged at %u/%u",
			      role, initial);
		}
	}
	return true;
}

static bool test_contracts(void)
{
	struct v32_symbol_decoder seed;
	struct v32_symbol_decoder invalid_seed;
	struct v32_uncoded_receiver receiver;
	struct v32_uncoded_receiver saved_receiver;
	struct v32_uncoded_symbol symbol;
	struct v32_uncoded_symbol saved_symbol;
	struct v32_equalizer_observation observation;
	union {
		struct v32_uncoded_receiver receiver;
		struct v32_symbol_decoder seed;
	} init_overlap;
	union {
		struct v32_uncoded_receiver receiver;
		struct v32_equalizer_observation observation;
	} owner_input;
	union {
		struct v32_uncoded_receiver receiver;
		struct v32_uncoded_symbol symbol;
	} owner_output;
	union {
		struct v32_equalizer_observation observation;
		struct v32_uncoded_symbol symbol;
	} input_output;

	CHECK(v32_symbol_decoder_init(
		      &seed, V32_CALLING_TRANSMITTER, 2) == V32_FRAMING_OK &&
	      v32_uncoded_receiver_init(&receiver, &seed) ==
		      V32_UNCODED_RECEIVER_OK,
	      "contract receiver did not initialize");
	invalid_seed = seed;
	invalid_seed.phase_quadrant = 4;
	memset(&symbol, 0xa5, sizeof(symbol));
	saved_symbol = symbol;
	saved_receiver = receiver;
	CHECK(v32_uncoded_receiver_init(NULL, &seed) ==
		      V32_UNCODED_RECEIVER_INVALID_ARGUMENT &&
	      v32_uncoded_receiver_init(&receiver, NULL) ==
		      V32_UNCODED_RECEIVER_INVALID_ARGUMENT &&
	      v32_uncoded_receiver_init(&receiver, &invalid_seed) ==
		      V32_UNCODED_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved_receiver, sizeof(receiver)) == 0,
	      "receiver initialization rejection changed its owner");
	init_overlap.seed = seed;
	CHECK(v32_uncoded_receiver_init(
		      &init_overlap.receiver, &init_overlap.seed) ==
		      V32_UNCODED_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&init_overlap.seed, &seed, sizeof(seed)) == 0,
	      "overlapping receiver seed was accepted or changed");

	observation = observation_for_quadrant(
		0, 7, UINT64_C(0x1122334455667788));
	observation.epoch = 0;
	CHECK(v32_uncoded_receiver_observe(
		      &receiver, V32_4800_BITS_PER_SYMBOL,
		      &observation, true, &symbol) ==
		      V32_UNCODED_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved_receiver, sizeof(receiver)) == 0 &&
	      memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0 &&
	      v32_uncoded_receiver_observe(
		      &receiver, V32_4800_BITS_PER_SYMBOL,
		      NULL, true, &symbol) ==
		      V32_UNCODED_RECEIVER_INVALID_ARGUMENT &&
	      v32_uncoded_receiver_observe(
		      &receiver, V32_4800_BITS_PER_SYMBOL,
		      &observation, true, NULL) ==
		      V32_UNCODED_RECEIVER_INVALID_ARGUMENT,
	      "invalid observation/output changed receiver state");

	owner_input.receiver = saved_receiver;
	CHECK(v32_uncoded_receiver_observe(
		      &owner_input.receiver, V32_4800_BITS_PER_SYMBOL,
		      &owner_input.observation, true, &symbol) ==
		      V32_UNCODED_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&owner_input.receiver, &saved_receiver,
		     sizeof(saved_receiver)) == 0 &&
	      memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0,
	      "owner/input overlap was not transactional");
	owner_output.receiver = saved_receiver;
	observation.epoch = 8;
	CHECK(v32_uncoded_receiver_observe(
		      &owner_output.receiver, V32_4800_BITS_PER_SYMBOL,
		      &observation, true, &owner_output.symbol) ==
		      V32_UNCODED_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&owner_output.receiver, &saved_receiver,
		     sizeof(saved_receiver)) == 0,
	      "owner/output overlap was not transactional");
	input_output.observation = observation;
	CHECK(v32_uncoded_receiver_observe(
		      &receiver, V32_4800_BITS_PER_SYMBOL,
		      &input_output.observation, true, &input_output.symbol) ==
		      V32_UNCODED_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved_receiver, sizeof(receiver)) == 0 &&
	      memcmp(&input_output.observation, &observation,
		     sizeof(observation)) == 0,
	      "input/output overlap was not transactional");

	receiver.decoder.phase_quadrant = 4;
	CHECK(!v32_uncoded_receiver_is_valid(&receiver) &&
	      v32_uncoded_receiver_observe(
		      &receiver, V32_4800_BITS_PER_SYMBOL,
		      &observation, true, &symbol) ==
		      V32_UNCODED_RECEIVER_INVALID_STATE &&
	      memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0 &&
	      v32_uncoded_receiver_observe(
		      NULL, V32_4800_BITS_PER_SYMBOL,
		      &observation, true, &symbol) ==
		      V32_UNCODED_RECEIVER_INVALID_STATE,
	      "corrupt receiver state was accepted");
	return true;
}

int main(void)
{
	if (!test_conditioning_rate_and_data_continuity() ||
	    !test_contracts())
		return 1;
	puts("V.32 four-point receiver tests passed");
	return 0;
}
