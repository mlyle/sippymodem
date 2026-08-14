// SPDX-License-Identifier: BSD-3-Clause

#include <v92_phase4_suvu_tx.h>

#include "test.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_LU = 5000,
};

static uint8_t wire_bit(const uint8_t *frame, size_t bit_index)
{
	return (uint8_t)(
		(frame[bit_index / 8u] >> (7u - bit_index % 8u)) & 1u);
}

static bool modulators_are_equal(
	const struct v92_phase4_modulator *left,
	const struct v92_phase4_modulator *right)
{
	return left->scrambler.history == right->scrambler.history &&
	       left->scrambler.polynomial == right->scrambler.polynomial &&
	       left->constellation == right->constellation &&
	       left->lu == right->lu &&
	       left->previous_encoded_sign == right->previous_encoded_sign;
}

static bool finish_training(
	struct v92_pcm_phase4_trn2u *trn2u,
	enum v92_constellation_size constellation, uint8_t previous_sign)
{
	CHECK(v92_pcm_phase4_trn2u_init(
		      trn2u, constellation, previous_sign, TEST_LU) ==
		      V92_PCM_PHASE4_TRN2U_OK,
	      "SUVu predecessor did not initialize");
	for (uint32_t symbol = 0;
	     symbol < V92_PCM_PHASE4_TRN2U_INITIAL_MIN_SYMBOLS; ++symbol) {
		int16_t sample;

		CHECK(v92_pcm_phase4_trn2u_next(trn2u, &sample) ==
			      V92_PCM_PHASE4_TRN2U_OK,
		      "SUVu predecessor failed at symbol %u", symbol);
	}
	CHECK(v92_pcm_phase4_trn2u_can_handoff(trn2u),
	      "SUVu predecessor did not reach its aligned handoff");
	return true;
}

static bool drive_record(
	struct v92_phase4_suvu_tx *tx,
	const struct v92_suvu_parameters *parameters,
	struct v92_phase4_modulator *oracle)
{
	uint8_t frame[V92_SUVU_FRAME_BYTES] = { 0 };
	size_t bit_count = 0;
	unsigned int bits = v92_phase4_modulator_bits_per_symbol(
		oracle->constellation);
	size_t symbol_count = V92_SUVU_FRAME_BITS / bits;

	CHECK(v92_suvu_encode(
		      parameters, oracle->constellation, frame, sizeof(frame),
		      &bit_count) == V92_SUVU_OK &&
	      bit_count == V92_SUVU_FRAME_BITS &&
	      memcmp(frame, tx->frame, sizeof(frame)) == 0,
	      "SUVu transmitter did not retain the canonical record");
	for (size_t symbol = 0; symbol < symbol_count; ++symbol) {
		uint8_t group = 0;
		int16_t expected = 0;
		int16_t actual = 0;

		for (unsigned int bit = 0; bit < bits; ++bit) {
			group |= (uint8_t)(
				wire_bit(frame, symbol * bits + bit) << bit);
		}
		CHECK(v92_phase4_modulator_next(oracle, group, &expected) ==
			      V92_PHASE4_MODULATOR_OK &&
		      v92_phase4_suvu_tx_next(tx, &actual) ==
			      V92_PHASE4_SUVU_TX_OK &&
		      actual == expected &&
		      tx->bit_index == (symbol + 1u) * bits &&
		      modulators_are_equal(&tx->modulator, oracle),
		      "SUVu modulation differed at symbol %zu/%zu", symbol,
		      symbol_count);
	}
	{
		struct v92_phase4_modulator handoff = { 0 };
		int16_t untouched = 1234;

		CHECK(v92_phase4_suvu_tx_record_is_complete(tx) &&
		      v92_phase4_suvu_tx_handoff(tx, &handoff) ==
			      V92_PHASE4_SUVU_TX_OK &&
		      modulators_are_equal(&handoff, oracle) &&
		      v92_phase4_suvu_tx_next(tx, &untouched) ==
			      V92_PHASE4_SUVU_TX_RECORD_COMPLETE &&
		      untouched == 1234,
		      "SUVu record did not end at its exact handoff");
	}
	return true;
}

static bool test_continuous_records(void)
{
	static const enum v92_constellation_size constellations[] = {
		V92_CONSTELLATION_4_POINT,
		V92_CONSTELLATION_8_POINT,
	};
	static const struct v92_suvu_parameters records[] = {
		{
			.prefilter_level_db_q2_2 = -16,
			.request_silence = true,
		},
		{
			.prefilter_level_db_q2_2 = 15,
			.wait_for_cpu = true,
			.acknowledges_cpd = true,
		},
	};

	for (size_t selection = 0;
	     selection < ARRAY_LENGTH(constellations); ++selection) {
		for (uint8_t previous_sign = 0;
		     previous_sign <= 1u; ++previous_sign) {
			struct v92_pcm_phase4_trn2u trn2u;
			struct v92_phase4_modulator oracle;
			struct v92_phase4_suvu_tx tx;

			CHECK(finish_training(
				      &trn2u, constellations[selection],
				      previous_sign) &&
			      v92_phase4_suvu_tx_init(
				      &tx, &trn2u, &records[0]) ==
				      V92_PHASE4_SUVU_TX_OK &&
			      tx.bit_index == 0u &&
			      modulators_are_equal(
				      &tx.modulator, &trn2u.modulator),
			      "initial SUVu record did not preserve TRN2u");
			oracle = trn2u.modulator;
			CHECK(drive_record(&tx, &records[0], &oracle) &&
			      v92_phase4_suvu_tx_start_next_record(
				      &tx, &records[1]) ==
				      V92_PHASE4_SUVU_TX_OK &&
			      tx.bit_index == 0u &&
			      modulators_are_equal(&tx.modulator, &oracle) &&
			      drive_record(&tx, &records[1], &oracle),
			      "repeated SUVu record lost coding continuity");
		}
	}
	return true;
}

