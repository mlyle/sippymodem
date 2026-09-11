// SPDX-License-Identifier: BSD-3-Clause

#include <v90_phase4_cp_rx.h>
#include <v90_phase4_cp_tx.h>

#include "test.h"

#include <math.h>
#include <string.h>

static void add_ucode(
	struct pcm_constellation *constellation, uint8_t ucode)
{
	constellation->ucode_mask[ucode / 8u] |=
		(uint8_t)(1u << (ucode % 8u));
}

static struct v90_cp_parameters parameters(
	enum v90_cp_kind kind, uint8_t count,
	bool codec_constellations_differ, uint8_t variant)
{
	struct v90_cp_parameters selected = {
		.kind = kind,
		.law = variant % 2u == 0 ? G711_LAW_PCMU : G711_LAW_PCMA,
		.upstream_rate_mask = (uint16_t)(0x1555u ^ variant),
		.trn1d_ratio_q3_13 = (uint16_t)(0x2000u + variant),
		.shaping_a1_q1_6 = (int8_t)(-16 - variant),
		.shaping_a2_q1_6 = (int8_t)(8 + variant),
		.shaping_b1_q1_6 = (int8_t)(-1 - variant),
		.shaping_b2_q1_6 = (int8_t)(31 - variant),
		.selected_rate_index = (uint8_t)(1u + variant),
		.sign_redundancy = (uint8_t)(variant % 4u),
		.spectral_lookahead = (uint8_t)((variant + 1u) % 4u),
		.constellation_count = count,
		.request_silence = variant % 2u != 0,
		.acknowledges_peer = variant % 3u == 0,
		.codec_constellations_differ = codec_constellations_differ,
	};

	for (size_t index = 0; index < count; ++index) {
		add_ucode(
			&selected.transmit_constellations[index],
			(uint8_t)(index * 19u + variant));
		add_ucode(
			&selected.transmit_constellations[index],
			(uint8_t)(127u - index));
		if (codec_constellations_differ) {
			add_ucode(
				&selected.codec_constellations[index],
				(uint8_t)(index * 7u + variant));
			add_ucode(
				&selected.codec_constellations[index],
				(uint8_t)(126u - index));
		}
	}
	for (size_t interval = 0;
	     interval < V90_CP_FRAME_INTERVALS; ++interval)
		selected.frame_constellation[interval] =
			(uint8_t)(interval % count);
	return selected;
}

static bool parameters_are_equal(
	const struct v90_cp_parameters *left,
	const struct v90_cp_parameters *right)
{
	uint8_t left_frame[V90_CP_MAX_FRAME_BYTES];
	uint8_t right_frame[V90_CP_MAX_FRAME_BYTES];
	size_t left_bits = 0;
	size_t right_bits = 0;

	return v90_cp_encode(
		       left, left_frame, sizeof(left_frame), &left_bits) ==
		       V90_CP_OK &&
	       v90_cp_encode(
		       right, right_frame, sizeof(right_frame), &right_bits) ==
		       V90_CP_OK &&
	       left_bits == right_bits &&
	       memcmp(left_frame, right_frame,
		      (left_bits + 7u) / 8u) == 0;
}

static bool transfer_record(
	struct v90_phase4_cp_tx *tx,
	struct v90_phase4_cp_rx *rx,
	bool receive_points, size_t corrupt_symbol,
	uint8_t rotation_offset,
	struct v90_cp_receive_event *event)
{
	struct v90_cp_receive_event captured = { 0 };
	bool complete = false;
	size_t symbol = 0;

	while (!complete) {
		struct qam_point point;
		struct v90_cp_receive_event emitted;
		uint8_t quarter;
		uint8_t rotation;

		CHECK(symbol < V90_PHASE4_CP_MAX_4_POINT_SYMBOLS &&
		      v90_phase4_cp_tx_next(
			      tx, &point, &quarter, &rotation, &complete) ==
			      V90_PHASE4_CP_TX_OK,
		      "CP receive fixture stopped at symbol %zu", symbol);
		if (symbol == corrupt_symbol) {
			if (tx->constellation == V34_CONSTELLATION_16_POINT)
				quarter ^= 1u;
			else
				rotation ^= 1u;
		}
		rotation = (uint8_t)((rotation + rotation_offset) & 3u);
		if (receive_points) {
			CHECK(v34_training_constellation_point(
				      tx->constellation, quarter, rotation, &point) ==
				      V34_CONSTELLATION_OK,
			      "CP point rotation failed at symbol %zu", symbol);
			point.in_phase += symbol % 2u == 0 ? 0.02 : -0.02;
			point.quadrature += symbol % 3u == 0 ? -0.015 : 0.015;
			CHECK(v90_phase4_cp_rx_receive_point(
				      rx, &point, &emitted) == V90_CP_OK,
			      "CP point receiver stopped at symbol %zu", symbol);
		} else {
			CHECK(v90_phase4_cp_rx_receive_decision(
				      rx, quarter, rotation, &emitted) == V90_CP_OK,
			      "CP decision receiver stopped at symbol %zu", symbol);
		}
		if (emitted.type != V90_CP_RECEIVE_NO_EVENT) {
			CHECK(captured.type == V90_CP_RECEIVE_NO_EVENT,
			      "CP record emitted multiple events");
			captured = emitted;
		}
		++symbol;
	}
	*event = captured;
	return true;
}

