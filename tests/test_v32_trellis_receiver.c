// SPDX-License-Identifier: BSD-3-Clause

#include <constellation.h>
#include <trellis_receiver.h>

#include "test.h"

#include <stdint.h>
#include <string.h>

enum {
	TEST_PRIOR_GROUPS = 79,
	TEST_PAYLOAD_GROUPS = 272,
	TEST_PAYLOAD_RING = V32_TRELLIS_DECODER_TRACEBACK_SYMBOLS + 1,
	TEST_LANES = 2 * 4 * V32_TRELLIS_QUADRANT_COUNT,
};

struct test_lane {
	struct v32_scrambler transmitter_scrambler;
	struct v32_trellis_encoder encoder;
	struct v32_trellis_receiver receiver;
	uint8_t payload[TEST_PAYLOAD_RING];
	uint32_t payload_history[TEST_PAYLOAD_RING];
	uint32_t random;
	uint32_t b1_history;
	uint64_t epoch;
	uint64_t lineage;
};

union test_overlap {
	struct v32_trellis_receiver receiver;
	struct v32_scrambler descrambler;
	struct v32_equalizer_observation observation;
	struct v32_trellis_receiver_symbol symbol;
};

static uint32_t fixture_random(uint32_t *state)
{
	*state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
	return *state;
}

static bool make_continuous_framing(
	enum v32_transmitter_role role,
	struct v32_scrambler *transmitter,
	struct v32_scrambler *receiver)
{
	uint32_t random = UINT32_C(0x32b10000) ^ (uint32_t)role;

	CHECK(v32_scrambler_init(transmitter, role) == V32_FRAMING_OK &&
	      v32_scrambler_init(receiver, role) == V32_FRAMING_OK,
	      "continuous framing for role %u did not initialize", role);
	for (size_t index = 0; index < TEST_PRIOR_GROUPS; ++index) {
		uint8_t input = (uint8_t)(fixture_random(&random) & 3u);
		uint8_t plain;
		uint8_t scrambled;

		CHECK(v32_scramble_group(
			      transmitter, input, V32_4800_BITS_PER_SYMBOL,
			      &scrambled) == V32_FRAMING_OK &&
		      v32_descramble_group(
			      receiver, scrambled, V32_4800_BITS_PER_SYMBOL,
			      &plain) == V32_FRAMING_OK &&
		      plain == input,
		      "continuous framing for role %u diverged at %zu", role,
		      index);
	}
	CHECK(transmitter->history != 0 &&
	      transmitter->history == receiver->history,
	      "continuous framing for role %u did not retain history", role);
	return true;
}

static bool transmit_group(
	struct test_lane *lane, uint8_t input,
	struct v32_equalizer_observation *observation)
{
	struct v32_fixed_point point;
	uint8_t scrambled;
	uint8_t signal;
	uint8_t bits = lane->encoder.data_bits_per_symbol;

	CHECK(v32_scramble_group(
		      &lane->transmitter_scrambler, input, bits,
		      &scrambled) == V32_FRAMING_OK &&
	      v32_trellis_encode(&lane->encoder, scrambled, &signal) ==
		      V32_TRELLIS_OK &&
	      v32_trellis_fixed_signal_point(bits, signal, &point) ==
		      V32_CONSTELLATION_OK,
	      "%u-bit transmitter rejected group %u", bits, input);
	lane->epoch++;
	*observation = (struct v32_equalizer_observation){
		.i = point.i,
		.q = point.q,
		.phase = (uint16_t)((lane->epoch * 17u) & INT16_MAX),
		.magnitude = (uint16_t)(0x1000u +
					(lane->epoch & 0x0fffu)),
		.epoch = lane->epoch,
		.lineage = lane->lineage,
	};
	return true;
}

static bool init_lanes(struct test_lane lanes[TEST_LANES])
{
	size_t lane_index = 0;

	for (unsigned int role = 0; role < 2; ++role) {
		struct v32_scrambler transmitter;
		struct v32_scrambler receiver;

		CHECK(make_continuous_framing(
			      (enum v32_transmitter_role)role,
			      &transmitter, &receiver),
		      "role %u framing setup failed", role);
		for (uint8_t bits = V32_TRELLIS_MIN_DATA_BITS;
		     bits <= V32_TRELLIS_MAX_DATA_BITS; ++bits) {
			for (uint8_t prior = 0;
			     prior < V32_TRELLIS_QUADRANT_COUNT; ++prior) {
				struct test_lane *lane = &lanes[lane_index];

				lane->transmitter_scrambler = transmitter;
				lane->random = UINT32_C(0x32b10001) ^
					(uint32_t)(lane_index * 0x9e37u);
				lane->epoch = UINT64_C(0x100000) +
					lane_index * 0x10000u;
				lane->lineage = UINT64_C(0x7432000000000000) |
					(lane_index + 1u);
				CHECK(v32_trellis_encoder_init(
					      &lane->encoder, bits, prior) ==
					      V32_TRELLIS_OK &&
				      v32_trellis_receiver_init(
					      &lane->receiver, &receiver, bits,
					      prior) == V32_TRELLIS_RECEIVER_OK,
				      "lane %zu (%u/%u/%u) did not initialize",
				      lane_index, role, bits, prior);
				lane_index++;
			}
		}
	}
	return lane_index == TEST_LANES;
}

