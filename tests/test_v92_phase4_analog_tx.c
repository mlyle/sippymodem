// SPDX-License-Identifier: BSD-3-Clause

#include <v92_phase4_analog_tx.h>

#include "test.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
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

static struct v92_cpu_parameters cpu_parameters(void)
{
	struct v92_cpu_parameters parameters = {
		.law = G711_LAW_PCMU,
		.trn1d_ratio_q3_13 = UINT16_C(0x2000),
		.data_rate_number = 21u,
		.sign_redundancy = 1u,
		.spectral_lookahead_frames = 1u,
		.constellation_count = 1u,
	};

	add_ucode(&parameters.transmit_constellations[0], 1u);
	add_ucode(&parameters.transmit_constellations[0], 31u);
	return parameters;
}

static struct v92_suvu_parameters suvu_parameters(void)
{
	return (struct v92_suvu_parameters){
		.prefilter_level_db_q2_2 = V92_SUVU_NO_MEASUREMENT_Q2_2,
		.wait_for_cpu = true,
	};
}

static bool initialize(
	struct v92_phase4_analog_tx *tx,
	enum v92_constellation_size constellation)
{
	struct v92_cpu_parameters cpu = cpu_parameters();
	struct v92_suvu_parameters suvu = suvu_parameters();

	CHECK(v92_phase4_analog_tx_init(
		      tx, constellation, 1u, TEST_LU, &suvu, &cpu) ==
		      V92_PHASE4_ANALOG_TX_OK &&
	      v92_phase4_analog_tx_is_valid(tx) &&
	      tx->stage == V92_PHASE4_ANALOG_TX_TRN2U,
	      "analogue Phase 4 transmitter did not initialize");
	return true;
}

static bool emit_symbols(
	struct v92_phase4_analog_tx *tx, size_t count)
{
	for (size_t symbol = 0u; symbol < count; ++symbol) {
		int16_t sample;

		CHECK(v92_phase4_analog_tx_next(tx, &sample) ==
			      V92_PHASE4_ANALOG_TX_OK,
		      "analogue Phase 4 output failed at symbol %zu", symbol);
	}
	return true;
}

static size_t record_symbols(const struct v92_phase4_analog_tx *tx)
{
	unsigned int bits = v92_phase4_modulator_bits_per_symbol(
		tx->signal.record.modulator.constellation);

	return tx->signal.record.frame_bit_count / bits;
}

static bool finish_record(struct v92_phase4_analog_tx *tx)
{
	size_t remaining;
	unsigned int bits = v92_phase4_modulator_bits_per_symbol(
		tx->signal.record.modulator.constellation);

	CHECK(bits != 0u && tx->signal.record.bit_index <=
		      tx->signal.record.frame_bit_count,
	      "record progress was invalid");
	remaining = (tx->signal.record.frame_bit_count -
		     tx->signal.record.bit_index) / bits;
	return emit_symbols(tx, remaining);
}

static bool current_suvu_acknowledges_cpd(
	const struct v92_phase4_analog_tx *tx, bool expected)
{
	struct v92_suvu_parameters decoded;

	CHECK(tx->stage == V92_PHASE4_ANALOG_TX_SUVU &&
	      v92_suvu_decode(
		      tx->signal.record.frame, V92_SUVU_FRAME_BYTES,
		      tx->signal.record.frame_bit_count,
		      v92_phase4_modulator_bits_per_symbol(
			      tx->signal.record.modulator.constellation) *
			      V92_SUVU_SYMBOLS_PER_FRAME,
		      &decoded) == V92_SUVU_OK &&
	      decoded.acknowledges_cpd == expected,
	      "SUVu acknowledgment differed");
	return true;
}

static bool current_cpu_acknowledges_cpd(
	const struct v92_phase4_analog_tx *tx, bool expected)
{
	struct v92_cpu_parameters decoded;
	unsigned int bits = v92_phase4_modulator_bits_per_symbol(
		tx->signal.record.modulator.constellation);

	CHECK(tx->stage == V92_PHASE4_ANALOG_TX_CPU &&
	      v92_cpu_decode(
		      tx->signal.record.frame, V92_CPU_MAX_FRAME_BYTES,
		      tx->signal.record.frame_bit_count,
		      bits * V92_CPU_SYMBOLS_PER_FRAME, &decoded) ==
		      V92_CPU_OK &&
	      decoded.acknowledges_cpd == expected,
	      "CPu acknowledgment differed");
	return true;
}

