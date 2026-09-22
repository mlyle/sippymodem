// SPDX-License-Identifier: BSD-3-Clause

#include <v90_phase4_cp_tx.h>

#include "test.h"

#include <math.h>
#include <string.h>

enum {
	V34_HISTORY_MASK = (1u << V34_SCRAMBLER_BITS) - 1u,
	V34_COMMON_TAP_DELAY = 23,
};

static const double TEST_TOLERANCE = 1e-14;

struct reference_mapper {
	uint32_t history;
	unsigned int primary_tap_delay;
	uint8_t previous_clockwise_quarter_turns;
};

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

static uint8_t packed_bit(
	const uint8_t *packed, size_t bit_index)
{
	return (uint8_t)((packed[bit_index / 8u] >>
			  (7u - bit_index % 8u)) &
			 1u);
}

static uint8_t packed_dibit(
	const uint8_t *packed, size_t first_bit)
{
	return (uint8_t)((packed_bit(packed, first_bit) << 1) |
			 packed_bit(packed, first_bit + 1u));
}

static uint8_t reference_scramble_bit(
	struct reference_mapper *mapper, uint8_t input)
{
	uint8_t output = input ^
		(uint8_t)((mapper->history >>
			   (mapper->primary_tap_delay - 1u)) &
			  1u) ^
		(uint8_t)((mapper->history >>
			   (V34_COMMON_TAP_DELAY - 1u)) &
			  1u);

	mapper->history =
		((mapper->history << 1) | output) & V34_HISTORY_MASK;
	return output;
}

static uint8_t reference_mapped_dibit(
	struct reference_mapper *mapper, uint8_t input_dibit)
{
	uint8_t first = reference_scramble_bit(
		mapper, (uint8_t)((input_dibit >> 1) & 1u));
	uint8_t second = reference_scramble_bit(
		mapper, (uint8_t)(input_dibit & 1u));

	return (uint8_t)((second << 1) | first);
}

static struct qam_point reference_point(
	enum v34_constellation_size constellation,
	uint8_t quarter_point, uint8_t clockwise_quarter_turns)
{
	double unit = constellation == V34_CONSTELLATION_4_POINT ?
			      sqrt(0.5) :
			      sqrt(0.1);
	double in_phase = (quarter_point & 1u) != 0u ? -3.0 : 1.0;
	double quadrature = (quarter_point & 2u) != 0u ? -3.0 : 1.0;

	for (uint8_t turn = 0; turn < clockwise_quarter_turns; ++turn) {
		double saved_in_phase = in_phase;

		in_phase = quadrature;
		quadrature = -saved_in_phase;
	}
	return (struct qam_point){
		.in_phase = in_phase * unit,
		.quadrature = quadrature * unit,
	};
}

static bool points_are_close(
	const struct qam_point *left,
	const struct qam_point *right)
{
	return fabs(left->in_phase - right->in_phase) < TEST_TOLERANCE &&
	       fabs(left->quadrature - right->quadrature) < TEST_TOLERANCE;
}

