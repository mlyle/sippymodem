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

static bool test_record_bit_order(void)
{
	/* Storage is MSB-first, but each two/three-bit modulation group is
	 * LSB-first. Three-bit symbols also straddle storage byte boundaries. */
	static const uint8_t frame[] = { 0x1b, 0xe4, 0x59 };
	static const uint8_t groups[][12] = {
		{ 0, 2, 1, 3, 3, 1, 2, 0, 2, 2, 1, 2 },
		{ 0, 3, 7, 3, 2, 4, 6, 4 },
	};

	for (unsigned mode = 0; mode < 2u; ++mode) {
		struct v92_phase4_modulator expected;
		struct v92_phase4_record_tx tx;
		unsigned bits = mode + 2u;
		int16_t sample = 1234;

		CHECK(v92_phase4_modulator_init(
			      &expected, mode ? V92_CONSTELLATION_8_POINT :
				      V92_CONSTELLATION_4_POINT, 1u, TEST_LU) ==
			      V92_PHASE4_MODULATOR_OK &&
		      v92_phase4_record_tx_init(
			      &tx, &expected, frame, sizeof(frame), 24u) ==
			      V92_PHASE4_RECORD_TX_OK,
		      "record bit-order fixture did not initialize");
		for (unsigned symbol = 0; symbol < 24u / bits; ++symbol) {
			int16_t reference;

			CHECK(v92_phase4_modulator_next(
				      &expected, groups[mode][symbol], &reference) ==
				      V92_PHASE4_MODULATOR_OK &&
			      v92_phase4_record_tx_next(&tx, &sample) ==
				      V92_PHASE4_RECORD_TX_OK && sample == reference &&
			      tx.modulator.scrambler.history == expected.scrambler.history &&
			      tx.modulator.previous_encoded_sign == expected.previous_encoded_sign &&
			      tx.bit_index == (symbol + 1u) * bits,
			      "record group order differed at width %u symbol %u", bits, symbol);
		}
		sample = 1234;
		CHECK(v92_phase4_record_tx_is_complete(&tx) &&
		      v92_phase4_record_tx_next(&tx, &sample) ==
			      V92_PHASE4_RECORD_TX_COMPLETE && sample == 1234,
		      "record emitted beyond its exact storage boundary");
	}
	return true;
}

static bool test_early_peer_sequence(void)
{
	struct v92_phase4_analog_tx tx;
	struct v92_suvd_parameters suvd = {
		.request_silence = true,
	};
	struct v92_phase4_record_tx handoff;
	int16_t sample = 1234;

	CHECK(initialize(&tx, V92_CONSTELLATION_4_POINT) &&
	      emit_symbols(&tx, 13u) &&
	      v92_phase4_analog_tx_note_suvd(&tx, &suvd) ==
		      V92_PHASE4_ANALOG_TX_OK &&
	      tx.control.cpu_pending && tx.control.peer_requests_silence &&
	      !v92_phase4_analog_tx_silence_requested(&tx) &&
	      emit_symbols(&tx, 11u) &&
	      tx.stage == V92_PHASE4_ANALOG_TX_TRN2U &&
	      tx.signal.trn2u.emitted_symbols == 24u,
	      "early SUVd did not preserve the aligned TRN2u boundary");
	CHECK(emit_symbols(&tx, 1u) &&
	      tx.stage == V92_PHASE4_ANALOG_TX_SUVU &&
	      tx.signal.record.bit_index == 2u &&
	      current_suvu_acknowledges_cpd(&tx, false) &&
	      finish_record(&tx) && tx.control.cpu_pending,
	      "early SUVd did not produce the first SUVu");
	CHECK(emit_symbols(&tx, 1u) &&
	      tx.stage == V92_PHASE4_ANALOG_TX_CPU &&
	      !tx.control.cpu_pending && current_cpu_acknowledges_cpd(&tx, false) &&
	      finish_record(&tx) && tx.control.cpu_sent &&
	      tx.control.completed_cpu_records == 1u &&
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
	      finish_record(&tx) && !tx.control.acknowledgment_sent &&
	      emit_symbols(&tx, 1u) &&
	      current_suvu_acknowledges_cpd(&tx, true) &&
	      finish_record(&tx) &&
	      v92_phase4_analog_tx_is_e2u_ready(&tx),
	      "mutual Phase 4 acknowledgment did not reach E2u");
	memset(&handoff, 0, sizeof(handoff));
	CHECK(v92_phase4_analog_tx_handoff(&tx, &handoff) ==
		      V92_PHASE4_ANALOG_TX_OK &&
	      v92_phase4_record_tx_is_complete(&handoff) &&
	      v92_phase4_analog_tx_next(&tx, &sample) ==
		      V92_PHASE4_ANALOG_TX_E2U_READY_RESULT &&
	      sample == 1234,
	      "E2u boundary did not preserve coding history");
	return true;
}