static bool test_b1_payload_boundary(void)
{
	struct test_lane lanes[TEST_LANES] = { 0 };

	CHECK(init_lanes(lanes), "trellis lanes did not initialize");
	for (size_t symbol = 0;
	     symbol < V32_TRELLIS_RECEIVER_B1_SYMBOLS; ++symbol) {
		for (size_t lane_index = 0;
		     lane_index < TEST_LANES; ++lane_index) {
			struct test_lane *lane = &lanes[lane_index];
			struct v32_equalizer_observation observation;
			uint8_t mask = (uint8_t)(
				(UINT8_C(1) <<
				 lane->encoder.data_bits_per_symbol) - 1u);
			uint8_t input = lane_index == 0 && symbol == 40 ?
					(uint8_t)(mask - 1u) : mask;

			CHECK(transmit_group(lane, input, &observation) &&
			      v32_trellis_receiver_track_b1(
				      &lane->receiver, &observation) ==
				      V32_TRELLIS_RECEIVER_OK &&
			      !v32_trellis_receiver_b1_is_confirmed(
				      &lane->receiver),
			      "lane %zu rejected B1 symbol %zu", lane_index,
			      symbol);
		}
	}
	for (size_t lane_index = 0; lane_index < TEST_LANES; ++lane_index)
		lanes[lane_index].b1_history =
			lanes[lane_index].transmitter_scrambler.history;

	for (size_t symbol = 0; symbol < TEST_PAYLOAD_GROUPS; ++symbol) {
		for (size_t lane_index = 0;
		     lane_index < TEST_LANES; ++lane_index) {
			struct test_lane *lane = &lanes[lane_index];
			struct v32_equalizer_observation observation;
			struct v32_trellis_receiver_symbol received = {
				.group = UINT8_MAX,
				.ready = true,
			};
			size_t slot = symbol % TEST_PAYLOAD_RING;
			uint8_t mask = (uint8_t)(
				(UINT8_C(1) <<
				 lane->encoder.data_bits_per_symbol) - 1u);

			lane->payload[slot] = (uint8_t)(
				fixture_random(&lane->random) & mask);
			CHECK(transmit_group(
				      lane, lane->payload[slot], &observation),
			      "lane %zu payload %zu did not transmit", lane_index,
			      symbol);
			lane->payload_history[slot] =
				lane->transmitter_scrambler.history;
			CHECK(v32_trellis_receiver_decode(
				      &lane->receiver, &observation, &received) ==
				      V32_TRELLIS_RECEIVER_OK,
			      "lane %zu payload %zu did not decode", lane_index,
			      symbol);
			if (symbol < V32_TRELLIS_DECODER_TRACEBACK_SYMBOLS) {
				CHECK(!received.ready && received.group == 0,
				      "lane %zu exposed delayed B1 at %zu",
				      lane_index, symbol);
				if (symbol + 1u ==
				    V32_TRELLIS_DECODER_TRACEBACK_SYMBOLS) {
					CHECK(v32_trellis_receiver_b1_is_confirmed(
						      &lane->receiver) ==
						      (lane_index != 0) &&
					      lane->receiver.descrambler.history ==
						      lane->b1_history,
					      "lane %zu B1 result or history changed",
					      lane_index);
				}
				continue;
			}
			{
				size_t expected_symbol = symbol -
					V32_TRELLIS_DECODER_TRACEBACK_SYMBOLS;
				size_t expected_slot =
					expected_symbol % TEST_PAYLOAD_RING;

				CHECK(received.ready &&
				      received.group ==
					      lane->payload[expected_slot] &&
				      lane->receiver.descrambler.history ==
					      lane->payload_history[expected_slot],
				      "lane %zu payload %zu was %u, expected %u",
				      lane_index, expected_symbol, received.group,
				      lane->payload[expected_slot]);
			}
		}
	}
	for (size_t lane_index = 0; lane_index < TEST_LANES; ++lane_index)
		CHECK(v32_trellis_receiver_is_valid(
			      &lanes[lane_index].receiver),
		      "lane %zu ended invalid", lane_index);
	return true;
}

