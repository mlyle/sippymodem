// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_v34_phase4_mp.h>

#include "test.h"

#include <math.h>
#include <string.h>

enum {
	TRN_PREFIX_SYMBOLS = SIPPYMODEM_V34_PHASE4_TRN_MIN_SYMBOLS + 5,
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
		.auxiliary_channel_enabled = true,
		.remote_transmitter_trellis =
			SIPPYMODEM_V34_MP_TRELLIS_32_STATE,
		.remote_transmitter_expanded_shaping = true,
		.acknowledges_peer_mp = acknowledged,
		.data_rate_capability_mask = 0x1a55,
		.asymmetric_rates_enabled = true,
	};
}

static bool transmitters_are_equal(
	const struct sippymodem_v34_phase4_mp *left,
	const struct sippymodem_v34_phase4_mp *right)
{
	return memcmp(left->frame, right->frame, sizeof(left->frame)) == 0 &&
	       left->scrambler.history == right->scrambler.history &&
	       left->scrambler.polynomial == right->scrambler.polynomial &&
	       left->role == right->role &&
	       left->constellation == right->constellation &&
	       left->bit_index == right->bit_index &&
	       left->previous_clockwise_quarter_turns ==
		       right->previous_clockwise_quarter_turns;
}

static uint8_t packed_bit(const uint8_t *packed, size_t bit_index)
{
	return (uint8_t)((packed[bit_index / 8u] >>
			  (7u - bit_index % 8u)) &
			 1u);
}

static uint8_t packed_dibit(const uint8_t *packed, size_t first_bit)
{
	return (uint8_t)((packed_bit(packed, first_bit) << 1) |
			 packed_bit(packed, first_bit + 1u));
}