static bool test_training_boundaries(void)
{
	const unsigned peer_times[] = { 0u, 13u, 2399u, 12001u, UINT_MAX };
	for (unsigned mode = 0; mode < 2u; ++mode)
		for (unsigned points = 4u; points <= 8u; points += 4u)
			for (unsigned which = 0; which < ARRAY_LENGTH(peer_times); ++which) {
				struct v92_phase4_analog_tx tx;
				struct v92_pcm_phase4_trn2u expected;
				struct v92_cpu_parameters cpu = cpu_parameters();
				struct v92_suvu_parameters suvu = suvu_parameters();
				const struct v92_suvd_parameters suvd = { 0 };
				cpu.acknowledges_cpd = mode != 0u;
				enum v92_phase4_analog_tx_result result = mode ?
					v92_phase4_analog_tx_init_after_ru(&tx, points, TEST_LU, &suvu, &cpu) :
					v92_phase4_analog_tx_init(&tx, points, 1u, TEST_LU, &suvu, &cpu);
				CHECK(result == V92_PHASE4_ANALOG_TX_OK && !tx.control.cpu.acknowledges_cpd &&
				      !tx.control.received_cpd && !tx.control.peer_confirmation_received &&
				      v92_pcm_phase4_trn2u_init(&expected, points, mode ? 0u : 1u, TEST_LU) ==
					V92_PCM_PHASE4_TRN2U_OK, "training boundary did not initialize");
				unsigned end = mode ? 2400u : 12000u;
				if (peer_times[which] < end)
					end = peer_times[which] ? (peer_times[which] + 11u) / 12u * 12u : 12u;
				for (unsigned clock = 0; clock <= end; ++clock) {
					int16_t sample, reference;
					if (clock == peer_times[which])
						CHECK(v92_phase4_analog_tx_note_suvd(&tx, &suvd) == V92_PHASE4_ANALOG_TX_OK,
						      "early SUVd failed");
					CHECK(v92_phase4_analog_tx_next(&tx, &sample) == V92_PHASE4_ANALOG_TX_OK,
					      "training output failed");
					if (clock < end)
						CHECK(tx.stage == V92_PHASE4_ANALOG_TX_TRN2U &&
						      v92_pcm_phase4_trn2u_next(&expected, &reference) == V92_PCM_PHASE4_TRN2U_OK &&
						      sample == reference, "training timing/coding differed at %u", clock);
					else
						CHECK(tx.stage == V92_PHASE4_ANALOG_TX_SUVU &&
						      current_suvu_acknowledges_cpd(&tx, false), "SUVu did not follow aligned training");
				}
			}
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
	      finish_record(&tx) && tx.control.cpu_sent &&
	      tx.control.completed_cpu_records == 1u &&
	      emit_symbols(&tx, 1u) &&
	      tx.stage == V92_PHASE4_ANALOG_TX_SUVU,
	      "timeout path did not send its first CPu");
	CHECK(v92_phase4_analog_tx_expire_cpu_wait(&tx) ==
		      V92_PHASE4_ANALOG_TX_OK &&
	      v92_phase4_analog_tx_expire_cpu_wait(&tx) ==
		      V92_PHASE4_ANALOG_TX_INVALID_STATE &&
	      finish_record(&tx) && emit_symbols(&tx, 1u) &&
	      tx.stage == V92_PHASE4_ANALOG_TX_SUVU &&
	      tx.control.retry == V92_UPSTREAM_RETRY_ELIGIBLE &&
	      v92_phase4_analog_tx_note_suvd(&tx, &suvd) == V92_PHASE4_ANALOG_TX_OK &&
	      finish_record(&tx) && emit_symbols(&tx, 1u) &&
	      tx.stage == V92_PHASE4_ANALOG_TX_CPU &&
	      finish_record(&tx) && tx.control.cpu_sent &&
	      tx.control.completed_cpu_records == 2u &&
	      !v92_phase4_analog_tx_is_e2u_ready(&tx),
	      "CPu retry did not wait for the complete peer record");
	CHECK(emit_symbols(&tx, 1u) &&
	      tx.stage == V92_PHASE4_ANALOG_TX_CPU &&
	      v92_phase4_analog_tx_note_cpd(&tx, false) ==
		      V92_PHASE4_ANALOG_TX_OK &&
	      finish_record(&tx) && emit_symbols(&tx, 1u) &&
	      current_cpu_acknowledges_cpd(&tx, true) &&
	      finish_record(&tx) && tx.control.acknowledgment_sent &&
	      !v92_phase4_analog_tx_is_e2u_ready(&tx) &&
	      v92_phase4_analog_tx_note_ed(&tx) ==
		      V92_PHASE4_ANALOG_TX_OK &&
	      v92_phase4_analog_tx_is_e2u_ready(&tx),
	      "Ed did not complete an acknowledged status boundary");
	return true;
}