static bool test_first_symbol_and_continuous_records(void)
{
	static const enum v34_station_role roles[] = {
		V34_CALLING_STATION,
		V34_ANSWERING_STATION,
	};
	static const enum v34_constellation_size constellations[] = {
		V34_CONSTELLATION_4_POINT,
		V34_CONSTELLATION_16_POINT,
	};
	struct v90_cp_parameters training = parameters(
		V90_CP_TRAINING, 1, false, 0);
	struct v90_cp_parameters data = parameters(
		V90_CP_DATA, V90_CP_MAX_CONSTELLATIONS, true, 2);

	for (size_t role = 0; role < ARRAY_LENGTH(roles); ++role) {
		for (size_t constellation = 0;
		     constellation < ARRAY_LENGTH(constellations);
		     ++constellation) {
			struct v90_phase4_cp_tx tx;
			struct v90_phase4_cp_rx rx;
			struct v90_cp_receive_event event;

			CHECK(v90_phase4_cp_tx_init(
				      &tx, roles[role], constellations[constellation],
				      &training) == V90_PHASE4_CP_TX_OK &&
			      v90_phase4_cp_rx_init(
				      &rx, roles[role], constellations[constellation]) ==
				      V90_CP_OK &&
			      transfer_record(
				      &tx, &rx, true, SIZE_MAX, 0, &event) &&
			      event.type == V90_CP_RECEIVE_FRAME &&
			      event.frame_result == V90_CP_OK &&
			      event.observed_bits == V90_CP_MIN_FRAME_BITS &&
			      parameters_are_equal(&event.parameters, &training),
			      "first CPt changed for role/constellation %zu/%zu",
			      role, constellation);
			CHECK(v90_phase4_cp_tx_start_next_record(
				      &tx, &data) == V90_PHASE4_CP_TX_OK &&
			      transfer_record(
				      &tx, &rx, false, SIZE_MAX, 0, &event) &&
			      event.type == V90_CP_RECEIVE_FRAME &&
			      event.frame_result == V90_CP_OK &&
			      event.observed_bits == V90_CP_MAX_FRAME_BITS &&
			      parameters_are_equal(&event.parameters, &data) &&
			      v90_phase4_cp_rx_is_valid(&rx),
			      "continuous CP changed for role/constellation %zu/%zu",
			      role, constellation);
		}
	}
	return true;
}

static bool test_corruption_and_reacquisition(void)
{
	static const enum v34_constellation_size constellations[] = {
		V34_CONSTELLATION_4_POINT,
		V34_CONSTELLATION_16_POINT,
	};
	struct v90_cp_parameters training = parameters(
		V90_CP_TRAINING, 1, false, 0);

	for (size_t constellation = 0;
	     constellation < ARRAY_LENGTH(constellations);
	     ++constellation) {
		struct v90_phase4_cp_tx tx;
		struct v90_phase4_cp_rx rx;
		struct v90_cp_receive_event event;
		size_t corrupt_symbol =
			constellations[constellation] ==
					V34_CONSTELLATION_4_POINT ?
				80u :
				40u;

		CHECK(v90_phase4_cp_tx_init(
			      &tx, V34_CALLING_STATION,
			      constellations[constellation], &training) ==
			      V90_PHASE4_CP_TX_OK &&
		      v90_phase4_cp_rx_init(
			      &rx, V34_CALLING_STATION,
			      constellations[constellation]) == V90_CP_OK &&
		      transfer_record(
			      &tx, &rx, false, corrupt_symbol, 0, &event) &&
		      event.type == V90_CP_RECEIVE_REJECTED &&
		      event.frame_result == V90_CP_CRC_MISMATCH,
		      "corrupt CP was not rejected for constellation %zu",
		      constellation);
		CHECK(v90_phase4_cp_tx_start_next_record(
			      &tx, &training) == V90_PHASE4_CP_TX_OK &&
		      transfer_record(
			      &tx, &rx, false, SIZE_MAX, 0, &event) &&
		      event.type == V90_CP_RECEIVE_FRAME &&
		      event.frame_result == V90_CP_OK &&
		      parameters_are_equal(&event.parameters, &training),
		      "CP receiver did not reacquire constellation %zu",
		      constellation);
	}
	return true;
}