static bool verify_record(
	struct v90_phase4_cp_tx *tx,
	struct reference_mapper *reference,
	const struct v90_cp_parameters *selected)
{
	uint8_t frame[V90_CP_MAX_FRAME_BYTES];
	size_t frame_bits = 0;
	size_t symbol_bits =
		tx->constellation == V34_CONSTELLATION_16_POINT ? 4u : 2u;

	CHECK(v90_cp_encode(
		      selected, frame, sizeof(frame), &frame_bits) ==
		      V90_CP_OK &&
	      frame_bits == tx->frame_bit_count &&
	      tx->bit_index == 0,
	      "CP symbol fixture did not encode");
	for (size_t first_bit = 0; first_bit < frame_bits;
	     first_bit += symbol_bits) {
		uint8_t increment = reference_mapped_dibit(
			reference, packed_dibit(frame, first_bit));
		uint8_t rotation = (uint8_t)(
			(reference->previous_clockwise_quarter_turns +
			 increment) &
			3u);
		uint8_t quarter = 0;
		struct qam_point expected;
		struct qam_point emitted = { 7.0, -9.0 };
		uint8_t emitted_quarter = UINT8_MAX;
		uint8_t emitted_rotation = UINT8_MAX;
		bool complete = false;

		if (symbol_bits == 4u)
			quarter = reference_mapped_dibit(
				reference, packed_dibit(frame, first_bit + 2u));
		reference->previous_clockwise_quarter_turns = rotation;
		expected = reference_point(
			tx->constellation, quarter, rotation);
		CHECK(v90_phase4_cp_tx_next(
			      tx, &emitted, &emitted_quarter,
			      &emitted_rotation, &complete) ==
			      V90_PHASE4_CP_TX_OK,
		      "CP transmitter stopped at bit %zu/%zu",
		      first_bit, frame_bits);
		CHECK(emitted_quarter == quarter &&
		      emitted_rotation == rotation &&
		      points_are_close(&emitted, &expected) &&
		      tx->scrambler.history == reference->history &&
		      tx->previous_clockwise_quarter_turns == rotation &&
		      tx->bit_index == first_bit + symbol_bits &&
		      complete == (first_bit + symbol_bits == frame_bits) &&
		      v90_phase4_cp_tx_is_valid(tx),
		      "CP symbol changed at bit %zu/%zu", first_bit,
		      frame_bits);
	}
	CHECK(v90_phase4_cp_tx_record_is_complete(tx),
	      "CP record did not reach its boundary");
	{
		struct v90_phase4_cp_tx saved = *tx;
		struct qam_point point = { 7.0, -9.0 };
		uint8_t quarter = UINT8_MAX;
		uint8_t rotation = UINT8_MAX;
		bool complete = false;

		CHECK(v90_phase4_cp_tx_next(
			      tx, &point, &quarter, &rotation, &complete) ==
			      V90_PHASE4_CP_TX_RECORD_COMPLETE &&
		      memcmp(tx, &saved, sizeof(saved)) == 0 &&
		      point.in_phase == 7.0 && point.quadrature == -9.0 &&
		      quarter == UINT8_MAX && rotation == UINT8_MAX &&
		      !complete,
		      "completed CP record emitted an extra symbol");
	}
	return true;
}

static bool verify_scr(
	struct v90_phase4_cp_tx *tx,
	struct reference_mapper *reference, size_t symbol_count)
{
	size_t saved_bit_index = tx->bit_index;

	CHECK(v90_phase4_cp_tx_record_is_complete(tx),
	      "SCR fixture did not start after CPt");
	for (size_t index = 0; index < symbol_count; ++index) {
		uint8_t increment = reference_mapped_dibit(reference, 3u);
		uint8_t rotation = (uint8_t)(
			(reference->previous_clockwise_quarter_turns + increment) &
			3u);
		uint8_t quarter = 0;
		struct qam_point expected;
		struct qam_point emitted = { 7.0, -9.0 };
		uint8_t emitted_quarter = UINT8_MAX;
		uint8_t emitted_rotation = UINT8_MAX;

		if (tx->constellation == V34_CONSTELLATION_16_POINT)
			quarter = reference_mapped_dibit(reference, 3u);
		reference->previous_clockwise_quarter_turns = rotation;
		expected = reference_point(tx->constellation, quarter, rotation);
		CHECK(v90_phase4_cp_tx_scr_next(
			      tx, &emitted, &emitted_quarter,
			      &emitted_rotation) == V90_PHASE4_CP_TX_OK &&
		      emitted_quarter == quarter &&
		      emitted_rotation == rotation &&
		      points_are_close(&emitted, &expected) &&
		      tx->scrambler.history == reference->history &&
		      tx->previous_clockwise_quarter_turns == rotation &&
		      tx->bit_index == saved_bit_index &&
		      v90_phase4_cp_tx_record_is_complete(tx),
		      "SCR symbol changed at index %zu", index);
	}
	return true;
}