static bool test_pending_handoff(enum v92_constellation_size constellation,
	bool repeat_cpu)
{
	struct v92_phase4_analog_tx tx;
	struct v92_phase4_analog_tx saved;
	struct v92_phase4_record_tx handoff;
	struct v92_suvd_parameters suvd = { 0 };

	CHECK(initialize(&tx, constellation) &&
	      v92_phase4_analog_tx_note_suvd(&tx, &suvd) == V92_PHASE4_ANALOG_TX_OK &&
	      v92_phase4_analog_tx_note_cpd(&tx, false) == V92_PHASE4_ANALOG_TX_OK &&
	      emit_symbols(&tx, 25u) && finish_record(&tx) &&
	      emit_symbols(&tx, 1u) && finish_record(&tx) &&
	      tx.control.cpu_sent && tx.control.acknowledgment_sent && emit_symbols(&tx, 5u),
	      "acknowledged control fixture did not prepare");
	if (repeat_cpu)
		CHECK(v92_phase4_analog_tx_expire_cpu_wait(&tx) == V92_PHASE4_ANALOG_TX_OK &&
		      v92_phase4_analog_tx_note_suvd(&tx, &suvd) == V92_PHASE4_ANALOG_TX_OK &&
		      finish_record(&tx) && emit_symbols(&tx, 5u) &&
		      tx.stage == V92_PHASE4_ANALOG_TX_CPU,
		      "repeated CPu fixture did not prepare");
	CHECK(!v92_phase4_analog_tx_can_handoff(&tx) &&
	      v92_phase4_analog_tx_handoff(&tx, &handoff) ==
		      V92_PHASE4_ANALOG_TX_INVALID_STATE,
	      "control handed off without peer confirmation");
	suvd.acknowledges_cpu = true;
	CHECK((repeat_cpu ? v92_phase4_analog_tx_note_suvd(&tx, &suvd) :
			    v92_phase4_analog_tx_note_ed(&tx)) == V92_PHASE4_ANALOG_TX_OK &&
	      !v92_phase4_analog_tx_is_e2u_ready(&tx) &&
	      v92_phase4_analog_tx_can_handoff(&tx),
	      "early peer confirmation discarded the control tail");
	saved = tx;
	CHECK(v92_phase4_analog_tx_handoff(&tx, &handoff) == V92_PHASE4_ANALOG_TX_OK &&
	      memcmp(&tx, &saved, sizeof(tx)) == 0 &&
	      memcmp(&handoff, &tx.signal.record, sizeof(handoff)) == 0,
	      "handoff changed the pending record or its coding history");
	while (!v92_phase4_record_tx_is_complete(&handoff)) {
		int16_t expected, actual;

		CHECK(v92_phase4_record_tx_next(&handoff, &actual) == V92_PHASE4_RECORD_TX_OK &&
		      v92_phase4_analog_tx_next(&tx, &expected) == V92_PHASE4_ANALOG_TX_OK &&
		      actual == expected,
		      "handoff changed an unsent control symbol");
	}
	CHECK(v92_phase4_analog_tx_is_e2u_ready(&tx) &&
	      memcmp(&handoff, &tx.signal.record, sizeof(handoff)) == 0,
	      "pending control did not finish at the E2u boundary");
	return true;
}