static bool test_fixed_phase_ambiguity(void)
{
	static const enum v34_constellation_size constellations[] = {
		V34_CONSTELLATION_4_POINT,
		V34_CONSTELLATION_16_POINT,
	};
	struct v90_cp_parameters training = parameters(
		V90_CP_TRAINING, 1, false, 0);

	for (size_t constellation = 0;
	     constellation < ARRAY_LENGTH(constellations); ++constellation) {
		for (uint8_t offset = 0; offset < 4u; ++offset) {
			struct v90_phase4_cp_tx tx;
			struct v90_phase4_cp_rx rx;
			struct v90_cp_receive_event event;

			CHECK(v90_phase4_cp_tx_init(
				      &tx, V34_CALLING_STATION,
				      constellations[constellation], &training) ==
				      V90_PHASE4_CP_TX_OK &&
			      v90_phase4_cp_rx_init(
				      &rx, V34_CALLING_STATION,
				      constellations[constellation]) == V90_CP_OK,
			      "CP phase-offset fixture %zu/%u did not initialize",
			      constellation, offset);
			rx.previous_clockwise_quarter_turns = offset;
			CHECK(transfer_record(
				      &tx, &rx, false, SIZE_MAX, offset, &event) &&
			      event.type == V90_CP_RECEIVE_FRAME &&
			      event.frame_result == V90_CP_OK &&
			      parameters_are_equal(&event.parameters, &training),
			      "CP fixed phase offset %zu/%u changed the record",
			      constellation, offset);
		}
	}
	return true;
}

static bool test_two_rejections_in_one_symbol(void)
{
	const enum v34_station_role roles[] = {
		V34_CALLING_STATION, V34_ANSWERING_STATION,
	};
	struct v90_cp_parameters selected =
		parameters(V90_CP_TRAINING, 1u, false, 0u);

	/* Sixteen consecutive constellation bits precede a mandatory start zero.
	 * Corrupting it and the next candidate's type bit rejects twice within
	 * one four-bit symbol: that is line noise, not an invalid API call. */
	for (uint8_t code = 0u; code < 16u; ++code)
		add_ucode(&selected.transmit_constellations[0], code);
	selected.acknowledges_peer = false;
	for (size_t role = 0u; role < ARRAY_LENGTH(roles); ++role) {
		for (unsigned int points = 0u; points < 2u; ++points) {
			struct v90_phase4_cp_tx tx;
			struct v90_phase4_cp_rx rx;
			struct v90_cp_receive_event event;
			struct v90_cp_parameters retry = selected;
			bool complete = false;
			unsigned int rejected = 0u;

			CHECK(v90_phase4_cp_tx_init(
				      &tx, roles[role], V34_CONSTELLATION_16_POINT,
				      &selected) == V90_PHASE4_CP_TX_OK &&
			      v90_phase4_cp_rx_init(
				      &rx, roles[role], V34_CONSTELLATION_16_POINT) ==
				      V90_CP_OK,
			      "noisy-symbol fixture did not initialize");
			for (size_t symbol = 0u; !complete; ++symbol) {
				struct qam_point point;
				uint8_t quarter, rotation;
				enum v90_cp_result result;

				CHECK(symbol < V90_PHASE4_CP_MAX_4_POINT_SYMBOLS &&
				      v90_phase4_cp_tx_next(
					      &tx, &point, &quarter, &rotation, &complete) ==
					      V90_PHASE4_CP_TX_OK,
				      "noisy-symbol transmitter stopped");
				if (symbol == 38u) {
					quarter ^= 2u;
					rotation ^= 2u;
				}
				if (points != 0u) {
					CHECK(v34_training_constellation_point(
						      V34_CONSTELLATION_16_POINT, quarter,
						      rotation, &point) == V34_CONSTELLATION_OK,
					      "noisy point did not map");
					result = v90_phase4_cp_rx_receive_point(
						&rx, &point, &event);
				} else {
					result = v90_phase4_cp_rx_receive_decision(
						&rx, quarter, rotation, &event);
				}
				CHECK(result == V90_CP_OK,
				      "noise became API failure at symbol %zu, role %zu, points %u",
				      symbol, role, points);
				CHECK(event.type != V90_CP_RECEIVE_FRAME,
				      "corrupt record was accepted");
				rejected += event.type == V90_CP_RECEIVE_REJECTED;
			}
			CHECK(rejected != 0u, "corrupt record was not rejected");
			retry.acknowledges_peer = true;
			CHECK(v90_phase4_cp_tx_start_next_record(&tx, &retry) ==
				      V90_PHASE4_CP_TX_OK &&
			      transfer_record(
				      &tx, &rx, points != 0u, SIZE_MAX, 0u, &event) &&
			      event.type == V90_CP_RECEIVE_FRAME &&
			      event.frame_result == V90_CP_OK &&
			      parameters_are_equal(&event.parameters, &retry),
			      "noisy symbol prevented clean retry");
			CHECK(rx.descrambler.history == tx.scrambler.history,
			      "noisy symbol lost continuous history");
		}
	}
	return true;
}

