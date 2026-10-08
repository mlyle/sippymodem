// SPDX-License-Identifier: BSD-3-Clause

#include "test.h"

#include "v90_phase3_analog_tx.h"

#include <stdint.h>
#include <string.h>

enum {
	PREFIX_FIXED_SYMBOLS =
		V34_PHASE3_S_SEQUENCE_SYMBOLS +
		V34_PHASE3_PP_SEQUENCE_SYMBOLS,
	EXTRA_TRN_SYMBOLS = 31,
};

static bool points_match(
	const struct qam_point *left, const struct qam_point *right)
{
	return left->in_phase == right->in_phase &&
	       left->quadrature == right->quadrature;
}

static bool trn_matches(
	const struct v34_training_sequence *left,
	const struct v34_training_sequence *right)
{
	return left->scrambler.history == right->scrambler.history &&
	       left->scrambler.polynomial == right->scrambler.polynomial &&
	       left->role == right->role &&
	       left->last_clockwise_quarter_turns ==
		       right->last_clockwise_quarter_turns &&
	       left->has_last_symbol == right->has_last_symbol;
}

static bool test_composed_sequence(void)
{
	struct v90_phase3_analog_tx tx;
	struct v34_phase3_transmitter prefix;
	struct v34_training_sequence boundary;
	struct v90_ja_tx ja;
	size_t prefix_symbols = PREFIX_FIXED_SYMBOLS +
				V34_TRAINING_SEQUENCE_MIN_SYMBOLS +
				EXTRA_TRN_SYMBOLS;

	CHECK(v90_phase3_analog_tx_init(&tx) == V90_PHASE3_ANALOG_TX_OK &&
	      v34_phase3_transmitter_init(
		      &prefix, V34_ANSWERING_STATION,
		      V34_CONSTELLATION_4_POINT) == V34_PHASE3_TRANSMITTER_OK &&
	      v90_phase3_analog_tx_is_valid(&tx) &&
	      tx.stage == V90_PHASE3_ANALOG_TX_PREFIX,
	      "analogue Phase 3 transmitter did not initialize");
	for (size_t symbol = 0; symbol < prefix_symbols; ++symbol) {
		struct qam_point actual;
		struct qam_point expected;

		if (symbol + 1u == PREFIX_FIXED_SYMBOLS +
					      V34_TRAINING_SEQUENCE_MIN_SYMBOLS) {
			struct v90_phase3_analog_tx saved = tx;

			CHECK(v90_phase3_analog_tx_start_ja(&tx) ==
				      V90_PHASE3_ANALOG_TX_INVALID_STATE &&
			      memcmp(&tx, &saved, sizeof(tx)) == 0,
			      "analogue Phase 3 ended TRN before 512 symbols");
		}
		CHECK(v90_phase3_analog_tx_next(&tx, &actual) ==
			      V90_PHASE3_ANALOG_TX_OK &&
		      v34_phase3_transmitter_next(&prefix, &expected) ==
			      V34_PHASE3_TRANSMITTER_OK &&
		      points_match(&actual, &expected),
		      "analogue Phase 3 prefix diverged at %zu", symbol);
	}
	CHECK(prefix.stage == V34_PHASE3_TRANSMITTER_TRN &&
	      v34_phase3_transmitter_trn_boundary(&prefix, &boundary) ==
		      V34_PHASE3_TRANSMITTER_OK &&
	      trn_matches(&boundary, &prefix.state.trn) &&
	      v90_ja_tx_init_from_trn(&ja, &boundary) == V90_JA_OK &&
	      v90_phase3_analog_tx_start_ja(&tx) ==
		      V90_PHASE3_ANALOG_TX_OK &&
	      tx.stage == V90_PHASE3_ANALOG_TX_JA,
	      "analogue TRN did not hand off to Ja");
	for (size_t symbol = 0;
	     symbol < V90_PHASE3_JA_DESCRIPTOR_SYMBOLS + 7u; ++symbol) {
		struct qam_point actual;
		struct qam_point expected;
		uint8_t rotation;

		if (symbol + 1u == V90_PHASE3_JA_DESCRIPTOR_SYMBOLS) {
			struct v90_phase3_analog_tx saved = tx;

			CHECK(v90_phase3_analog_tx_finish_ja(&tx) ==
				      V90_PHASE3_ANALOG_TX_INVALID_STATE &&
			      memcmp(&tx, &saved, sizeof(tx)) == 0,
			      "analogue Ja ended before one descriptor");
		}
		CHECK(v90_phase3_analog_tx_next(&tx, &actual) ==
			      V90_PHASE3_ANALOG_TX_OK &&
		      v90_ja_tx_next(&ja, &expected, &rotation) == V90_JA_OK &&
		      points_match(&actual, &expected) &&
		      tx.ja_symbols == symbol + 1u,
		      "analogue Ja diverged at %zu", symbol);
	}
	CHECK(tx.sequence.ja.bit_index == 14 &&
	      v90_phase3_analog_tx_finish_ja(&tx) ==
		      V90_PHASE3_ANALOG_TX_OK &&
	      tx.stage == V90_PHASE3_ANALOG_TX_COMPLETE,
	      "analogue Ja did not end between descriptor boundaries");
	{
		struct v90_phase3_analog_tx saved = tx;
		struct qam_point output = { 11.0, -6.0 };
		struct qam_point saved_output = output;

		CHECK(v90_phase3_analog_tx_next(&tx, &output) ==
			      V90_PHASE3_ANALOG_TX_DONE &&
		      memcmp(&tx, &saved, sizeof(tx)) == 0 &&
		      memcmp(&output, &saved_output, sizeof(output)) == 0,
		      "completed analogue Phase 3 emitted another symbol");
	}
	return true;
}