static bool prime_b1(
	struct v32_trellis_receiver *receiver,
	struct v32_equalizer_observation *last)
{
	struct test_lane lane = { 0 };
	struct v32_scrambler receive_scrambler;
	uint8_t mark = (UINT8_C(1) << V32_TRELLIS_MIN_DATA_BITS) - 1u;

	CHECK(v32_scrambler_init(
		      &lane.transmitter_scrambler,
		      V32_CALLING_TRANSMITTER) == V32_FRAMING_OK &&
	      v32_scrambler_init(
		      &receive_scrambler,
		      V32_CALLING_TRANSMITTER) == V32_FRAMING_OK &&
	      v32_trellis_encoder_init(
		      &lane.encoder, V32_TRELLIS_MIN_DATA_BITS, 0) ==
		      V32_TRELLIS_OK &&
	      v32_trellis_receiver_init(
		      &lane.receiver, &receive_scrambler,
		      V32_TRELLIS_MIN_DATA_BITS, 0) ==
		      V32_TRELLIS_RECEIVER_OK,
	      "contract B1 fixture did not initialize");
	lane.epoch = 1;
	lane.lineage = UINT64_C(0x7432c00000000001);
	for (size_t index = 0;
	     index < V32_TRELLIS_RECEIVER_B1_SYMBOLS; ++index) {
		CHECK(transmit_group(&lane, mark, last) &&
		      v32_trellis_receiver_track_b1(
			      &lane.receiver, last) ==
			      V32_TRELLIS_RECEIVER_OK,
		      "contract B1 fixture stopped at %zu", index);
	}
	*receiver = lane.receiver;
	return true;
}