static bool test_transactional_contracts(void)
{
	struct v90_phase4_cp_rx rx;
	struct v90_phase4_cp_rx saved;
	struct v90_phase4_cp_rx untouched;
	struct v90_phase4_cp_rx untouched_saved;
	struct v90_cp_receive_event event;
	struct v90_cp_receive_event saved_event;
	struct qam_point invalid_point = { NAN, 0.0 };

	memset(&untouched, 0xa5, sizeof(untouched));
	untouched_saved = untouched;
	CHECK(v90_phase4_cp_rx_init(
		      NULL, V34_CALLING_STATION,
		      V34_CONSTELLATION_4_POINT) == V90_CP_INVALID_ARGUMENT &&
	      v90_phase4_cp_rx_init(
		      &untouched, (enum v34_station_role)99,
		      V34_CONSTELLATION_4_POINT) == V90_CP_INVALID_ARGUMENT &&
	      v90_phase4_cp_rx_init(
		      &untouched, V34_CALLING_STATION,
		      (enum v34_constellation_size)8) == V90_CP_INVALID_ARGUMENT &&
	      memcmp(&untouched, &untouched_saved, sizeof(untouched)) == 0,
	      "rejected CP receiver initialization mutated state");
	CHECK(v90_phase4_cp_rx_init(
		      &rx, V34_ANSWERING_STATION,
		      V34_CONSTELLATION_4_POINT) == V90_CP_OK &&
	      !v90_phase4_cp_rx_is_valid(NULL),
	      "CP receiver contract fixture did not initialize");
	memset(&event, 0xa5, sizeof(event));
	saved = rx;
	saved_event = event;
	CHECK(v90_phase4_cp_rx_receive_decision(
		      &rx, 1, 0, &event) == V90_CP_INVALID_ARGUMENT &&
	      v90_phase4_cp_rx_receive_decision(
		      &rx, 0, 4, &event) == V90_CP_INVALID_ARGUMENT &&
	      v90_phase4_cp_rx_receive_decision(
		      &rx, 0, 0, NULL) == V90_CP_INVALID_ARGUMENT &&
	      v90_phase4_cp_rx_receive_point(
		      &rx, NULL, &event) == V90_CP_INVALID_ARGUMENT &&
	      v90_phase4_cp_rx_receive_point(
		      &rx, &invalid_point, &event) == V90_CP_INVALID_ARGUMENT &&
	      v90_phase4_cp_rx_receive_point(
		      &rx, &invalid_point, NULL) == V90_CP_INVALID_ARGUMENT &&
	      memcmp(&rx, &saved, sizeof(rx)) == 0 &&
	      memcmp(&event, &saved_event, sizeof(event)) == 0,
	      "rejected CP receive call was not transactional");
	rx.previous_clockwise_quarter_turns = 4;
	CHECK(!v90_phase4_cp_rx_is_valid(&rx) &&
	      v90_phase4_cp_rx_receive_decision(
		      &rx, 0, 0, &event) == V90_CP_INVALID_ARGUMENT,
	      "CP receiver accepted corrupt differential phase");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "first symbol and continuous records",
		  test_first_symbol_and_continuous_records },
		{ "corruption and reacquisition",
		  test_corruption_and_reacquisition },
		{ "fixed phase ambiguity", test_fixed_phase_ambiguity },
		{ "two rejections in one symbol", test_two_rejections_in_one_symbol },
		{ "transactional contracts", test_transactional_contracts },
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