static bool reach_ja(struct v90_phase3_analog_tx *tx)
{
	struct qam_point point;
	size_t symbols = PREFIX_FIXED_SYMBOLS + V34_TRAINING_SEQUENCE_MIN_SYMBOLS;

	if (v90_phase3_analog_tx_init(tx) != V90_PHASE3_ANALOG_TX_OK)
		return false;
	for (size_t symbol = 0; symbol < symbols; ++symbol) {
		if (v90_phase3_analog_tx_next(tx, &point) !=
		    V90_PHASE3_ANALOG_TX_OK)
			return false;
	}
	return v90_phase3_analog_tx_start_ja(tx) ==
	       V90_PHASE3_ANALOG_TX_OK;
}

static bool test_boundaries_and_overflow(void)
{
	struct v90_phase3_analog_tx tx;
	struct v90_phase3_analog_tx saved;
	struct v34_training_sequence boundary;
	struct v34_training_sequence saved_boundary;
	struct qam_point point = { 3.0, -4.0 };
	struct qam_point saved_point = point;

	CHECK(v90_phase3_analog_tx_init(NULL) ==
		      V90_PHASE3_ANALOG_TX_INVALID_ARGUMENT &&
	      !v90_phase3_analog_tx_is_valid(NULL),
	      "analogue Phase 3 accepted a null owner");
	CHECK(v90_phase3_analog_tx_init(&tx) == V90_PHASE3_ANALOG_TX_OK,
	      "analogue boundary fixture did not initialize");
	memset(&boundary, 0xa5, sizeof(boundary));
	saved_boundary = boundary;
	CHECK(v34_phase3_transmitter_trn_boundary(
		      NULL, &boundary) == V34_PHASE3_TRANSMITTER_INVALID_ARGUMENT &&
	      v34_phase3_transmitter_trn_boundary(
		      &tx.sequence.prefix, NULL) ==
		      V34_PHASE3_TRANSMITTER_INVALID_ARGUMENT &&
	      v34_phase3_transmitter_trn_boundary(
		      &tx.sequence.prefix, &boundary) ==
		      V34_PHASE3_TRANSMITTER_INVALID_STATE &&
	      memcmp(&boundary, &saved_boundary, sizeof(boundary)) == 0,
	      "TRN boundary API was not transactional");
	saved = tx;
	CHECK(v90_phase3_analog_tx_next(NULL, &point) ==
		      V90_PHASE3_ANALOG_TX_INVALID_ARGUMENT &&
	      v90_phase3_analog_tx_next(&tx, NULL) ==
		      V90_PHASE3_ANALOG_TX_INVALID_ARGUMENT &&
	      v90_phase3_analog_tx_finish_ja(&tx) ==
		      V90_PHASE3_ANALOG_TX_INVALID_STATE &&
	      memcmp(&tx, &saved, sizeof(tx)) == 0 &&
	      memcmp(&point, &saved_point, sizeof(point)) == 0,
	      "analogue Phase 3 invalid call changed state or output");
	CHECK(reach_ja(&tx), "analogue overflow fixture did not reach Ja");
	saved = tx;
	CHECK(v90_phase3_analog_tx_start_ja(&tx) ==
		      V90_PHASE3_ANALOG_TX_INVALID_STATE &&
	      memcmp(&tx, &saved, sizeof(tx)) == 0,
	      "analogue Phase 3 restarted Ja");
	tx.ja_symbols = UINT32_MAX;
	tx.sequence.ja.bit_index = (uint16_t)(
		(UINT32_MAX % V90_PHASE3_JA_DESCRIPTOR_SYMBOLS) * 2u);
	CHECK(v90_phase3_analog_tx_is_valid(&tx),
	      "analogue overflow state was not representable");
	saved = tx;
	point = saved_point;
	CHECK(v90_phase3_analog_tx_next(&tx, &point) ==
		      V90_PHASE3_ANALOG_TX_INTERNAL_ERROR &&
	      memcmp(&tx, &saved, sizeof(tx)) == 0 &&
	      memcmp(&point, &saved_point, sizeof(point)) == 0,
	      "analogue Ja counter overflow was not transactional");
	tx.stage = (enum v90_phase3_analog_tx_stage)99;
	CHECK(!v90_phase3_analog_tx_is_valid(&tx),
	      "analogue Phase 3 validation changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "composed sequence", test_composed_sequence },
		{ "boundaries and overflow", test_boundaries_and_overflow },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		else {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0 ? 0 : 1;
}