static bool test_early_peer_sequence(void)
{
	struct v92_phase4_analog_tx tx;
	struct v92_suvd_parameters suvd = {
		.request_silence = true,
	};
	struct v92_phase4_modulator handoff;
	int16_t sample = 1234;

	CHECK(initialize(&tx, V92_CONSTELLATION_4_POINT) &&
	      emit_symbols(&tx, 13u) &&
	      v92_phase4_analog_tx_note_suvd(&tx, &suvd) ==
		      V92_PHASE4_ANALOG_TX_OK &&
	      tx.cpu_pending && v92_phase4_analog_tx_silence_requested(&tx) &&
	      emit_symbols(&tx, 11u) &&
	      tx.stage == V92_PHASE4_ANALOG_TX_TRN2U &&
	      tx.signal.trn2u.emitted_symbols == 24u,
	      "early SUVd did not preserve the aligned TRN2u boundary");
	CHECK(emit_symbols(&tx, 1u) &&
	      tx.stage == V92_PHASE4_ANALOG_TX_SUVU &&
	      tx.signal.record.bit_index == 2u &&
	      current_suvu_acknowledges_cpd(&tx, false) &&
	      finish_record(&tx) && tx.cpu_pending,
	      "early SUVd did not produce the first SUVu");
	CHECK(emit_symbols(&tx, 1u) &&
	      tx.stage == V92_PHASE4_ANALOG_TX_CPU &&
	      !tx.cpu_pending && current_cpu_acknowledges_cpd(&tx, false) &&
	      finish_record(&tx) && tx.cpu_sent &&
	      tx.completed_cpu_records == 1u &&
	      v92_phase4_record_tx_is_complete(&tx.signal.record),
	      "first CPu was not sent after downstream status");
	CHECK(emit_symbols(&tx, 1u) &&
	      current_suvu_acknowledges_cpd(&tx, false) &&
	      v92_phase4_analog_tx_note_cpd(&tx, false) ==
		      V92_PHASE4_ANALOG_TX_OK,
	      "CPd event was not accepted during SUVu");
	suvd.acknowledges_cpu = true;
	CHECK(v92_phase4_analog_tx_note_suvd(&tx, &suvd) ==
		      V92_PHASE4_ANALOG_TX_OK &&
	      finish_record(&tx) && !tx.acknowledgment_sent &&
	      emit_symbols(&tx, 1u) &&
	      current_suvu_acknowledges_cpd(&tx, true) &&
	      finish_record(&tx) &&
	      v92_phase4_analog_tx_is_e2u_ready(&tx),
	      "mutual Phase 4 acknowledgment did not reach E2u");
	memset(&handoff, 0, sizeof(handoff));
	CHECK(v92_phase4_analog_tx_handoff(&tx, &handoff) ==
		      V92_PHASE4_ANALOG_TX_OK &&
	      v92_phase4_modulator_is_valid(&handoff) &&
	      v92_phase4_analog_tx_next(&tx, &sample) ==
		      V92_PHASE4_ANALOG_TX_E2U_READY_RESULT &&
	      sample == 1234,
	      "E2u boundary did not preserve coding history");
	return true;
}

static bool test_timeout_and_retry(void)
{
	struct v92_phase4_analog_tx tx;
	struct v92_suvd_parameters suvd = { 0 };
	size_t first_status_symbols;

	CHECK(initialize(&tx, V92_CONSTELLATION_8_POINT) &&
	      emit_symbols(
		      &tx, V92_PCM_PHASE4_TRN2U_INITIAL_MIN_SYMBOLS) &&
	      tx.stage == V92_PHASE4_ANALOG_TX_TRN2U &&
	      emit_symbols(&tx, 1u) &&
	      tx.stage == V92_PHASE4_ANALOG_TX_SUVU,
	      "TRN2u timeout did not start SUVu");
	first_status_symbols = record_symbols(&tx);
	CHECK(finish_record(&tx) && emit_symbols(&tx, 1u) &&
	      tx.stage == V92_PHASE4_ANALOG_TX_SUVU &&
	      tx.signal.record.bit_index == 3u &&
	      record_symbols(&tx) == first_status_symbols,
	      "status did not repeat before SUVd");
	CHECK(v92_phase4_analog_tx_note_suvd(&tx, &suvd) ==
		      V92_PHASE4_ANALOG_TX_OK &&
	      finish_record(&tx) && emit_symbols(&tx, 1u) &&
	      tx.stage == V92_PHASE4_ANALOG_TX_CPU &&
	      finish_record(&tx) && tx.cpu_sent &&
	      tx.completed_cpu_records == 1u &&
	      emit_symbols(&tx, 1u) &&
	      tx.stage == V92_PHASE4_ANALOG_TX_SUVU,
	      "timeout path did not send its first CPu");
	CHECK(v92_phase4_analog_tx_repeat_cpu(&tx) ==
		      V92_PHASE4_ANALOG_TX_OK &&
	      v92_phase4_analog_tx_repeat_cpu(&tx) ==
		      V92_PHASE4_ANALOG_TX_INVALID_STATE &&
	      finish_record(&tx) && emit_symbols(&tx, 1u) &&
	      tx.stage == V92_PHASE4_ANALOG_TX_CPU &&
	      finish_record(&tx) && tx.cpu_sent &&
	      tx.completed_cpu_records == 2u &&
	      !v92_phase4_analog_tx_is_e2u_ready(&tx),
	      "timer-requested CPu retry was not exactly one record");
	CHECK(emit_symbols(&tx, 1u) &&
	      v92_phase4_analog_tx_note_cpd(&tx, false) ==
		      V92_PHASE4_ANALOG_TX_OK &&
	      finish_record(&tx) && emit_symbols(&tx, 1u) &&
	      current_suvu_acknowledges_cpd(&tx, true) &&
	      finish_record(&tx) && tx.acknowledgment_sent &&
	      !v92_phase4_analog_tx_is_e2u_ready(&tx) &&
	      v92_phase4_analog_tx_note_ed(&tx) ==
		      V92_PHASE4_ANALOG_TX_OK &&
	      v92_phase4_analog_tx_is_e2u_ready(&tx),
	      "Ed did not complete an acknowledged status boundary");
	return true;
}