static bool test_contracts(void)
{
	const struct v92_suvu_parameters valid = {
		.prefilter_level_db_q2_2 = 0,
	};
	const struct v92_suvu_parameters invalid = {
		.prefilter_level_db_q2_2 = 16,
	};
	struct v92_pcm_phase4_trn2u trn2u;
	struct v92_phase4_suvu_tx tx;
	struct v92_phase4_suvu_tx before;
	struct v92_phase4_modulator handoff;
	struct v92_phase4_modulator handoff_before;
	int16_t sample = 1234;
	static const char *const names[] = {
		"ok", "record complete", "record in progress",
		"invalid argument", "invalid parameters", "invalid state",
		"internal error",
	};

	memset(&tx, 0xa5, sizeof(tx));
	before = tx;
	CHECK(v92_phase4_suvu_tx_init(NULL, NULL, NULL) ==
		      V92_PHASE4_SUVU_TX_INVALID_ARGUMENT &&
	      v92_phase4_suvu_tx_init(&tx, NULL, &valid) ==
		      V92_PHASE4_SUVU_TX_INVALID_ARGUMENT &&
	      memcmp(&tx, &before, sizeof(tx)) == 0,
	      "invalid SUVu initialization changed its destination");
	CHECK(v92_pcm_phase4_trn2u_init(
		      &trn2u, V92_CONSTELLATION_4_POINT, 0u, TEST_LU) ==
		      V92_PCM_PHASE4_TRN2U_OK &&
	      v92_phase4_suvu_tx_init(&tx, &trn2u, &valid) ==
		      V92_PHASE4_SUVU_TX_INVALID_STATE &&
	      memcmp(&tx, &before, sizeof(tx)) == 0,
	      "premature SUVu handoff changed its destination");
	CHECK(finish_training(
		      &trn2u, V92_CONSTELLATION_4_POINT, 0u),
	      "SUVu contract predecessor failed");
	CHECK(v92_phase4_suvu_tx_init(&tx, &trn2u, &invalid) ==
		      V92_PHASE4_SUVU_TX_INVALID_PARAMETERS &&
	      memcmp(&tx, &before, sizeof(tx)) == 0 &&
	      v92_phase4_suvu_tx_init(&tx, &trn2u, &valid) ==
		      V92_PHASE4_SUVU_TX_OK,
	      "SUVu parameter validation was not transactional");
	before = tx;
	memset(&handoff, 0xa5, sizeof(handoff));
	handoff_before = handoff;
	CHECK(!v92_phase4_suvu_tx_record_is_complete(&tx) &&
	      v92_phase4_suvu_tx_start_next_record(&tx, &valid) ==
		      V92_PHASE4_SUVU_TX_RECORD_IN_PROGRESS &&
	      v92_phase4_suvu_tx_handoff(&tx, &handoff) ==
		      V92_PHASE4_SUVU_TX_RECORD_IN_PROGRESS &&
	      v92_phase4_suvu_tx_next(NULL, &sample) ==
		      V92_PHASE4_SUVU_TX_INVALID_ARGUMENT &&
	      v92_phase4_suvu_tx_next(&tx, NULL) ==
		      V92_PHASE4_SUVU_TX_INVALID_ARGUMENT &&
	      memcmp(&tx, &before, sizeof(tx)) == 0 &&
	      memcmp(&handoff, &handoff_before, sizeof(handoff)) == 0 &&
	      sample == 1234,
	      "in-progress SUVu operations were not transactional");
	tx.frame[0] ^= UINT8_C(0x80);
	before = tx;
	CHECK(!v92_phase4_suvu_tx_is_valid(&tx) &&
	      v92_phase4_suvu_tx_next(&tx, &sample) ==
		      V92_PHASE4_SUVU_TX_INVALID_STATE &&
	      v92_phase4_suvu_tx_start_next_record(&tx, &valid) ==
		      V92_PHASE4_SUVU_TX_INVALID_STATE &&
	      v92_phase4_suvu_tx_handoff(&tx, &handoff) ==
		      V92_PHASE4_SUVU_TX_INVALID_STATE &&
	      memcmp(&tx, &before, sizeof(tx)) == 0 && sample == 1234,
	      "invalid SUVu state was not rejected transactionally");
	CHECK(!v92_phase4_suvu_tx_is_valid(NULL) &&
	      v92_phase4_suvu_tx_handoff(NULL, &handoff) ==
		      V92_PHASE4_SUVU_TX_INVALID_ARGUMENT &&
	      v92_phase4_suvu_tx_handoff(&tx, NULL) ==
		      V92_PHASE4_SUVU_TX_INVALID_ARGUMENT,
	      "null SUVu contract arguments were accepted");
	for (size_t index = 0; index < ARRAY_LENGTH(names); ++index)
		CHECK(strcmp(
			      v92_phase4_suvu_tx_result_name(
				      (enum v92_phase4_suvu_tx_result)index),
			      names[index]) == 0,
		      "SUVu transmitter result name %zu differed", index);
	CHECK(strcmp(
		      v92_phase4_suvu_tx_result_name(
			      (enum v92_phase4_suvu_tx_result)-1),
		      "unknown") == 0,
	      "unknown SUVu transmitter result name differed");
	return true;
}

int main(void)
{
	fprintf(stderr, "[ RUN      ] V.92 Phase 4 SUVu transmitter\n");
	if (!test_continuous_records() || !test_contracts()) {
		fprintf(stderr, "[  FAILED  ] V.92 Phase 4 SUVu transmitter\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] V.92 Phase 4 SUVu transmitter\n");
	return 0;
}