static bool test_control_records(void)
{
	/* Identical policy at both training widths and every PCM data width.
	 * Neither record creation nor receipt of an ACK counts an unsent tail. */
	for (unsigned width = 24; width <= 72; width = width == 24 ? 36 : width + 2) {
		struct v92_upstream_control control;
		struct v92_suvu_parameters suvu = suvu_parameters(), decoded_suvu;
		struct v92_cpu_parameters cpu = cpu_parameters(), decoded_cpu;
		struct v92_suvd_parameters suvd = { 0 };
		uint8_t frame[V92_CPU_MAX_FRAME_BYTES];
		size_t bits;
		CHECK(v92_upstream_control_init(&control, &suvu, &cpu) &&
		      v92_upstream_control_note_suvd(&control, &suvd) &&
		      v92_upstream_control_start(&control, width, frame, sizeof(frame), &bits) &&
		      bits % width == 0 &&
		      v92_suvu_decode(frame, sizeof(frame), bits, width, &decoded_suvu) == V92_SUVU_OK &&
		      !decoded_suvu.acknowledges_cpd && !decoded_suvu.request_silence &&
		      v92_upstream_control_note_cpd(&control, false) &&
		      v92_upstream_control_complete(&control) && !control.acknowledgment_sent,
		      "width %u changed the initial SUVu or retroactively acknowledged CPd", width);
		CHECK(v92_upstream_control_start(&control, width, frame, sizeof(frame), &bits) &&
		      bits % width == 0 &&
		      v92_cpu_decode(frame, sizeof(frame), bits, width, &decoded_cpu) == V92_CPU_OK &&
		      decoded_cpu.acknowledges_cpd && decoded_cpu.data_rate_number == cpu.data_rate_number &&
		      !control.cpu_sent && !control.acknowledgment_sent &&
		      v92_upstream_control_complete(&control) && control.completed_cpu_records == 1 &&
		      control.acknowledgment_sent && !v92_upstream_control_can_finish(&control),
		      "width %u lost CPu alignment/completion", width);
		CHECK(v92_upstream_control_start(&control, width, frame, sizeof(frame), &bits) &&
		      v92_upstream_control_expire_cpu_wait(&control) &&
		      v92_upstream_control_note_suvd(&control, &suvd) &&
		      v92_upstream_control_complete(&control) &&
		      v92_upstream_control_start(&control, width, frame, sizeof(frame), &bits) &&
		      control.record == V92_UPSTREAM_CONTROL_CPU,
		      "width %u did not qualify a retry", width);
		suvd.acknowledges_cpu = true;
		CHECK(v92_upstream_control_note_suvd(&control, &suvd) &&
		      v92_upstream_control_can_finish(&control) && !control.record_complete &&
		      !v92_upstream_control_start(&control, width, frame, sizeof(frame), &bits) &&
		      v92_upstream_control_complete(&control) && control.completed_cpu_records == 2 &&
		      !v92_upstream_control_complete(&control) &&
		      !v92_upstream_control_start(&control, width, frame, sizeof(frame), &bits),
		      "width %u discarded the acknowledged CPu tail or repeated after completion", width);
	}
	return true;
}

