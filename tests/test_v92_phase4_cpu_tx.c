// SPDX-License-Identifier: BSD-3-Clause

#include <v92_phase4_cpu_tx.h>

#include "test.h"

#include <string.h>

enum {
	TEST_LU = 5000,
};

static void add_ucode(
	struct pcm_constellation *constellation, uint8_t ucode)
{
	constellation->ucode_mask[ucode / 8u] |=
		(uint8_t)(1u << (ucode % 8u));
}

static struct v92_cpu_parameters parameters(
	uint8_t count, bool codec_differs, bool acknowledgment)
{
	struct v92_cpu_parameters value = {
		.law = acknowledgment ? G711_LAW_PCMA : G711_LAW_PCMU,
		.trn1d_ratio_q3_13 = UINT16_C(0x8765),
		.shaping_a1_q1_6 = -64,
		.shaping_a2_q1_6 = 63,
		.shaping_b1_q1_6 = -1,
		.shaping_b2_q1_6 = -127,
		.data_rate_number = V92_CPU_MAX_DATA_RATE_NUMBER,
		.sign_redundancy = 2u,
		.spectral_lookahead_frames = 3u,
		.constellation_count = count,
		.acknowledges_cpd = acknowledgment,
		.codec_constellations_differ = codec_differs,
	};

	for (uint8_t index = 0u; index < count; ++index) {
		add_ucode(&value.transmit_constellations[index], index);
		add_ucode(
			&value.transmit_constellations[index],
			(uint8_t)(127u - index));
		if (codec_differs) {
			add_ucode(
				&value.codec_constellations[index],
				(uint8_t)(64u + index));
		}
	}
	for (size_t interval = 0u;
	     interval < V92_CPU_FRAME_INTERVALS; ++interval) {
		value.frame_constellation[interval] =
			(uint8_t)(interval % count);
	}
	return value;
}