static bool test_roles_constellations_and_continuity(void)
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
	struct v90_cp_parameters repeated = parameters(
		V90_CP_TRAINING, 3, true, 1);
	struct v90_cp_parameters data = parameters(
		V90_CP_DATA, V90_CP_MAX_CONSTELLATIONS, true, 2);

	CHECK(V90_PHASE4_CP_MIN_4_POINT_SYMBOLS == 146 &&
	      V90_PHASE4_CP_MAX_4_POINT_SYMBOLS == 894 &&
	      V90_PHASE4_CP_MIN_16_POINT_SYMBOLS == 73 &&
	      V90_PHASE4_CP_MAX_16_POINT_SYMBOLS == 447,
	      "CP symbol dimensions changed");
	for (size_t role = 0; role < ARRAY_LENGTH(roles); ++role) {
		for (size_t constellation = 0;
		     constellation < ARRAY_LENGTH(constellations);
		     ++constellation) {
			struct v90_phase4_cp_tx tx;
			struct reference_mapper reference = {
				.primary_tap_delay =
					roles[role] == V34_CALLING_STATION ? 18u : 5u,
			};
			uint32_t history;
			uint8_t rotation;

			CHECK(v90_phase4_cp_tx_init(
				      &tx, roles[role], constellations[constellation],
				      &training) == V90_PHASE4_CP_TX_OK &&
			      tx.scrambler.history == 0 &&
			      tx.previous_clockwise_quarter_turns == 0 &&
			      verify_record(&tx, &reference, &training) &&
			      verify_scr(&tx, &reference, 11u),
			      "first CPt failed for role/constellation %zu/%zu",
			      role, constellation);
			history = tx.scrambler.history;
			rotation = tx.previous_clockwise_quarter_turns;
			CHECK(v90_phase4_cp_tx_start_next_record(
				      &tx, &repeated) == V90_PHASE4_CP_TX_OK &&
			      tx.scrambler.history == history &&
			      tx.previous_clockwise_quarter_turns == rotation &&
			      verify_record(&tx, &reference, &repeated),
			      "repeated CPt failed for role/constellation %zu/%zu",
			      role, constellation);
			history = tx.scrambler.history;
			rotation = tx.previous_clockwise_quarter_turns;
			CHECK(v90_phase4_cp_tx_start_next_record(
				      &tx, &data) == V90_PHASE4_CP_TX_OK &&
			      tx.scrambler.history == history &&
			      tx.previous_clockwise_quarter_turns == rotation &&
			      verify_record(&tx, &reference, &data),
			      "CP data failed for role/constellation %zu/%zu",
			      role, constellation);
		}
	}
	return true;
}