static int16_t silence_wire_sample(enum g711_law law, bool inverted, int16_t sample)
{
	if (inverted)
		sample = (int16_t)-sample;
	return law == G711_LAW_PCMU ? g711_decode_pcmu(g711_encode_pcmu(sample)) :
		g711_decode_pcma(g711_encode_pcma(sample));
}

static bool run_silence_agreement(enum v92_constellation_size constellation,
	enum g711_law law, bool inverted, unsigned requests, unsigned arrival, unsigned confirmation)
{
	struct v92_phase4_analog_tx tx;
	struct v92_phase4_demodulator demodulator;
	struct v92_cpu_parameters cpu = cpu_parameters();
	struct v92_suvu_parameters suvu = suvu_parameters();
	struct v92_suvd_parameters suvd = { .request_silence = (requests & 2u) != 0u };
	unsigned width = v92_phase4_modulator_bits_per_symbol(constellation);
	unsigned records = 0u;
	bool confirmed = false;
	uint8_t frame[V92_SUVU_FRAME_BYTES] = { 0 };
	size_t bits = 0u;

	suvu.request_silence = (requests & 1u) != 0u;
	cpu.law = law;
	CHECK(v92_phase4_analog_tx_init_after_ru(&tx, constellation, TEST_LU, &suvu, &cpu) ==
		V92_PHASE4_ANALOG_TX_OK &&
	      v92_phase4_demodulator_init(&demodulator, constellation, inverted, TEST_LU) ==
		V92_PHASE4_MODULATOR_OK, "silence fixture did not initialize");
	for (unsigned clock = 0u; clock < 3000u && !v92_phase4_analog_tx_is_e2u_ready(&tx); ++clock) {
		bool first = clock == arrival;
		bool acknowledge = !confirmed && (confirmation == 0u ? first :
			records == (confirmation == 3u ? 2u : 1u) &&
			tx.stage == V92_PHASE4_ANALOG_TX_SUVU &&
			!tx.control.record_complete && tx.signal.record.bit_index >= 5u * width);
		if (first || acknowledge) {
			struct v92_phase4_analog_tx before = tx;
			suvd.acknowledges_cpu = acknowledge;
			CHECK((acknowledge && confirmation == 2u ? v92_phase4_analog_tx_note_ed(&tx) :
				v92_phase4_analog_tx_note_suvd(&tx, &suvd)) == V92_PHASE4_ANALOG_TX_OK &&
			      memcmp(&tx.signal, &before.signal, sizeof(tx.signal)) == 0,
			      "peer silence agreement rewrote an in-flight symbol/record");
			confirmed |= acknowledge;
		}
		int16_t sample, nearest;
		uint8_t group;
		CHECK(v92_phase4_analog_tx_next(&tx, &sample) == V92_PHASE4_ANALOG_TX_OK &&
		      v92_phase4_demodulator_next(&demodulator, silence_wire_sample(law, inverted, sample),
			&group, &nearest) ==
			V92_PHASE4_MODULATOR_OK, "silence transmitter/demodulator stopped at %u", clock);
		if (tx.stage == V92_PHASE4_ANALOG_TX_TRN2U) {
			CHECK(group == (1u << width) - 1u, "pre-silence TRN2u changed");
			continue;
		}
		CHECK(tx.control.record == V92_UPSTREAM_CONTROL_SUVU &&
		      !tx.control.cpu_pending && !tx.control.cpu_sent && !tx.control.received_cpd &&
		      tx.control.completed_cpu_records == 0u &&
		      (clock < arrival || v92_phase4_analog_tx_silence_requested(&tx)),
		      "silence agreement sent or invented a parameter exchange");
		if (tx.signal.record.bit_index == width) {
			memset(frame, 0, sizeof(frame));
			bits = 0u;
		}
		CHECK(bits + width <= sizeof(frame) * 8u, "silence status grew beyond SUVu");
		for (unsigned bit = 0; bit < width; ++bit, ++bits)
			frame[bits / 8u] |= (uint8_t)(((group >> bit) & 1u) << (7u - bits % 8u));
		if (tx.control.record_complete) {
			struct v92_suvu_parameters decoded;
			CHECK(v92_suvu_decode(frame, sizeof(frame), bits, width * 12u, &decoded) == V92_SUVU_OK &&
			      decoded.request_silence == suvu.request_silence &&
			      decoded.acknowledges_cpd == (records != 0u) &&
			      decoded.prefilter_level_db_q2_2 == suvu.prefilter_level_db_q2_2 &&
			      decoded.wait_for_cpu == suvu.wait_for_cpu,
			      "wire SUVu lost its request, acknowledgement, CRC or alignment");
			records++;
		}
		CHECK(!v92_phase4_analog_tx_can_handoff(&tx) || (confirmed && records >= 2u),
		      "silence agreement finished before both acknowledgements");
	}
	CHECK(v92_phase4_analog_tx_is_e2u_ready(&tx) && confirmed &&
	      records == (confirmation == 3u ? 3u : 2u) &&
	      memcmp(&tx.control.cpu, &cpu, sizeof(cpu)) == 0,
	      "silence agreement did not reach the complete SUVu tail");
	struct v92_phase4_record_tx tail, e2u;
	CHECK(v92_phase4_analog_tx_handoff(&tx, &tail) == V92_PHASE4_ANALOG_TX_OK &&
	      v92_phase4_record_tx_is_complete(&tail) &&
	      v92_phase4_record_tx_init_e2u(&e2u, &tail.modulator, false) == V92_PHASE4_RECORD_TX_OK,
	      "silence E2u could not inherit the completed control coder");
	for (unsigned symbol = 0; symbol < V92_PHASE4_E2U_SYMBOLS; ++symbol) {
		int16_t sample, nearest;
		uint8_t group;
		CHECK(v92_phase4_record_tx_next(&e2u, &sample) == V92_PHASE4_RECORD_TX_OK &&
		      v92_phase4_demodulator_next(&demodulator, silence_wire_sample(law, inverted, sample),
			&group, &nearest) ==
			V92_PHASE4_MODULATOR_OK && group == 0u,
		      "silence E2u lost continuous GPA or differential history");
	}
	CHECK(v92_phase4_record_tx_is_complete(&e2u), "silence E2u exceeded one frame");
	return true;
}