static uint8_t wire_bit(const uint8_t *frame, size_t bit)
{
	return (uint8_t)(
		(frame[bit / 8u] >> (7u - bit % 8u)) & 1u);
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

static bool advance_predecessor(
	struct v92_phase4_modulator *modulator)
{
	unsigned int bits = v92_phase4_modulator_bits_per_symbol(
		modulator->constellation);

	for (unsigned int symbol = 0u; symbol < 19u; ++symbol) {
		uint8_t group = (uint8_t)(
			(symbol * 5u + 1u) & ((1u << bits) - 1u));
		int16_t sample;

		CHECK(v92_phase4_modulator_next(
			      modulator, group, &sample) ==
			      V92_PHASE4_MODULATOR_OK,
		      "CPu predecessor failed at symbol %u", symbol);
	}
	return true;
}

static bool drive_record(
	struct v92_phase4_cpu_tx *tx,
	const struct v92_cpu_parameters *parameters,
	struct v92_phase4_modulator *oracle)
{
	uint8_t frame[V92_CPU_MAX_FRAME_BYTES] = { 0 };
	size_t frame_bits = 0u;
	unsigned int bits = v92_phase4_modulator_bits_per_symbol(
		oracle->constellation);

	CHECK(v92_cpu_encode(
		      parameters, bits, frame, sizeof(frame), &frame_bits) ==
		      V92_CPU_OK &&
	      tx->record.frame_bit_count == frame_bits &&
	      memcmp(
		      tx->record.frame, frame,
		      (frame_bits + 7u) / 8u) == 0,
	      "CPu transmitter did not retain its canonical record");
	for (size_t first_bit = 0u;
	     first_bit < frame_bits; first_bit += bits) {
		uint8_t group = 0u;
		int16_t expected = 0;
		int16_t actual = 0;

		for (unsigned int bit = 0u; bit < bits; ++bit) {
			group |= (uint8_t)(
				wire_bit(frame, first_bit + bit) << bit);
		}
		CHECK(v92_phase4_modulator_next(
			      oracle, group, &expected) ==
			      V92_PHASE4_MODULATOR_OK &&
		      v92_phase4_cpu_tx_next(tx, &actual) ==
			      V92_PHASE4_CPU_TX_OK &&
		      actual == expected &&
		      tx->record.bit_index == first_bit + bits &&
		      modulators_are_equal(&tx->record.modulator, oracle),
		      "CPu modulation differed at bit %zu/%zu",
		      first_bit, frame_bits);
	}
	{
		struct v92_phase4_modulator handoff;
		int16_t untouched = 1234;

		CHECK(v92_phase4_cpu_tx_record_is_complete(tx) &&
		      v92_phase4_cpu_tx_handoff(tx, &handoff) ==
			      V92_PHASE4_CPU_TX_OK &&
		      modulators_are_equal(&handoff, oracle) &&
		      v92_phase4_cpu_tx_next(tx, &untouched) ==
			      V92_PHASE4_CPU_TX_RECORD_COMPLETE &&
		      untouched == 1234,
		      "CPu record did not end at its exact handoff");
	}
	return true;
}

static bool test_continuous_records(void)
{
	static const enum v92_constellation_size constellations[] = {
		V92_CONSTELLATION_4_POINT,
		V92_CONSTELLATION_8_POINT,
	};
	struct v92_cpu_parameters first = parameters(1u, false, false);
	struct v92_cpu_parameters second = parameters(
		V92_CPU_MAX_CONSTELLATIONS, true, true);

	for (size_t selection = 0u;
	     selection < ARRAY_LENGTH(constellations); ++selection) {
		struct v92_phase4_modulator predecessor;
		struct v92_phase4_modulator oracle;
		struct v92_phase4_cpu_tx tx;

		CHECK(v92_phase4_modulator_init(
			      &predecessor, constellations[selection],
			      (uint8_t)selection, TEST_LU) ==
			      V92_PHASE4_MODULATOR_OK &&
		      advance_predecessor(&predecessor) &&
		      v92_phase4_cpu_tx_init(
			      &tx, &predecessor, &first) ==
			      V92_PHASE4_CPU_TX_OK &&
		      tx.record.bit_index == 0u &&
		      modulators_are_equal(
			      &tx.record.modulator, &predecessor),
		      "CPu did not preserve its predecessor");
		oracle = predecessor;
		CHECK(drive_record(&tx, &first, &oracle) &&
		      v92_phase4_cpu_tx_start_next_record(
			      &tx, &second) == V92_PHASE4_CPU_TX_OK &&
		      tx.record.bit_index == 0u &&
		      modulators_are_equal(&tx.record.modulator, &oracle) &&
		      drive_record(&tx, &second, &oracle),
		      "repeated CPu record lost coding continuity");
	}
	return true;
}

static bool test_contracts(void)
{
	struct v92_cpu_parameters valid = parameters(1u, false, false);
	struct v92_cpu_parameters invalid = valid;
	struct v92_phase4_modulator modulator;
	struct v92_phase4_modulator invalid_modulator = { 0 };
	struct v92_phase4_modulator handoff;
	struct v92_phase4_modulator handoff_before;
	struct v92_phase4_cpu_tx tx;
	struct v92_phase4_cpu_tx before;
	int16_t sample = 1234;
	static const char *const names[] = {
		"ok", "record complete", "record in progress",
		"invalid argument", "invalid parameters", "invalid state",
		"internal error",
	};

	invalid.constellation_count = 0u;
	CHECK(v92_phase4_modulator_init(
		      &modulator, V92_CONSTELLATION_4_POINT,
		      0u, TEST_LU) == V92_PHASE4_MODULATOR_OK,
	      "CPu contract modulator did not initialize");
	memset(&tx, 0xa5, sizeof(tx));
	before = tx;
	CHECK(v92_phase4_cpu_tx_init(NULL, &modulator, &valid) ==
		      V92_PHASE4_CPU_TX_INVALID_ARGUMENT &&
	      v92_phase4_cpu_tx_init(&tx, NULL, &valid) ==
		      V92_PHASE4_CPU_TX_INVALID_ARGUMENT &&
	      v92_phase4_cpu_tx_init(&tx, &modulator, NULL) ==
		      V92_PHASE4_CPU_TX_INVALID_ARGUMENT &&
	      v92_phase4_cpu_tx_init(
		      &tx, &invalid_modulator, &valid) ==
		      V92_PHASE4_CPU_TX_INVALID_STATE &&
	      v92_phase4_cpu_tx_init(&tx, &modulator, &invalid) ==
		      V92_PHASE4_CPU_TX_INVALID_PARAMETERS &&
	      memcmp(&tx, &before, sizeof(tx)) == 0,
	      "invalid CPu initialization changed state");
	CHECK(v92_phase4_cpu_tx_init(&tx, &modulator, &valid) ==
		      V92_PHASE4_CPU_TX_OK,
	      "valid CPu initialization failed");
	before = tx;
	memset(&handoff, 0xa5, sizeof(handoff));
	handoff_before = handoff;
	CHECK(v92_phase4_cpu_tx_start_next_record(&tx, &valid) ==
		      V92_PHASE4_CPU_TX_RECORD_IN_PROGRESS &&
	      v92_phase4_cpu_tx_handoff(&tx, &handoff) ==
		      V92_PHASE4_CPU_TX_RECORD_IN_PROGRESS &&
	      v92_phase4_cpu_tx_next(NULL, &sample) ==
		      V92_PHASE4_CPU_TX_INVALID_ARGUMENT &&
	      v92_phase4_cpu_tx_next(&tx, NULL) ==
		      V92_PHASE4_CPU_TX_INVALID_ARGUMENT &&
	      memcmp(&tx, &before, sizeof(tx)) == 0 &&
	      memcmp(&handoff, &handoff_before, sizeof(handoff)) == 0 &&
	      sample == 1234,
	      "in-progress CPu operations were not transactional");
	tx.record.frame[0] ^= UINT8_C(0x80);
	before = tx;
	CHECK(!v92_phase4_cpu_tx_is_valid(&tx) &&
	      v92_phase4_cpu_tx_next(&tx, &sample) ==
		      V92_PHASE4_CPU_TX_INVALID_STATE &&
	      v92_phase4_cpu_tx_start_next_record(&tx, &valid) ==
		      V92_PHASE4_CPU_TX_INVALID_STATE &&
	      v92_phase4_cpu_tx_handoff(&tx, &handoff) ==
		      V92_PHASE4_CPU_TX_INVALID_STATE &&
	      memcmp(&tx, &before, sizeof(tx)) == 0 && sample == 1234,
	      "invalid CPu state was not rejected transactionally");
	CHECK(!v92_phase4_cpu_tx_is_valid(NULL) &&
	      v92_phase4_cpu_tx_handoff(NULL, &handoff) ==
		      V92_PHASE4_CPU_TX_INVALID_ARGUMENT &&
	      v92_phase4_cpu_tx_handoff(&tx, NULL) ==
		      V92_PHASE4_CPU_TX_INVALID_ARGUMENT,
	      "null CPu contract arguments were accepted");
	for (size_t index = 0u; index < ARRAY_LENGTH(names); ++index) {
		CHECK(strcmp(
			      v92_phase4_cpu_tx_result_name(
				      (enum v92_phase4_cpu_tx_result)index),
			      names[index]) == 0,
		      "CPu transmitter result name %zu differed", index);
	}
	CHECK(strcmp(
		      v92_phase4_cpu_tx_result_name(
			      (enum v92_phase4_cpu_tx_result)-1),
		      "unknown") == 0,
	      "unknown CPu transmitter result name differed");
	return true;
}

int main(void)
{
	fprintf(stderr, "[ RUN      ] V.92 Phase 4 CPu transmitter\n");
	if (!test_continuous_records() || !test_contracts()) {
		fprintf(stderr, "[  FAILED  ] V.92 Phase 4 CPu transmitter\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] V.92 Phase 4 CPu transmitter\n");
	return 0;
}