static bool test_contracts(void)
{
	struct v32_scrambler descrambler;
	struct v32_scrambler invalid_descrambler;
	struct v32_trellis_receiver receiver;
	struct v32_trellis_receiver saved_receiver;
	struct v32_trellis_receiver ready_receiver;
	struct v32_trellis_receiver_symbol symbol;
	struct v32_trellis_receiver_symbol saved_symbol;
	struct v32_equalizer_observation observation = {
		.i = -4096,
		.q = 4096,
		.phase = 0x1234,
		.magnitude = 0x2345,
		.epoch = 7,
		.lineage = UINT64_C(0x7432c00000000002),
	};
	union test_overlap overlap = { 0 };
	union test_overlap saved_overlap;

	CHECK(v32_scrambler_init(
		      &descrambler, V32_CALLING_TRANSMITTER) ==
		      V32_FRAMING_OK,
	      "contract descrambler did not initialize");
	descrambler.history = 0x123456;
	CHECK(v32_trellis_receiver_init(
		      &receiver, &descrambler, V32_TRELLIS_MIN_DATA_BITS,
		      0) == V32_TRELLIS_RECEIVER_OK &&
	      v32_trellis_receiver_is_valid(&receiver) &&
	      !v32_trellis_receiver_is_valid(NULL) &&
	      !v32_trellis_receiver_b1_is_confirmed(&receiver),
	      "contract receiver did not initialize");
	saved_receiver = receiver;
	invalid_descrambler = descrambler;
	invalid_descrambler.transmitter_role =
		(enum v32_transmitter_role)99;
	CHECK(v32_trellis_receiver_init(
		      NULL, &descrambler, V32_TRELLIS_MIN_DATA_BITS, 0) ==
		      V32_TRELLIS_RECEIVER_INVALID_ARGUMENT &&
	      v32_trellis_receiver_init(
		      &receiver, NULL, V32_TRELLIS_MIN_DATA_BITS, 0) ==
		      V32_TRELLIS_RECEIVER_INVALID_ARGUMENT &&
	      v32_trellis_receiver_init(
		      &receiver, &invalid_descrambler,
		      V32_TRELLIS_MIN_DATA_BITS, 0) ==
		      V32_TRELLIS_RECEIVER_INVALID_ARGUMENT &&
	      v32_trellis_receiver_init(
		      &receiver, &descrambler, 2, 0) ==
		      V32_TRELLIS_RECEIVER_INVALID_ARGUMENT &&
	      v32_trellis_receiver_init(
		      &receiver, &descrambler, 7, 0) ==
		      V32_TRELLIS_RECEIVER_INVALID_ARGUMENT &&
	      v32_trellis_receiver_init(
		      &receiver, &descrambler, V32_TRELLIS_MIN_DATA_BITS,
		      V32_TRELLIS_QUADRANT_COUNT) ==
		      V32_TRELLIS_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved_receiver, sizeof(receiver)) == 0,
	      "invalid initialization changed its owner");
	overlap.descrambler = descrambler;
	saved_overlap = overlap;
	CHECK(v32_trellis_receiver_init(
		      &overlap.receiver, &overlap.descrambler,
		      V32_TRELLIS_MIN_DATA_BITS, 0) ==
		      V32_TRELLIS_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&overlap, &saved_overlap, sizeof(overlap)) == 0,
	      "overlapping initialization was not transactional");
	observation.epoch = 0;
	CHECK(v32_trellis_receiver_track_b1(
		      &receiver, &observation) ==
		      V32_TRELLIS_RECEIVER_INVALID_ARGUMENT &&
	      v32_trellis_receiver_track_b1(NULL, &observation) ==
		      V32_TRELLIS_RECEIVER_INVALID_STATE &&
	      v32_trellis_receiver_track_b1(&receiver, NULL) ==
		      V32_TRELLIS_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved_receiver, sizeof(receiver)) == 0,
	      "invalid B1 observation changed its owner");
	overlap.receiver = saved_receiver;
	CHECK(v32_trellis_receiver_track_b1(
		      &overlap.receiver,
		      (const struct v32_equalizer_observation *)(const void *)
			      &overlap.receiver) ==
		      V32_TRELLIS_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&overlap.receiver, &saved_receiver,
		     sizeof(saved_receiver)) == 0,
	      "B1 owner/input overlap was not transactional");
	memset(&symbol, 0x5a, sizeof(symbol));
	saved_symbol = symbol;
	observation.epoch = 8;
	CHECK(v32_trellis_receiver_decode(
		      &receiver, &observation, &symbol) ==
		      V32_TRELLIS_RECEIVER_INVALID_STATE &&
	      memcmp(&receiver, &saved_receiver, sizeof(receiver)) == 0 &&
	      memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0,
	      "early payload decode changed state or output");
	CHECK(prime_b1(&ready_receiver, &observation),
	      "ready B1 contract fixture failed");
	saved_receiver = ready_receiver;
	CHECK(v32_trellis_receiver_track_b1(
		      &ready_receiver, &observation) ==
		      V32_TRELLIS_RECEIVER_INVALID_STATE &&
	      v32_trellis_receiver_decode(
		      &ready_receiver, NULL, &symbol) ==
		      V32_TRELLIS_RECEIVER_INVALID_ARGUMENT &&
	      v32_trellis_receiver_decode(
		      &ready_receiver, &observation, NULL) ==
		      V32_TRELLIS_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&ready_receiver, &saved_receiver,
		     sizeof(saved_receiver)) == 0 &&
	      memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0,
	      "terminal B1 or null payload changed state/output");
	overlap.receiver = ready_receiver;
	CHECK(v32_trellis_receiver_decode(
		      &overlap.receiver,
		      (const struct v32_equalizer_observation *)(const void *)
			      &overlap.receiver,
		      &symbol) == V32_TRELLIS_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&overlap.receiver, &ready_receiver,
		     sizeof(ready_receiver)) == 0 &&
	      memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0,
	      "payload owner/input overlap was not transactional");
	overlap.receiver = ready_receiver;
	CHECK(v32_trellis_receiver_decode(
		      &overlap.receiver, &observation,
		      (struct v32_trellis_receiver_symbol *)(void *)
			      &overlap.receiver) ==
		      V32_TRELLIS_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&overlap.receiver, &ready_receiver,
		     sizeof(ready_receiver)) == 0,
	      "payload owner/output overlap was not transactional");
	overlap.observation = observation;
	saved_overlap = overlap;
	CHECK(v32_trellis_receiver_decode(
		      &ready_receiver, &overlap.observation,
		      &overlap.symbol) ==
		      V32_TRELLIS_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&ready_receiver, &saved_receiver,
		     sizeof(saved_receiver)) == 0 &&
	      memcmp(&overlap, &saved_overlap, sizeof(overlap)) == 0,
	      "payload input/output overlap was not transactional");
	receiver = saved_receiver;
	receiver.decoder.paths[0][0].predecessor =
		V32_TRELLIS_STATE_COUNT;
	CHECK(!v32_trellis_receiver_is_valid(&receiver) &&
	      v32_trellis_receiver_decode(
		      &receiver, &observation, &symbol) ==
		      V32_TRELLIS_RECEIVER_INVALID_STATE &&
	      !v32_trellis_receiver_b1_is_confirmed(&receiver) &&
	      memcmp(&symbol, &saved_symbol, sizeof(symbol)) == 0,
	      "corrupt decoder state was accepted");
	return true;
}

int main(void)
{
	if (!test_b1_payload_boundary() || !test_contracts())
		return 1;
	puts("V.32 trellis receiver tests passed");
	return 0;
}