static bool test_silence_agreement(void)
{
	const unsigned arrivals[] = { 0u, 2399u, 2401u, 2423u };
	for (unsigned points = 4u; points <= 8u; points += 4u)
		for (enum g711_law law = G711_LAW_PCMU; law <= G711_LAW_PCMA; ++law)
			for (unsigned inverted = 0u; inverted < 2u; ++inverted)
				for (unsigned requests = 1u; requests <= 3u; ++requests)
					for (unsigned which = 0u; which < ARRAY_LENGTH(arrivals); ++which)
						for (unsigned confirmation = 0u; confirmation < 4u; ++confirmation)
							CHECK(run_silence_agreement(points, law, inverted, requests,
								arrivals[which], confirmation),
							      "silence points=%u law=%u inverted=%u requests=%u arrival=%u confirmation=%u",
							      points, law, inverted, requests, arrivals[which], confirmation);
	return true;
}

static bool test_silence_contracts(void)
{
	struct v92_phase4_analog_tx tx;
	struct v92_cpu_parameters cpu = cpu_parameters();
	struct v92_suvu_parameters suvu = suvu_parameters();
	const struct v92_suvd_parameters request = { .request_silence = true }, ordinary = { 0 };
	CHECK(v92_phase4_analog_tx_init_after_ru(&tx, V92_CONSTELLATION_4_POINT, TEST_LU,
		&suvu, &cpu) == V92_PHASE4_ANALOG_TX_OK, "silence contract fixture failed");
	struct v92_upstream_control fresh = tx.control, before;
	uint8_t frame[V92_CPU_MAX_FRAME_BYTES];
	size_t bits = 999u;
	memset(frame, 0xa5, sizeof(frame));
	CHECK(!v92_upstream_control_start(&tx.control, 38u, frame, sizeof(frame), &bits) &&
	      memcmp(&tx.control, &fresh, sizeof(fresh)) == 0 && bits == 999u,
	      "rate renegotiation accepted a data-mode control width");
	for (size_t index = 0; index < sizeof(frame); ++index)
		CHECK(frame[index] == 0xa5u, "rejected rate width changed the output");
	CHECK(v92_upstream_control_note_suvd(&tx.control, &request), "silence request failed");
	before = tx.control;
	CHECK(!v92_upstream_control_note_cpd(&tx.control, true) &&
	      !v92_upstream_control_expire_cpu_wait(&tx.control) &&
	      !v92_upstream_control_note_suvd(&tx.control, &ordinary) &&
	      memcmp(&tx.control, &before, sizeof(before)) == 0,
	      "silence agreement accepted CPd, a CPu retry, or a changed request");
	tx.control.received_cpd = true;
	CHECK(!v92_upstream_control_is_valid(&tx.control), "silence agreement validated a false CPd");
	tx.control = fresh;
	CHECK(v92_upstream_control_note_cpd(&tx.control, false), "early CPd was not accepted");
	before = tx.control;
	CHECK(!v92_upstream_control_note_suvd(&tx.control, &request) &&
	      memcmp(&tx.control, &before, sizeof(before)) == 0 &&
	      v92_upstream_control_is_valid(&tx.control), "late silence corrupted an accepted CPd");
	tx.control = fresh;
	CHECK(v92_upstream_control_note_suvd(&tx.control, &ordinary), "ordinary SUVd failed");
	before = tx.control;
	CHECK(!v92_upstream_control_note_suvd(&tx.control, &request) &&
	      memcmp(&tx.control, &before, sizeof(before)) == 0,
	      "ordinary parameter negotiation changed into a silence agreement");
	return true;
}