static void reference_mapper_init(
	struct reference_mapper *mapper,
	const struct sippymodem_v34_phase4_trn *trn)
{
	*mapper = (struct reference_mapper){
		.history = trn->scrambler.history,
		.primary_tap_delay =
			trn->role == SIPPYMODEM_V34_CALLING_STATION ? 18u : 5u,
		.previous_clockwise_quarter_turns =
			trn->last_clockwise_quarter_turns,
	};
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

static bool generate_trn_prefix(
	struct sippymodem_v34_phase4_trn *trn,
	enum sippymodem_v34_station_role role,
	enum sippymodem_v34_constellation_size constellation)
{
	CHECK(sippymodem_v34_phase4_trn_init(trn, role, constellation) ==
		      SIPPYMODEM_V34_PHASE4_TRN_OK,
	      "TRN prefix did not initialize");
	for (size_t symbol = 0; symbol < TRN_PREFIX_SYMBOLS; ++symbol) {
		struct sippymodem_qam_point point;
		uint8_t quarter;
		uint8_t rotation;

		CHECK(sippymodem_v34_phase4_trn_next(
			      trn, &point, &quarter, &rotation) ==
			      SIPPYMODEM_V34_PHASE4_TRN_OK,
		      "TRN prefix stopped at symbol %zu", symbol);
	}
	return true;
}

static bool verify_record(
	struct sippymodem_v34_phase4_mp *transmitter,
	struct reference_mapper *reference,
	const struct sippymodem_v34_mp_parameters *selected)
{
	uint8_t expected_frame[SIPPYMODEM_V34_MP_TYPE0_FRAME_BYTES];
	size_t symbol_bits =
		transmitter->constellation ==
				SIPPYMODEM_V34_CONSTELLATION_16_POINT ?
			4u :
			2u;
	size_t symbol_count = SIPPYMODEM_V34_MP_TYPE0_FRAME_BITS /
		symbol_bits;

	CHECK(sippymodem_v34_mp_type0_encode(
		      selected, expected_frame, sizeof(expected_frame)) ==
		      SIPPYMODEM_V34_MP_OK &&
	      memcmp(transmitter->frame, expected_frame,
		     sizeof(expected_frame)) == 0,
	      "MP transmitter did not own the selected wire record");
	for (size_t symbol = 0; symbol < symbol_count; ++symbol) {
		size_t first_bit = symbol * symbol_bits;
		uint8_t increment = reference_mapped_dibit(
			reference, packed_dibit(expected_frame, first_bit));
		uint8_t rotation = (uint8_t)(
			(reference->previous_clockwise_quarter_turns + increment) &
			3u);
		uint8_t quarter = 0;
		struct sippymodem_qam_point expected;
		struct sippymodem_qam_point point = { 7.0, -9.0 };
		uint8_t emitted_quarter = UINT8_MAX;
		uint8_t emitted_rotation = UINT8_MAX;
		bool complete = true;

		if (symbol_bits == 4u)
			quarter = reference_mapped_dibit(
				reference,
				packed_dibit(expected_frame, first_bit + 2u));
		reference->previous_clockwise_quarter_turns = rotation;
		expected = reference_point(
			transmitter->constellation, quarter, rotation);
		CHECK(sippymodem_v34_phase4_mp_next(
			      transmitter, &point, &emitted_quarter,
			      &emitted_rotation, &complete) ==
			      SIPPYMODEM_V34_PHASE4_MP_OK,
		      "MP record stopped at symbol %zu/%zu", symbol,
		      symbol_count);
		CHECK(emitted_quarter == quarter &&
		      emitted_rotation == rotation &&
		      complete == (symbol + 1u == symbol_count) &&
		      points_are_close(&point, &expected) &&
		      transmitter->bit_index == first_bit + symbol_bits &&
		      transmitter->scrambler.history == reference->history &&
		      transmitter->previous_clockwise_quarter_turns ==
			      reference->previous_clockwise_quarter_turns &&
		      sippymodem_v34_phase4_mp_is_valid(transmitter),
		      "MP record symbol %zu/%zu changed", symbol,
		      symbol_count);
	}
	CHECK(sippymodem_v34_phase4_mp_record_is_complete(transmitter),
	      "MP record did not finish at its exact wire boundary");
	{
		struct sippymodem_v34_phase4_mp saved = *transmitter;
		struct sippymodem_qam_point point = { 7.0, -9.0 };
		uint8_t quarter = UINT8_MAX;
		uint8_t rotation = UINT8_MAX;
		bool complete = false;

		CHECK(sippymodem_v34_phase4_mp_next(
			      transmitter, &point, &quarter, &rotation,
			      &complete) ==
			      SIPPYMODEM_V34_PHASE4_MP_RECORD_COMPLETE &&
		      transmitters_are_equal(transmitter, &saved) &&
		      point.in_phase == 7.0 && point.quadrature == -9.0 &&
		      quarter == UINT8_MAX && rotation == UINT8_MAX &&
		      !complete,
		      "completed MP record emitted an extra symbol");
	}
	return true;
}

static bool test_role_and_constellation_records(void)
{
	static const enum sippymodem_v34_station_role roles[] = {
		SIPPYMODEM_V34_CALLING_STATION,
		SIPPYMODEM_V34_ANSWERING_STATION,
	};
	static const enum sippymodem_v34_constellation_size constellations[] = {
		SIPPYMODEM_V34_CONSTELLATION_4_POINT,
		SIPPYMODEM_V34_CONSTELLATION_16_POINT,
	};

	CHECK(SIPPYMODEM_V34_PHASE4_MP_4_POINT_SYMBOLS == 44 &&
	      SIPPYMODEM_V34_PHASE4_MP_16_POINT_SYMBOLS == 22,
	      "MP Type 0 symbol counts changed");
	for (size_t role_index = 0; role_index < ARRAY_LENGTH(roles);
	     ++role_index) {
		for (size_t constellation_index = 0;
		     constellation_index < ARRAY_LENGTH(constellations);
		     ++constellation_index) {
			struct sippymodem_v34_phase4_trn trn;
			struct sippymodem_v34_phase4_mp transmitter;
			struct sippymodem_v34_mp_parameters mp =
				parameters(false);
			struct sippymodem_v34_mp_parameters mp_prime =
				parameters(true);
			struct reference_mapper reference;

			CHECK(generate_trn_prefix(
				      &trn, roles[role_index],
				      constellations[constellation_index]),
			      "TRN prefix %zu/%zu failed", role_index,
			      constellation_index);
			CHECK(sippymodem_v34_phase4_mp_init_from_trn(
				      &transmitter, &trn, &mp) ==
				      SIPPYMODEM_V34_PHASE4_MP_OK &&
			      sippymodem_v34_phase4_mp_is_valid(&transmitter) &&
			      !sippymodem_v34_phase4_mp_record_is_complete(
				      &transmitter) &&
			      transmitter.scrambler.history ==
				      trn.scrambler.history &&
			      transmitter.scrambler.polynomial ==
				      trn.scrambler.polynomial &&
			      transmitter.previous_clockwise_quarter_turns ==
				      trn.last_clockwise_quarter_turns &&
			      transmitter.bit_index == 0,
			      "MP transition %zu/%zu did not inherit TRN state",
			      role_index, constellation_index);
			{
				struct sippymodem_v34_phase4_mp saved = transmitter;

				CHECK(sippymodem_v34_phase4_mp_start_next_record(
					      &transmitter, &mp_prime) ==
					      SIPPYMODEM_V34_PHASE4_MP_RECORD_IN_PROGRESS &&
				      transmitters_are_equal(&transmitter, &saved),
				      "MP transition %zu/%zu changed an active record",
				      role_index, constellation_index);
			}
			reference_mapper_init(&reference, &trn);
			CHECK(verify_record(&transmitter, &reference, &mp),
			      "MP record %zu/%zu failed", role_index,
			      constellation_index);
			{
				uint32_t history = transmitter.scrambler.history;
				uint8_t rotation =
					transmitter.previous_clockwise_quarter_turns;

				CHECK(sippymodem_v34_phase4_mp_start_next_record(
					      &transmitter, &mp_prime) ==
					      SIPPYMODEM_V34_PHASE4_MP_OK &&
				      transmitter.bit_index == 0 &&
				      transmitter.scrambler.history == history &&
				      transmitter.previous_clockwise_quarter_turns ==
					      rotation,
				      "MP-prime transition %zu/%zu reset mapper state",
				      role_index, constellation_index);
			}
			CHECK(verify_record(
				      &transmitter, &reference, &mp_prime),
			      "MP-prime record %zu/%zu failed", role_index,
			      constellation_index);
		}
	}
	return true;
}

static bool test_copy_behavior(void)
{
	struct sippymodem_v34_phase4_trn trn;
	struct sippymodem_v34_phase4_mp original;
	struct sippymodem_v34_phase4_mp copy;
	struct sippymodem_v34_mp_parameters selected = parameters(false);

	CHECK(generate_trn_prefix(
		      &trn, SIPPYMODEM_V34_CALLING_STATION,
		      SIPPYMODEM_V34_CONSTELLATION_16_POINT) &&
	      sippymodem_v34_phase4_mp_init_from_trn(
		      &original, &trn, &selected) ==
		      SIPPYMODEM_V34_PHASE4_MP_OK,
	      "MP copy fixture did not initialize");
	for (size_t symbol = 0; symbol < 7; ++symbol) {
		struct sippymodem_qam_point point;
		uint8_t quarter;
		uint8_t rotation;
		bool complete;

		CHECK(sippymodem_v34_phase4_mp_next(
			      &original, &point, &quarter, &rotation,
			      &complete) == SIPPYMODEM_V34_PHASE4_MP_OK &&
		      !complete,
		      "MP copy fixture stopped at symbol %zu", symbol);
	}
	copy = original;
	for (size_t symbol = 0; symbol < 5; ++symbol) {
		struct sippymodem_qam_point original_point;
		struct sippymodem_qam_point copied_point;
		uint8_t original_quarter;
		uint8_t copied_quarter;
		uint8_t original_rotation;
		uint8_t copied_rotation;
		bool original_complete;
		bool copied_complete;

		CHECK(sippymodem_v34_phase4_mp_next(
			      &original, &original_point, &original_quarter,
			      &original_rotation, &original_complete) ==
			      SIPPYMODEM_V34_PHASE4_MP_OK &&
		      sippymodem_v34_phase4_mp_next(
			      &copy, &copied_point, &copied_quarter,
			      &copied_rotation, &copied_complete) ==
			      SIPPYMODEM_V34_PHASE4_MP_OK &&
		      transmitters_are_equal(&original, &copy) &&
		      original_quarter == copied_quarter &&
		      original_rotation == copied_rotation &&
		      original_complete == copied_complete &&
		      points_are_close(&original_point, &copied_point),
		      "copied MP transmitter diverged at symbol %zu", symbol);
	}
	return true;
}

static bool test_validation(void)
{
	struct sippymodem_v34_phase4_trn trn;
	struct sippymodem_v34_phase4_mp transmitter;
	struct sippymodem_v34_phase4_mp saved;
	struct sippymodem_v34_mp_parameters valid = parameters(false);
	struct sippymodem_v34_mp_parameters invalid = valid;
	struct sippymodem_qam_point point = { 7.0, -9.0 };
	uint8_t quarter = UINT8_MAX;
	uint8_t rotation = UINT8_MAX;
	bool complete = true;

	memset(&transmitter, 0xa5, sizeof(transmitter));
	saved = transmitter;
	CHECK(sippymodem_v34_phase4_trn_init(
		      &trn, SIPPYMODEM_V34_ANSWERING_STATION,
		      SIPPYMODEM_V34_CONSTELLATION_4_POINT) ==
		      SIPPYMODEM_V34_PHASE4_TRN_OK &&
	      sippymodem_v34_phase4_mp_init_from_trn(
		      NULL, &trn, &valid) ==
		      SIPPYMODEM_V34_PHASE4_MP_INVALID_ARGUMENT &&
	      sippymodem_v34_phase4_mp_init_from_trn(
		      &transmitter, NULL, &valid) ==
		      SIPPYMODEM_V34_PHASE4_MP_INVALID_ARGUMENT &&
	      sippymodem_v34_phase4_mp_init_from_trn(
		      &transmitter, &trn, &valid) ==
		      SIPPYMODEM_V34_PHASE4_MP_INVALID_ARGUMENT &&
	      transmitters_are_equal(&transmitter, &saved),
	      "MP initialization accepted TRN without a final symbol");
	{
		struct sippymodem_qam_point trn_point;
		uint8_t trn_quarter;
		uint8_t trn_rotation;

		CHECK(sippymodem_v34_phase4_trn_next(
			      &trn, &trn_point, &trn_quarter,
			      &trn_rotation) == SIPPYMODEM_V34_PHASE4_TRN_OK,
		      "MP validation TRN did not emit a symbol");
	}
	invalid.maximum_call_to_answer_rate_2400_bps_units = 0;
	CHECK(sippymodem_v34_phase4_mp_init_from_trn(
		      &transmitter, &trn, NULL) ==
		      SIPPYMODEM_V34_PHASE4_MP_INVALID_ARGUMENT &&
	      sippymodem_v34_phase4_mp_init_from_trn(
		      &transmitter, &trn, &invalid) ==
		      SIPPYMODEM_V34_PHASE4_MP_INVALID_PARAMETERS &&
	      transmitters_are_equal(&transmitter, &saved),
	      "MP initialization accepted invalid parameters");
	CHECK(sippymodem_v34_phase4_mp_init_from_trn(
		      &transmitter, &trn, &valid) ==
		      SIPPYMODEM_V34_PHASE4_MP_OK,
	      "MP validation fixture did not initialize");
	saved = transmitter;
	CHECK(sippymodem_v34_phase4_mp_next(
		      NULL, &point, &quarter, &rotation, &complete) ==
		      SIPPYMODEM_V34_PHASE4_MP_INVALID_ARGUMENT &&
	      sippymodem_v34_phase4_mp_next(
		      &transmitter, NULL, &quarter, &rotation, &complete) ==
		      SIPPYMODEM_V34_PHASE4_MP_INVALID_ARGUMENT &&
	      sippymodem_v34_phase4_mp_next(
		      &transmitter, &point, NULL, &rotation, &complete) ==
		      SIPPYMODEM_V34_PHASE4_MP_INVALID_ARGUMENT &&
	      sippymodem_v34_phase4_mp_next(
		      &transmitter, &point, &quarter, NULL, &complete) ==
		      SIPPYMODEM_V34_PHASE4_MP_INVALID_ARGUMENT &&
	      sippymodem_v34_phase4_mp_next(
		      &transmitter, &point, &quarter, &rotation, NULL) ==
		      SIPPYMODEM_V34_PHASE4_MP_INVALID_ARGUMENT &&
	      transmitters_are_equal(&transmitter, &saved) &&
	      point.in_phase == 7.0 && point.quadrature == -9.0 &&
	      quarter == UINT8_MAX && rotation == UINT8_MAX && complete,
	      "invalid MP emission mutated state or outputs");

	transmitter = saved;
	transmitter.bit_index = SIPPYMODEM_V34_MP_TYPE0_FRAME_BITS;
	CHECK(sippymodem_v34_phase4_mp_is_valid(&transmitter),
	      "MP validation fixture did not reach a record boundary");
	saved = transmitter;
	CHECK(sippymodem_v34_phase4_mp_start_next_record(
		      NULL, &valid) ==
		      SIPPYMODEM_V34_PHASE4_MP_INVALID_ARGUMENT &&
	      sippymodem_v34_phase4_mp_start_next_record(
		      &transmitter, NULL) ==
		      SIPPYMODEM_V34_PHASE4_MP_INVALID_ARGUMENT &&
	      sippymodem_v34_phase4_mp_start_next_record(
		      &transmitter, &invalid) ==
		      SIPPYMODEM_V34_PHASE4_MP_INVALID_PARAMETERS &&
	      transmitters_are_equal(&transmitter, &saved),
	      "invalid MP replacement changed a completed record");

	transmitter = saved;
	transmitter.bit_index = 1;
	CHECK(!sippymodem_v34_phase4_mp_is_valid(&transmitter),
	      "MP accepted a partial symbol cursor");
	transmitter = saved;
	transmitter.frame[4] ^= 1u;
	CHECK(!sippymodem_v34_phase4_mp_is_valid(&transmitter),
	      "MP accepted a corrupt owned record");
	transmitter = saved;
	transmitter.previous_clockwise_quarter_turns = 4;
	CHECK(!sippymodem_v34_phase4_mp_is_valid(&transmitter),
	      "MP accepted an invalid differential phase");
	transmitter = saved;
	transmitter.role = SIPPYMODEM_V34_CALLING_STATION;
	CHECK(!sippymodem_v34_phase4_mp_is_valid(&transmitter),
	      "MP accepted a role/polynomial mismatch");
	transmitter = saved;
	transmitter.constellation =
		(enum sippymodem_v34_constellation_size)8;
	CHECK(!sippymodem_v34_phase4_mp_is_valid(&transmitter) &&
	      !sippymodem_v34_phase4_mp_is_valid(NULL) &&
	      !sippymodem_v34_phase4_mp_record_is_complete(NULL),
	      "MP accepted corrupt state");

	CHECK(strcmp(sippymodem_v34_phase4_mp_result_name(
		     SIPPYMODEM_V34_PHASE4_MP_RECORD_COMPLETE),
		     "record complete") == 0 &&
	      strcmp(sippymodem_v34_phase4_mp_result_name(
		     SIPPYMODEM_V34_PHASE4_MP_RECORD_IN_PROGRESS),
		     "record in progress") == 0 &&
	      strcmp(sippymodem_v34_phase4_mp_result_name(
		     (enum sippymodem_v34_phase4_mp_result)5),
		     "unknown") == 0,
	      "MP result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "role and constellation records",
		  test_role_and_constellation_records },
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