static bool test_transactional_contracts(void)
{
	struct v90_cp_parameters training = parameters(
		V90_CP_TRAINING, 1, false, 0);
	struct v90_cp_parameters data = parameters(
		V90_CP_DATA, 1, false, 1);
	struct v90_cp_parameters invalid = training;
	struct v90_phase4_cp_tx tx;
	struct v90_phase4_cp_tx saved;
	struct v90_phase4_cp_tx untouched;
	struct v90_phase4_cp_tx untouched_saved;
	struct qam_point point = { 7.0, -9.0 };
	struct qam_point saved_point;
	uint8_t quarter = UINT8_MAX;
	uint8_t rotation = UINT8_MAX;
	uint8_t saved_quarter;
	uint8_t saved_rotation;
	bool complete = true;
	bool saved_complete;

	memset(&untouched, 0xa5, sizeof(untouched));
	untouched_saved = untouched;
	invalid.selected_rate_index = V90_CP_RATE_INDEX_MAX + 1u;
	CHECK(v90_phase4_cp_tx_init(
		      NULL, V34_CALLING_STATION,
		      V34_CONSTELLATION_4_POINT, &training) ==
		      V90_PHASE4_CP_TX_INVALID_ARGUMENT &&
	      v90_phase4_cp_tx_init(
		      &untouched, (enum v34_station_role)99,
		      V34_CONSTELLATION_4_POINT, &training) ==
		      V90_PHASE4_CP_TX_INVALID_ARGUMENT &&
	      v90_phase4_cp_tx_init(
		      &untouched, V34_CALLING_STATION,
		      (enum v34_constellation_size)8, &training) ==
		      V90_PHASE4_CP_TX_INVALID_ARGUMENT &&
	      v90_phase4_cp_tx_init(
		      &untouched, V34_CALLING_STATION,
		      V34_CONSTELLATION_4_POINT, NULL) ==
		      V90_PHASE4_CP_TX_INVALID_ARGUMENT &&
	      v90_phase4_cp_tx_init(
		      &untouched, V34_CALLING_STATION,
		      V34_CONSTELLATION_4_POINT, &data) ==
		      V90_PHASE4_CP_TX_INVALID_PARAMETERS &&
	      v90_phase4_cp_tx_init(
		      &untouched, V34_CALLING_STATION,
		      V34_CONSTELLATION_4_POINT, &invalid) ==
		      V90_PHASE4_CP_TX_INVALID_PARAMETERS &&
	      memcmp(&untouched, &untouched_saved, sizeof(untouched)) == 0,
	      "rejected CP transmitter initialization mutated state");

	CHECK(v90_phase4_cp_tx_init(
		      &tx, V34_CALLING_STATION,
		      V34_CONSTELLATION_16_POINT, &training) ==
		      V90_PHASE4_CP_TX_OK &&
	      !v90_phase4_cp_tx_is_valid(NULL) &&
	      !v90_phase4_cp_tx_record_is_complete(NULL),
	      "CP transmitter contract fixture did not initialize");
	saved = tx;
	saved_point = point;
	saved_quarter = quarter;
	saved_rotation = rotation;
	CHECK(v90_phase4_cp_tx_scr_next(
		      &tx, &point, &quarter, &rotation) ==
		      V90_PHASE4_CP_TX_RECORD_IN_PROGRESS &&
	      memcmp(&tx, &saved, sizeof(tx)) == 0 &&
	      point.in_phase == saved_point.in_phase &&
	      point.quadrature == saved_point.quadrature &&
	      quarter == saved_quarter && rotation == saved_rotation,
	      "SCR entered before the CPt record boundary");
	CHECK(v90_phase4_cp_tx_start_next_record(NULL, &data) ==
		      V90_PHASE4_CP_TX_INVALID_ARGUMENT &&
	      v90_phase4_cp_tx_start_next_record(&tx, NULL) ==
		      V90_PHASE4_CP_TX_INVALID_ARGUMENT &&
	      v90_phase4_cp_tx_start_next_record(&tx, &data) ==
		      V90_PHASE4_CP_TX_RECORD_IN_PROGRESS &&
	      memcmp(&tx, &saved, sizeof(tx)) == 0,
	      "CP record boundary rejection mutated state");
	CHECK(v90_phase4_cp_tx_next(
		      &tx, &point, &quarter, &rotation, &complete) ==
		      V90_PHASE4_CP_TX_OK,
	      "CP contract fixture did not emit one symbol");
	saved = tx;
	saved_point = point;
	saved_quarter = quarter;
	saved_rotation = rotation;
	saved_complete = complete;
	CHECK(v90_phase4_cp_tx_next(
		      &tx, NULL, &quarter, &rotation, &complete) ==
		      V90_PHASE4_CP_TX_INVALID_ARGUMENT &&
	      v90_phase4_cp_tx_next(
		      &tx, &point, NULL, &rotation, &complete) ==
		      V90_PHASE4_CP_TX_INVALID_ARGUMENT &&
	      v90_phase4_cp_tx_next(
		      &tx, &point, &quarter, NULL, &complete) ==
		      V90_PHASE4_CP_TX_INVALID_ARGUMENT &&
		      v90_phase4_cp_tx_next(
		      &tx, &point, &quarter, &rotation, NULL) ==
		      V90_PHASE4_CP_TX_INVALID_ARGUMENT &&
	      memcmp(&tx, &saved, sizeof(tx)) == 0 &&
	      point.in_phase == saved_point.in_phase &&
	      point.quadrature == saved_point.quadrature &&
	      quarter == saved_quarter && rotation == saved_rotation &&
	      complete == saved_complete,
	      "missing CP symbol output mutated state");
	for (unsigned int position = 0; position < 3u; ++position) {
		struct v90_phase4_cp_tx corrupt = saved;
		struct v90_phase4_cp_tx before;

		corrupt.bit_index = position == 2u ? corrupt.frame_bit_count :
			position * v34_constellation_bits_per_symbol(corrupt.constellation);
		corrupt.frame[4] ^= 1u;
		before = corrupt;
		CHECK(v90_phase4_cp_tx_next(
			      &corrupt, &point, &quarter, &rotation, &complete) ==
			      V90_PHASE4_CP_TX_INVALID_ARGUMENT &&
		      memcmp(&corrupt, &before, sizeof(corrupt)) == 0 &&
		      point.in_phase == saved_point.in_phase &&
		      point.quadrature == saved_point.quadrature &&
		      quarter == saved_quarter && rotation == saved_rotation &&
		      complete == saved_complete,
		      "corrupt CP record mutated state or outputs at position %u",
		      position);
	}
	tx.previous_clockwise_quarter_turns = 4;
	CHECK(!v90_phase4_cp_tx_is_valid(&tx) &&
	      v90_phase4_cp_tx_next(
		      &tx, &point, &quarter, &rotation, &complete) ==
		      V90_PHASE4_CP_TX_INVALID_ARGUMENT,
	      "CP transmitter accepted corrupt differential phase");
	tx = saved;
	tx.bit_index = tx.frame_bit_count;
	tx.frame[0] ^= 1u;
	saved = tx;
	CHECK(!v90_phase4_cp_tx_record_is_complete(&tx) &&
	      v90_phase4_cp_tx_scr_next(&tx, &point, &quarter, &rotation) ==
		      V90_PHASE4_CP_TX_INVALID_ARGUMENT &&
	      memcmp(&tx, &saved, sizeof(tx)) == 0 &&
	      point.in_phase == saved_point.in_phase && point.quadrature == saved_point.quadrature &&
	      quarter == saved_quarter && rotation == saved_rotation,
	      "SCR accepted a corrupt record at the complete cursor");
	CHECK(strcmp(v90_phase4_cp_tx_result_name(V90_PHASE4_CP_TX_OK),
		     "ok") == 0 &&
	      strcmp(v90_phase4_cp_tx_result_name(
			     V90_PHASE4_CP_TX_RECORD_COMPLETE),
		     "record complete") == 0 &&
	      strcmp(v90_phase4_cp_tx_result_name(
			     V90_PHASE4_CP_TX_RECORD_IN_PROGRESS),
		     "record in progress") == 0 &&
	      strcmp(v90_phase4_cp_tx_result_name(
			     V90_PHASE4_CP_TX_INVALID_ARGUMENT),
		     "invalid argument") == 0 &&
	      strcmp(v90_phase4_cp_tx_result_name(
			     V90_PHASE4_CP_TX_INVALID_PARAMETERS),
		     "invalid parameters") == 0 &&
	      strcmp(v90_phase4_cp_tx_result_name(
			     (enum v90_phase4_cp_tx_result)99),
		     "unknown") == 0,
	      "CP transmitter result names changed");
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "roles, constellations, and continuity",
		  test_roles_constellations_and_continuity },
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
