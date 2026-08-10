// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v34_phase4_e_sequence.h>

#include "test.h"

#include <math.h>
#include <string.h>

enum {
	TRN_PREFIX_SYMBOLS = SIPPYMODEM_V34_PHASE4_TRN_MIN_SYMBOLS + 3,
	V34_HISTORY_MASK = (1u << SIPPYMODEM_V34_SCRAMBLER_BITS) - 1u,
	V34_COMMON_TAP_DELAY = 23,
};

static const double TEST_TOLERANCE = 1e-14;

struct reference_mapper {
	uint32_t history;
	unsigned int primary_tap_delay;
	uint8_t previous_clockwise_quarter_turns;
};

static struct sippymodem_v34_mp_parameters parameters(bool acknowledged)
{
	return (struct sippymodem_v34_mp_parameters){
		.maximum_call_to_answer_rate_2400_bps_units = 10,
		.maximum_answer_to_call_rate_2400_bps_units = 9,
		.remote_transmitter_trellis =
			SIPPYMODEM_V34_TRELLIS_32_STATE,
		.acknowledges_peer_mp = acknowledged,
		.data_rate_capability_mask = 0x1555,
		.asymmetric_rates_enabled = true,
	};
}

static bool sequences_are_equal(
	const struct sippymodem_v34_phase4_e_sequence *left,
	const struct sippymodem_v34_phase4_e_sequence *right)
{
	return left->scrambler.history == right->scrambler.history &&
	       left->scrambler.polynomial == right->scrambler.polynomial &&
	       left->role == right->role &&
	       left->constellation == right->constellation &&
	       left->previous_clockwise_quarter_turns ==
		       right->previous_clockwise_quarter_turns &&
	       left->emitted_bits == right->emitted_bits;
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

static uint8_t reference_mapped_ones(struct reference_mapper *mapper)
{
	uint8_t first = reference_scramble_bit(mapper, 1u);
	uint8_t second = reference_scramble_bit(mapper, 1u);

	return (uint8_t)((second << 1) | first);
}

static struct sippymodem_qam_point reference_point(
	enum sippymodem_v34_constellation_size constellation,
	uint8_t quarter_point, uint8_t clockwise_quarter_turns)
{
	double unit = constellation == SIPPYMODEM_V34_CONSTELLATION_4_POINT ?
			      sqrt(0.5) :
			      sqrt(0.1);
	double in_phase = (quarter_point & 1u) != 0u ? -3.0 : 1.0;
	double quadrature = (quarter_point & 2u) != 0u ? -3.0 : 1.0;

	for (uint8_t turn = 0; turn < clockwise_quarter_turns; ++turn) {
		double saved_in_phase = in_phase;

		in_phase = quadrature;
		quadrature = -saved_in_phase;
	}
	return (struct sippymodem_qam_point){
		.in_phase = in_phase * unit,
		.quadrature = quadrature * unit,
	};
}

static bool points_are_close(
	const struct sippymodem_qam_point *left,
	const struct sippymodem_qam_point *right)
{
	return fabs(left->in_phase - right->in_phase) < TEST_TOLERANCE &&
	       fabs(left->quadrature - right->quadrature) < TEST_TOLERANCE;
}

static bool generate_completed_mp(
	struct sippymodem_v34_phase4_mp *mp,
	enum sippymodem_v34_station_role role,
	enum sippymodem_v34_constellation_size constellation,
	bool acknowledged)
{
	struct sippymodem_v34_phase4_trn trn;
	struct sippymodem_v34_mp_parameters selected =
		parameters(acknowledged);

	CHECK(sippymodem_v34_phase4_trn_init(&trn, role, constellation) ==
		      SIPPYMODEM_V34_PHASE4_TRN_OK,
	      "E fixture TRN did not initialize");
	for (size_t symbol = 0; symbol < TRN_PREFIX_SYMBOLS; ++symbol) {
		struct sippymodem_qam_point point;
		uint8_t quarter;
		uint8_t rotation;

		CHECK(sippymodem_v34_phase4_trn_next(
			      &trn, &point, &quarter, &rotation) ==
			      SIPPYMODEM_V34_PHASE4_TRN_OK,
		      "E fixture TRN stopped at symbol %zu", symbol);
	}
	CHECK(sippymodem_v34_phase4_mp_init_from_trn(mp, &trn, &selected) ==
		      SIPPYMODEM_V34_PHASE4_MP_OK,
	      "E fixture MP did not initialize");
	while (!sippymodem_v34_phase4_mp_record_is_complete(mp)) {
		struct sippymodem_qam_point point;
		uint8_t quarter;
		uint8_t rotation;
		bool complete;

		CHECK(sippymodem_v34_phase4_mp_next(
			      mp, &point, &quarter, &rotation, &complete) ==
			      SIPPYMODEM_V34_PHASE4_MP_OK,
		      "E fixture MP stopped at bit %zu", mp->bit_index);
	}
	return true;
}

static void reference_mapper_init(
	struct reference_mapper *mapper,
	const struct sippymodem_v34_phase4_mp *mp)
{
	*mapper = (struct reference_mapper){
		.history = mp->scrambler.history,
		.primary_tap_delay =
			mp->role == SIPPYMODEM_V34_CALLING_STATION ? 18u : 5u,
		.previous_clockwise_quarter_turns =
			mp->previous_clockwise_quarter_turns,
	};
}

static bool verify_sequence(
	struct sippymodem_v34_phase4_e_sequence *sequence,
	struct reference_mapper *reference)
{
	uint8_t symbol_bits =
		sequence->constellation ==
				SIPPYMODEM_V34_CONSTELLATION_16_POINT ?
			4u :
			2u;
	size_t symbol_count = SIPPYMODEM_V34_PHASE4_E_BITS / symbol_bits;

	for (size_t symbol = 0; symbol < symbol_count; ++symbol) {
		uint8_t increment = reference_mapped_ones(reference);
		uint8_t rotation = (uint8_t)(
			(reference->previous_clockwise_quarter_turns + increment) &
			3u);
		uint8_t quarter = 0;
		struct sippymodem_qam_point expected;
		struct sippymodem_qam_point point = { 7.0, -9.0 };
		uint8_t emitted_quarter = UINT8_MAX;
		uint8_t emitted_rotation = UINT8_MAX;

		if (symbol_bits == 4u)
			quarter = reference_mapped_ones(reference);
		reference->previous_clockwise_quarter_turns = rotation;
		expected = reference_point(
			sequence->constellation, quarter, rotation);
		CHECK(sippymodem_v34_phase4_e_sequence_next(
			      sequence, &point, &emitted_quarter,
			      &emitted_rotation) ==
			      SIPPYMODEM_V34_PHASE4_E_SEQUENCE_OK,
		      "E sequence stopped at symbol %zu/%zu", symbol,
		      symbol_count);
		CHECK(emitted_quarter == quarter &&
		      emitted_rotation == rotation &&
		      points_are_close(&point, &expected) &&
		      sequence->emitted_bits == (symbol + 1u) * symbol_bits &&
		      sequence->scrambler.history == reference->history &&
		      sequence->previous_clockwise_quarter_turns ==
			      reference->previous_clockwise_quarter_turns &&
		      sippymodem_v34_phase4_e_sequence_is_valid(sequence) &&
		      sippymodem_v34_phase4_e_sequence_is_complete(sequence) ==
			      (symbol + 1u == symbol_count),
		      "E symbol %zu/%zu changed", symbol, symbol_count);
	}
	{
		struct sippymodem_v34_phase4_e_sequence saved = *sequence;
		struct sippymodem_qam_point point = { 7.0, -9.0 };
		uint8_t quarter = UINT8_MAX;
		uint8_t rotation = UINT8_MAX;

		CHECK(sippymodem_v34_phase4_e_sequence_next(
			      sequence, &point, &quarter, &rotation) ==
			      SIPPYMODEM_V34_PHASE4_E_SEQUENCE_COMPLETE &&
		      sequences_are_equal(sequence, &saved) &&
		      point.in_phase == 7.0 && point.quadrature == -9.0 &&
		      quarter == UINT8_MAX && rotation == UINT8_MAX,
		      "completed E sequence emitted an extra symbol");
	}
	return true;
}

static bool test_roles_constellations_and_records(void)
{
	static const enum sippymodem_v34_station_role roles[] = {
		SIPPYMODEM_V34_CALLING_STATION,
		SIPPYMODEM_V34_ANSWERING_STATION,
	};
	static const enum sippymodem_v34_constellation_size constellations[] = {
		SIPPYMODEM_V34_CONSTELLATION_4_POINT,
		SIPPYMODEM_V34_CONSTELLATION_16_POINT,
	};

	CHECK(SIPPYMODEM_V34_PHASE4_E_BITS == 20 &&
	      SIPPYMODEM_V34_PHASE4_E_4_POINT_SYMBOLS == 10 &&
	      SIPPYMODEM_V34_PHASE4_E_16_POINT_SYMBOLS == 5,
	      "E sequence dimensions changed");
	for (size_t role_index = 0; role_index < ARRAY_LENGTH(roles);
	     ++role_index) {
		for (size_t constellation_index = 0;
		     constellation_index < ARRAY_LENGTH(constellations);
		     ++constellation_index) {
			for (unsigned int acknowledged = 0; acknowledged < 2;
			     ++acknowledged) {
				struct sippymodem_v34_phase4_mp mp;
				struct sippymodem_v34_phase4_e_sequence sequence;
				struct reference_mapper reference;

				CHECK(generate_completed_mp(
					      &mp, roles[role_index],
					      constellations[constellation_index],
					      acknowledged != 0),
				      "E MP fixture %zu/%zu/%u failed", role_index,
				      constellation_index, acknowledged);
				reference_mapper_init(&reference, &mp);
				CHECK(sippymodem_v34_phase4_e_sequence_init_from_mp(
					      &sequence, &mp) ==
					      SIPPYMODEM_V34_PHASE4_E_SEQUENCE_OK &&
				      sequence.scrambler.history == mp.scrambler.history &&
				      sequence.scrambler.polynomial ==
					      mp.scrambler.polynomial &&
				      sequence.previous_clockwise_quarter_turns ==
					      mp.previous_clockwise_quarter_turns &&
				      sequence.emitted_bits == 0 &&
				      !sippymodem_v34_phase4_e_sequence_is_complete(
					      &sequence),
				      "E transition %zu/%zu/%u did not inherit MP state",
				      role_index, constellation_index, acknowledged);
				CHECK(verify_sequence(&sequence, &reference),
				      "E sequence %zu/%zu/%u failed", role_index,
				      constellation_index, acknowledged);
			}
		}
	}
	return true;
}

static bool test_copy_behavior(void)
{
	struct sippymodem_v34_phase4_mp mp;
	struct sippymodem_v34_phase4_e_sequence original;
	struct sippymodem_v34_phase4_e_sequence copy;

	CHECK(generate_completed_mp(
		      &mp, SIPPYMODEM_V34_CALLING_STATION,
		      SIPPYMODEM_V34_CONSTELLATION_16_POINT, true) &&
	      sippymodem_v34_phase4_e_sequence_init_from_mp(&original, &mp) ==
		      SIPPYMODEM_V34_PHASE4_E_SEQUENCE_OK,
	      "E copy fixture did not initialize");
	{
		struct sippymodem_qam_point point;
		uint8_t quarter;
		uint8_t rotation;

		CHECK(sippymodem_v34_phase4_e_sequence_next(
			      &original, &point, &quarter, &rotation) ==
			      SIPPYMODEM_V34_PHASE4_E_SEQUENCE_OK,
		      "E copy fixture did not advance");
	}
	copy = original;
	while (!sippymodem_v34_phase4_e_sequence_is_complete(&original)) {
		struct sippymodem_qam_point original_point;
		struct sippymodem_qam_point copied_point;
		uint8_t original_quarter;
		uint8_t copied_quarter;
		uint8_t original_rotation;
		uint8_t copied_rotation;

		CHECK(sippymodem_v34_phase4_e_sequence_next(
			      &original, &original_point, &original_quarter,
			      &original_rotation) ==
			      SIPPYMODEM_V34_PHASE4_E_SEQUENCE_OK &&
		      sippymodem_v34_phase4_e_sequence_next(
			      &copy, &copied_point, &copied_quarter,
			      &copied_rotation) ==
			      SIPPYMODEM_V34_PHASE4_E_SEQUENCE_OK &&
		      sequences_are_equal(&original, &copy) &&
		      original_quarter == copied_quarter &&
		      original_rotation == copied_rotation &&
		      points_are_close(&original_point, &copied_point),
		      "copied E sequence diverged at bit %u",
		      original.emitted_bits);
	}
	return true;
}

static bool test_validation(void)
{
	struct sippymodem_v34_phase4_mp mp;
	struct sippymodem_v34_phase4_e_sequence sequence = {
		.scrambler = {
			.history = 7,
			.polynomial = SIPPYMODEM_V34_POLYNOMIAL_GPC,
		},
		.role = SIPPYMODEM_V34_CALLING_STATION,
		.constellation = SIPPYMODEM_V34_CONSTELLATION_4_POINT,
		.previous_clockwise_quarter_turns = 2,
		.emitted_bits = 4,
	};
	struct sippymodem_v34_phase4_e_sequence saved = sequence;
	struct sippymodem_qam_point point = { 7.0, -9.0 };
	uint8_t quarter = UINT8_MAX;
	uint8_t rotation = UINT8_MAX;

	CHECK(generate_completed_mp(
		      &mp, SIPPYMODEM_V34_ANSWERING_STATION,
		      SIPPYMODEM_V34_CONSTELLATION_4_POINT, true),
	      "E validation MP fixture failed");
	mp.bit_index = 0;
	CHECK(sippymodem_v34_phase4_e_sequence_init_from_mp(
		      NULL, &mp) ==
		      SIPPYMODEM_V34_PHASE4_E_SEQUENCE_INVALID_ARGUMENT &&
	      sippymodem_v34_phase4_e_sequence_init_from_mp(
		      &sequence, NULL) ==
		      SIPPYMODEM_V34_PHASE4_E_SEQUENCE_INVALID_ARGUMENT &&
	      sippymodem_v34_phase4_e_sequence_init_from_mp(
		      &sequence, &mp) ==
		      SIPPYMODEM_V34_PHASE4_E_SEQUENCE_INVALID_ARGUMENT &&
	      sequences_are_equal(&sequence, &saved),
	      "E initialization accepted an incomplete MP record");
	mp.bit_index = SIPPYMODEM_V34_MP_TYPE0_FRAME_BITS;
	CHECK(sippymodem_v34_phase4_e_sequence_init_from_mp(&sequence, &mp) ==
		      SIPPYMODEM_V34_PHASE4_E_SEQUENCE_OK,
	      "E validation fixture did not initialize");
	saved = sequence;
	CHECK(sippymodem_v34_phase4_e_sequence_next(
		      NULL, &point, &quarter, &rotation) ==
		      SIPPYMODEM_V34_PHASE4_E_SEQUENCE_INVALID_ARGUMENT &&
	      sippymodem_v34_phase4_e_sequence_next(
		      &sequence, NULL, &quarter, &rotation) ==
		      SIPPYMODEM_V34_PHASE4_E_SEQUENCE_INVALID_ARGUMENT &&
	      sippymodem_v34_phase4_e_sequence_next(
		      &sequence, &point, NULL, &rotation) ==
		      SIPPYMODEM_V34_PHASE4_E_SEQUENCE_INVALID_ARGUMENT &&
	      sippymodem_v34_phase4_e_sequence_next(
		      &sequence, &point, &quarter, NULL) ==
		      SIPPYMODEM_V34_PHASE4_E_SEQUENCE_INVALID_ARGUMENT &&
	      sequences_are_equal(&sequence, &saved) &&
	      point.in_phase == 7.0 && point.quadrature == -9.0 &&
	      quarter == UINT8_MAX && rotation == UINT8_MAX,
	      "invalid E emission mutated state or outputs");

	sequence = saved;
	sequence.emitted_bits = 1;
	CHECK(!sippymodem_v34_phase4_e_sequence_is_valid(&sequence),
	      "E accepted a partial-symbol cursor");
	sequence = saved;
	sequence.emitted_bits = SIPPYMODEM_V34_PHASE4_E_BITS + 1;
	CHECK(!sippymodem_v34_phase4_e_sequence_is_valid(&sequence),
	      "E accepted a cursor beyond its fixed length");
	sequence = saved;
	sequence.previous_clockwise_quarter_turns = 4;
	CHECK(!sippymodem_v34_phase4_e_sequence_is_valid(&sequence),
	      "E accepted an invalid differential phase");
	sequence = saved;
	sequence.role = SIPPYMODEM_V34_CALLING_STATION;
	CHECK(!sippymodem_v34_phase4_e_sequence_is_valid(&sequence),
	      "E accepted a role/polynomial mismatch");
	sequence = saved;
	sequence.constellation =
		(enum sippymodem_v34_constellation_size)8;
	CHECK(!sippymodem_v34_phase4_e_sequence_is_valid(&sequence),
	      "E accepted an invalid constellation");
	sequence = saved;
	sequence.scrambler.history = UINT32_C(1) <<
		SIPPYMODEM_V34_SCRAMBLER_BITS;
	CHECK(!sippymodem_v34_phase4_e_sequence_is_valid(&sequence) &&
	      !sippymodem_v34_phase4_e_sequence_is_valid(NULL) &&
	      !sippymodem_v34_phase4_e_sequence_is_complete(NULL),
	      "E accepted corrupt state");

	CHECK(strcmp(sippymodem_v34_phase4_e_sequence_result_name(
		     SIPPYMODEM_V34_PHASE4_E_SEQUENCE_COMPLETE),
		     "complete") == 0 &&
	      strcmp(sippymodem_v34_phase4_e_sequence_result_name(
		     (enum sippymodem_v34_phase4_e_sequence_result)3),
		     "unknown") == 0,
	      "E result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "roles, constellations, and records",
		  test_roles_constellations_and_records },
		{ "copy behavior", test_copy_behavior },
		{ "validation", test_validation },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (!tests[index].run()) {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		} else {
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		}
	}
	return failures == 0 ? 0 : 1;
}