static bool test_control_contracts(void)
{
	struct v92_upstream_control control, saved;
	struct v92_suvu_parameters suvu = suvu_parameters();
	struct v92_cpu_parameters cpu = cpu_parameters();
	uint8_t frame[V92_CPU_MAX_FRAME_BYTES], before[sizeof(frame)];
	size_t bits = 999;
	CHECK(v92_upstream_control_init(&control, &suvu, &cpu), "control init failed");
	saved = control;
	memset(frame, 0xa5, sizeof(frame));
	memcpy(before, frame, sizeof(frame));
	CHECK(!v92_upstream_control_start(NULL, 36, frame, sizeof(frame), &bits) &&
	      !v92_upstream_control_start(&control, 37, frame, sizeof(frame), &bits) &&
	      !v92_upstream_control_start(&control, 36, frame, 1, &bits) &&
	      !v92_upstream_control_start(&control, 36, frame, sizeof(frame), (void *)frame) &&
	      !v92_upstream_control_start(&control, 36, (void *)&control, sizeof(control), &bits) &&
	      !v92_upstream_control_start(&control, 36, frame, sizeof(frame), (void *)&control) &&
	      !v92_upstream_control_complete(&control) &&
	      !v92_upstream_control_expire_cpu_wait(&control) &&
	      memcmp(&saved, &control, sizeof(saved)) == 0 && bits == 999 &&
	      memcmp(frame, before, sizeof(frame)) == 0,
	      "rejected control operation changed state/output");
	control.record = V92_UPSTREAM_CONTROL_CPU;
	control.record_complete = false;
	control.received_suvd = control.cpu_sent = true;
	control.completed_cpu_records = UINT64_MAX;
	saved = control;
	CHECK(v92_upstream_control_is_valid(&control) &&
	      !v92_upstream_control_complete(&control) && memcmp(&saved, &control, sizeof(saved)) == 0,
	      "record counter overflow changed state");
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
	struct v92_phase4_record_tx handoff;
	struct v92_phase4_record_tx handoff_before;
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
	CHECK(v92_phase4_analog_tx_init_after_ru(NULL, V92_CONSTELLATION_4_POINT, TEST_LU,
		&suvu, &cpu) == V92_PHASE4_ANALOG_TX_INVALID_ARGUMENT &&
	      v92_phase4_analog_tx_init_after_ru(&tx, V92_CONSTELLATION_4_POINT, TEST_LU,
		&suvu, NULL) == V92_PHASE4_ANALOG_TX_INVALID_ARGUMENT &&
	      v92_phase4_analog_tx_init_after_ru(&tx, V92_CONSTELLATION_4_POINT, TEST_LU,
		NULL, &cpu) == V92_PHASE4_ANALOG_TX_INVALID_ARGUMENT &&
	      v92_phase4_analog_tx_init_after_ru(&tx, V92_CONSTELLATION_4_POINT, INT16_MAX,
		&suvu, &cpu) == V92_PHASE4_ANALOG_TX_INVALID_PARAMETERS &&
	      memcmp(&tx, &before, sizeof(tx)) == 0, "invalid rate entry changed its destination");
	CHECK(v92_phase4_analog_tx_init(
		      &tx, V92_CONSTELLATION_4_POINT, 0u, INT16_MAX,
		      &suvu, &cpu) == V92_PHASE4_ANALOG_TX_INVALID_PARAMETERS &&
	      memcmp(&tx, &before, sizeof(tx)) == 0 &&
	      initialize(&tx, V92_CONSTELLATION_4_POINT),
	      "invalid modulation initialization was not transactional");
	CHECK(initialize(&ed_tx, V92_CONSTELLATION_4_POINT) &&
	      v92_phase4_analog_tx_note_ed(&ed_tx) ==
		      V92_PHASE4_ANALOG_TX_OK &&
	      ed_tx.control.peer_confirmation_received && !ed_tx.control.received_cpd,
	      "Ed was confused with receipt of CPd");
	CHECK(initialize(&cancel_tx, V92_CONSTELLATION_4_POINT) &&
	      v92_phase4_analog_tx_note_suvd(&cancel_tx, &suvd) ==
		      V92_PHASE4_ANALOG_TX_OK &&
	      emit_symbols(&cancel_tx, 25u) && finish_record(&cancel_tx) &&
	      emit_symbols(&cancel_tx, 1u) && finish_record(&cancel_tx) &&
	      emit_symbols(&cancel_tx, 1u) &&
	      v92_phase4_analog_tx_expire_cpu_wait(&cancel_tx) ==
		      V92_PHASE4_ANALOG_TX_OK &&
	      cancel_tx.control.retry == V92_UPSTREAM_RETRY_ELIGIBLE &&
	      v92_phase4_analog_tx_note_suvd(&cancel_tx, &suvd) == V92_PHASE4_ANALOG_TX_OK &&
	      cancel_tx.control.retry == V92_UPSTREAM_RETRY_ACTIVE &&
	      v92_phase4_analog_tx_note_ed(&cancel_tx) ==
		      V92_PHASE4_ANALOG_TX_OK &&
	      !cancel_tx.control.cpu_pending && cancel_tx.control.retry == V92_UPSTREAM_RETRY_IDLE,
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
	      v92_phase4_analog_tx_expire_cpu_wait(&tx) ==
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
		      &tx, (struct v92_phase4_record_tx *)(void *)&tx) ==
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
	      !v92_phase4_analog_tx_can_handoff(NULL) &&
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
	if (!test_record_bit_order() || !test_silence_agreement() || !test_silence_contracts() ||
	    !test_control_records() || !test_control_contracts() ||
	    !test_early_peer_sequence() || !test_training_boundaries() || !test_timeout_and_retry() ||
	    !test_pending_handoff(V92_CONSTELLATION_4_POINT, false) ||
	    !test_pending_handoff(V92_CONSTELLATION_4_POINT, true) ||
	    !test_pending_handoff(V92_CONSTELLATION_8_POINT, false) ||
	    !test_pending_handoff(V92_CONSTELLATION_8_POINT, true) ||
	    !test_contracts()) {
		fprintf(stderr, "[  FAILED  ] V.92 analogue Phase 4 control\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] V.92 analogue Phase 4 control\n");
	return 0;
}