static bool test_contracts(void)
{
	struct v92_suvu_parameters suvu = suvu_parameters();
	struct v92_cpu_parameters cpu = cpu_parameters();
	struct v92_phase4_analog_tx tx;
	struct v92_phase4_analog_tx ed_tx;
	struct v92_phase4_analog_tx cancel_tx;
	struct v92_phase4_analog_tx before;
	struct v92_phase4_modulator handoff;
	struct v92_phase4_modulator handoff_before;
	struct v92_suvd_parameters suvd = { 0 };
	int16_t sample = 1234;
	static const char *const names[] = {
		"ok", "E2u ready", "invalid argument", "invalid parameters",
		"invalid state", "internal error",
	};

	memset(&tx, 0xa5, sizeof(tx));
	before = tx;
	CHECK(v92_phase4_analog_tx_init(
		      NULL, V92_CONSTELLATION_4_POINT, 0u, TEST_LU,
		      &suvu, &cpu) == V92_PHASE4_ANALOG_TX_INVALID_ARGUMENT &&
	      v92_phase4_analog_tx_init(
		      &tx, V92_CONSTELLATION_4_POINT, 0u, TEST_LU,
		      NULL, &cpu) == V92_PHASE4_ANALOG_TX_INVALID_ARGUMENT &&
	      memcmp(&tx, &before, sizeof(tx)) == 0,
	      "null initialization changed its destination");
	suvu.acknowledges_cpd = true;
	CHECK(v92_phase4_analog_tx_init(
		      &tx, V92_CONSTELLATION_4_POINT, 0u, TEST_LU,
		      &suvu, &cpu) == V92_PHASE4_ANALOG_TX_INVALID_PARAMETERS &&
	      memcmp(&tx, &before, sizeof(tx)) == 0,
	      "caller-controlled SUVu acknowledgment was accepted");
	suvu.acknowledges_cpd = false;
	cpu.acknowledges_cpd = true;
	CHECK(v92_phase4_analog_tx_init(
		      &tx, V92_CONSTELLATION_4_POINT, 0u, TEST_LU,
		      &suvu, &cpu) == V92_PHASE4_ANALOG_TX_INVALID_PARAMETERS &&
	      memcmp(&tx, &before, sizeof(tx)) == 0,
	      "caller-controlled CPu acknowledgment was accepted");
	cpu.acknowledges_cpd = false;
	CHECK(v92_phase4_analog_tx_init(
		      &tx, V92_CONSTELLATION_4_POINT, 0u, INT16_MAX,
		      &suvu, &cpu) == V92_PHASE4_ANALOG_TX_INVALID_PARAMETERS &&
	      memcmp(&tx, &before, sizeof(tx)) == 0 &&
	      initialize(&tx, V92_CONSTELLATION_4_POINT),
	      "invalid modulation initialization was not transactional");
	CHECK(initialize(&ed_tx, V92_CONSTELLATION_4_POINT) &&
	      v92_phase4_analog_tx_note_ed(&ed_tx) ==
		      V92_PHASE4_ANALOG_TX_OK &&
	      ed_tx.peer_confirmation_received && !ed_tx.received_cpd,
	      "Ed was confused with receipt of CPd");
	CHECK(initialize(&cancel_tx, V92_CONSTELLATION_4_POINT) &&
	      v92_phase4_analog_tx_note_suvd(&cancel_tx, &suvd) ==
		      V92_PHASE4_ANALOG_TX_OK &&
	      emit_symbols(&cancel_tx, 25u) && finish_record(&cancel_tx) &&
	      emit_symbols(&cancel_tx, 1u) && finish_record(&cancel_tx) &&
	      emit_symbols(&cancel_tx, 1u) &&
	      v92_phase4_analog_tx_repeat_cpu(&cancel_tx) ==
		      V92_PHASE4_ANALOG_TX_OK &&
	      cancel_tx.cpu_pending &&
	      v92_phase4_analog_tx_note_ed(&cancel_tx) ==
		      V92_PHASE4_ANALOG_TX_OK &&
	      !cancel_tx.cpu_pending,
	      "peer confirmation did not cancel a queued CPu retry");
	before = tx;
	memset(&handoff, 0xa5, sizeof(handoff));
	handoff_before = handoff;
	CHECK(v92_phase4_analog_tx_note_suvd(NULL, &suvd) ==
		      V92_PHASE4_ANALOG_TX_INVALID_ARGUMENT &&
	      v92_phase4_analog_tx_note_suvd(&tx, NULL) ==
		      V92_PHASE4_ANALOG_TX_INVALID_ARGUMENT &&
	      v92_phase4_analog_tx_note_cpd(NULL, false) ==
		      V92_PHASE4_ANALOG_TX_INVALID_ARGUMENT &&
	      v92_phase4_analog_tx_note_ed(NULL) ==
		      V92_PHASE4_ANALOG_TX_INVALID_ARGUMENT &&
	      v92_phase4_analog_tx_repeat_cpu(&tx) ==
		      V92_PHASE4_ANALOG_TX_INVALID_STATE &&
	      v92_phase4_analog_tx_next(NULL, &sample) ==
		      V92_PHASE4_ANALOG_TX_INVALID_ARGUMENT &&
	      v92_phase4_analog_tx_next(&tx, NULL) ==
		      V92_PHASE4_ANALOG_TX_INVALID_ARGUMENT &&
	      v92_phase4_analog_tx_next(
		      &tx, (int16_t *)(void *)&tx) ==
		      V92_PHASE4_ANALOG_TX_INVALID_ARGUMENT &&
	      v92_phase4_analog_tx_handoff(&tx, &handoff) ==
		      V92_PHASE4_ANALOG_TX_INVALID_STATE &&
	      v92_phase4_analog_tx_handoff(
		      &tx, (struct v92_phase4_modulator *)(void *)&tx) ==
		      V92_PHASE4_ANALOG_TX_INVALID_ARGUMENT &&
	      memcmp(&tx, &before, sizeof(tx)) == 0 &&
	      memcmp(&handoff, &handoff_before, sizeof(handoff)) == 0 &&
	      sample == 1234,
	      "invalid operations changed state or output");
	tx.stage = (enum v92_phase4_analog_tx_stage)-1;
	before = tx;
	CHECK(!v92_phase4_analog_tx_is_valid(&tx) &&
	      v92_phase4_analog_tx_next(&tx, &sample) ==
		      V92_PHASE4_ANALOG_TX_INVALID_STATE &&
	      memcmp(&tx, &before, sizeof(tx)) == 0 && sample == 1234 &&
	      !v92_phase4_analog_tx_is_valid(NULL) &&
	      !v92_phase4_analog_tx_is_e2u_ready(NULL) &&
	      !v92_phase4_analog_tx_silence_requested(NULL),
	      "invalid state was not rejected transactionally");
	for (size_t index = 0u; index < ARRAY_LENGTH(names); ++index)
		CHECK(strcmp(
			      v92_phase4_analog_tx_result_name(
				      (enum v92_phase4_analog_tx_result)index),
			      names[index]) == 0,
		      "analogue Phase 4 result name %zu differed", index);
	CHECK(strcmp(
		      v92_phase4_analog_tx_result_name(
			      (enum v92_phase4_analog_tx_result)-1),
		      "unknown") == 0,
	      "unknown analogue Phase 4 result name differed");
	return true;
}

int main(void)
{
	fprintf(stderr, "[ RUN      ] V.92 analogue Phase 4 control\n");
	if (!test_early_peer_sequence() || !test_timeout_and_retry() ||
	    !test_contracts()) {
		fprintf(stderr, "[  FAILED  ] V.92 analogue Phase 4 control\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] V.92 analogue Phase 4 control\n");
	return 0;
}
