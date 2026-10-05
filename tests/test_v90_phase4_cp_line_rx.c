// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>
#include <v90_phase3_return_line_rx.h>
#include <v90_phase4_cp_line_rx.h>
#include <v90_phase4_cp_line_tx.h>

#include "test.h"
#include "v90_phase4_line_fixture.h"

#include <string.h>

enum {
	SAMPLE_LIMIT = 50000,
};

static int16_t quantize(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMA ?
		       g711_decode_pcma(g711_encode_pcma(sample)) :
		       g711_decode_pcmu(g711_encode_pcmu(sample));
}

static void add_ucode(
	struct pcm_constellation *constellation, uint8_t ucode)
{
	constellation->ucode_mask[ucode / 8u] |=
		(uint8_t)(1u << (ucode % 8u));
}

static struct v90_cp_parameters parameters(
	enum v90_cp_kind kind, enum g711_law law, uint8_t variant)
{
	struct v90_cp_parameters selected = {
		.kind = kind,
		.law = law,
		.upstream_rate_mask = (uint16_t)(0x1555u ^ variant),
		.trn1d_ratio_q3_13 = (uint16_t)(0x2000u + variant),
		.shaping_a1_q1_6 = (int8_t)(-16 - variant),
		.shaping_a2_q1_6 = (int8_t)(8 + variant),
		.shaping_b1_q1_6 = (int8_t)(-1 - variant),
		.shaping_b2_q1_6 = (int8_t)(31 - variant),
		.selected_rate_index = (uint8_t)(1u + variant),
		.sign_redundancy = (uint8_t)(variant % 4u),
		.spectral_lookahead = (uint8_t)((variant + 1u) % 4u),
		.constellation_count = 1,
		.request_silence = variant != 0,
		.acknowledges_peer = variant != 0,
	};

	add_ucode(&selected.transmit_constellations[0], variant);
	add_ucode(&selected.transmit_constellations[0], 127u - variant);
	return selected;
}

static bool parameters_match(
	const struct v90_cp_parameters *left,
	const struct v90_cp_parameters *right)
{
	uint8_t left_frame[V90_CP_MAX_FRAME_BYTES] = { 0 };
	uint8_t right_frame[V90_CP_MAX_FRAME_BYTES] = { 0 };
	size_t left_bits = 0;
	size_t right_bits = 0;

	return v90_cp_encode(
		       left, left_frame, sizeof(left_frame), &left_bits) ==
		       V90_CP_OK &&
	       v90_cp_encode(
		       right, right_frame, sizeof(right_frame), &right_bits) ==
		       V90_CP_OK &&
	       left_bits == right_bits &&
	       memcmp(left_frame, right_frame, sizeof(left_frame)) == 0;
}

static bool verify_line(
	enum g711_law law, enum v34_constellation_size constellation)
{
	struct v90_cp_parameters training =
		parameters(V90_CP_TRAINING, law, 0);
	struct v90_cp_parameters data = parameters(V90_CP_DATA, law, 1);
	struct v90_cp_parameters filler = parameters(V90_CP_DATA, law, 2);
	const struct v90_cp_parameters *expected[] = { &training, &data };
	struct v90_phase3_return_line_handoff handoff;
	struct v90_phase4_cp_line_tx tx;
	struct v90_phase4_cp_line_rx rx;
	unsigned int started_records = 1;
	unsigned int received_records = 0;
	int16_t frozen_gain = 0;

	CHECK(v90_phase4_line_fixture_init(
		      law, constellation, &training, &tx, &handoff),
	      "law/constellation %d/%d did not reach CPt", law,
	      constellation);
	/* A final drain may have stopped on a quiet observation. The new CP
	 * owner must release that policy without resetting its trained clock. */
	CHECK(v34_receive_timing_set_feedback_held(
		      &handoff.upstream.line.timing, true) == V34_RECEIVE_TIMING_OK,
	      "CP fixture could not retain a quiet training hold");
	CHECK(v90_phase4_cp_line_rx_init(
		      &rx, &handoff, constellation) ==
		      V90_PHASE4_CP_LINE_RX_OK &&
	      rx.line.timing.gain.mode == V34_GAIN_TRACKING,
	      "CP line receiver did not accept the delayed handoff");
	{
		struct v34_timing_control expected =
			handoff.upstream.line.timing.timing;

		expected.feedback_held = false;
		CHECK(handoff.upstream.line.timing.timing.feedback_held &&
		      !rx.line.timing.timing.feedback_held &&
		      memcmp(&rx.line.timing.timing, &expected,
			     sizeof(expected)) == 0,
		      "CP did not release only the old quiet timing policy");
	}
	for (uint32_t sample_index = 0;
	     sample_index < SAMPLE_LIMIT; ++sample_index) {
		struct v90_cp_receive_event event = { 0 };
		struct v90_phase4_cp_rx expected_symbols = rx.symbols;
		struct v34_fixed_training_decision decision;
		struct v34_fixed_training_decision saved_decision;
		bool boundary_was_complete = rx.boundary_complete;
		int16_t sample;
		enum v90_phase4_cp_line_rx_result result;

		if (v90_phase4_cp_line_tx_record_is_complete(&tx)) {
			const struct v90_cp_parameters *next =
				started_records == 1 ? &data : &filler;

			CHECK(v90_phase4_cp_line_tx_start_next_record(
				      &tx, next) == V90_PHASE4_CP_LINE_TX_OK,
			      "CP transmitter could not start record %u",
			      started_records);
			started_records++;
		}
		CHECK(v90_phase4_cp_line_tx_sample(&tx, &sample) ==
			      V90_PHASE4_CP_LINE_TX_OK,
		      "CP transmitter stopped before record %u",
		      received_records);
		memset(&decision, 0xa5, sizeof(decision));
		saved_decision = decision;
		result = v90_phase4_cp_line_rx_sample(
			&rx, quantize(law, sample), &event, &decision);
		if (boundary_was_complete &&
		    (result == V90_PHASE4_CP_LINE_RX_SYMBOL ||
		     result == V90_PHASE4_CP_LINE_RX_RECORD_READY)) {
			struct v90_cp_receive_event expected_event;

			CHECK(v90_phase4_cp_rx_receive_decision(&expected_symbols,
				      decision.quarter_point, decision.clockwise_quarter_turns,
				      &expected_event) == V90_CP_OK &&
			      memcmp(&expected_symbols, &rx.symbols, sizeof(expected_symbols)) == 0,
			      "exported CP decision did not match the consumed symbol");
		} else {
			CHECK(memcmp(&decision, &saved_decision, sizeof(decision)) == 0,
			      "a sample without a CP symbol changed the decision output");
		}
		if (result != V90_PHASE4_CP_LINE_RX_RECORD_READY) {
			CHECK(result == V90_PHASE4_CP_LINE_RX_OK ||
			      result == V90_PHASE4_CP_LINE_RX_SYMBOL,
			      "CP line receiver reported %s at sample %u",
			      v90_phase4_cp_line_rx_result_name(result),
			      sample_index);
			CHECK(rx.line.timing.gain.mode ==
				      (received_records == 0 ?
					       V34_GAIN_TRACKING : V34_GAIN_FROZEN) &&
			      (received_records == 0 ||
			       rx.line.timing.gain.gain == frozen_gain),
			      "CP gain did not freeze at the first verified record");
			continue;
		}
		CHECK(received_records < ARRAY_LENGTH(expected) &&
		      event.type == V90_CP_RECEIVE_FRAME &&
		      event.frame_result == V90_CP_OK &&
		      event.observed_bits ==
			      v90_cp_frame_bits(expected[received_records]) &&
		      parameters_match(
			      &event.parameters,
			      expected[received_records]) &&
		      v90_phase4_cp_line_rx_has_pending_record(&rx) &&
		      v90_phase4_cp_line_rx_pending_record_is_valid(&rx) &&
		      rx.line.timing.gain.mode == V34_GAIN_FROZEN &&
		      (received_records == 0 ||
		       rx.line.timing.gain.gain == frozen_gain),
		      "CP line record %u changed", received_records);
		frozen_gain = rx.line.timing.gain.gain;
		if (received_records == 0) {
			struct v34_gain_control settled = rx.line.timing.gain;

			CHECK(v34_gain_control_refresh_level(&settled) ==
				      V34_GAIN_CONTROL_OK &&
			      memcmp(&settled, &rx.line.timing.gain,
				     sizeof(settled)) == 0,
			      "CP froze an under-level gain at its first valid record");
		}
		{
			struct v90_phase4_cp_line_rx saved = rx;
			struct v90_cp_receive_event sentinel = {
				.type = V90_CP_RECEIVE_REJECTED,
				.observed_bits = 777,
			};
			struct v90_cp_receive_event saved_event = sentinel;

			CHECK(v90_phase4_cp_line_rx_sample(
				      &rx, 0, &sentinel, NULL) ==
				      V90_PHASE4_CP_LINE_RX_INVALID_STATE &&
			      memcmp(&rx, &saved, sizeof(rx)) == 0 &&
			      memcmp(&sentinel, &saved_event,
				     sizeof(sentinel)) == 0 &&
			      v90_phase4_cp_line_rx_continue(&rx) ==
				      V90_PHASE4_CP_LINE_RX_OK,
			      "CP record pause consumed another sample");
		}
		received_records++;
		if (received_records == ARRAY_LENGTH(expected))
			return !v90_phase4_cp_line_rx_has_failed(&rx);
	}
	CHECK(false, "CP line receiver did not complete two records");
	return false;
}

static bool test_continuous_g711_lines(void)
{
	CHECK(verify_line(G711_LAW_PCMU, V34_CONSTELLATION_4_POINT),
	      "PCMU four-point CP line failed");
	CHECK(verify_line(G711_LAW_PCMU, V34_CONSTELLATION_16_POINT),
	      "PCMU sixteen-point CP line failed");
	CHECK(verify_line(G711_LAW_PCMA, V34_CONSTELLATION_4_POINT),
	      "PCMA four-point CP line failed");
	CHECK(verify_line(G711_LAW_PCMA, V34_CONSTELLATION_16_POINT),
	      "PCMA sixteen-point CP line failed");
	return true;
}

static bool test_transactional_contracts(void)
{
	struct v90_cp_parameters training =
		parameters(V90_CP_TRAINING, G711_LAW_PCMU, 0);
	struct v90_phase3_return_line_handoff handoff;
	struct v90_phase4_cp_line_tx tx;
	struct v90_phase4_cp_line_rx rx;
	struct v90_phase4_cp_line_rx saved;
	struct v90_phase4_cp_line_rx untouched;
	struct v90_phase4_cp_line_rx untouched_saved;
	struct v90_cp_receive_event event = { 0 };
	struct v34_line_receiver expected_line;

	CHECK(v90_phase4_line_fixture_init(
		      G711_LAW_PCMU, V34_CONSTELLATION_4_POINT,
		      &training, &tx, &handoff),
	      "CP validation fixture did not reach CPt");
	expected_line = handoff.upstream.line;
	CHECK(v34_gain_control_set_mode(
		      &expected_line.timing.gain, V34_GAIN_TRACKING) ==
		      V34_GAIN_CONTROL_OK,
	      "CP input gain did not enter acquisition");
	handoff.marker.clockwise_quarter_turn_offset = 3;
	memset(&untouched, 0xa5, sizeof(untouched));
	untouched_saved = untouched;
	CHECK(v90_phase4_cp_line_rx_init(
		      NULL, &handoff, V34_CONSTELLATION_4_POINT) ==
		      V90_PHASE4_CP_LINE_RX_INVALID_ARGUMENT &&
	      v90_phase4_cp_line_rx_init(
		      &untouched, &handoff,
		      (enum v34_constellation_size)8) ==
		      V90_PHASE4_CP_LINE_RX_INVALID_ARGUMENT &&
	      memcmp(&untouched, &untouched_saved,
		     sizeof(untouched)) == 0 &&
	      v90_phase4_cp_line_rx_init(
		      &rx, &handoff, V34_CONSTELLATION_4_POINT) ==
		      V90_PHASE4_CP_LINE_RX_OK &&
	      v90_phase4_cp_line_rx_is_valid(&rx) &&
	      (rx.boundary_complete ||
	       (v34_phase3_s_receiver_is_valid(&rx.boundary) &&
		!v34_phase3_s_receiver_is_complete(&rx.boundary))) &&
	      memcmp(&rx.line, &expected_line,
		     sizeof(rx.line)) == 0 &&
	      rx.decoder_power_q15 ==
		      v90_upstream_training_rx_decoder_power(&handoff.upstream) &&
	      rx.adaptation == V34_ADAPTIVE_EQUALIZER_FULL,
	      "CP line initialization contract changed");
	saved = rx;
	CHECK(v90_phase4_cp_line_rx_sample(NULL, 0, &event, NULL) ==
		      V90_PHASE4_CP_LINE_RX_INVALID_ARGUMENT &&
	      v90_phase4_cp_line_rx_sample(&rx, 0, NULL, NULL) ==
		      V90_PHASE4_CP_LINE_RX_INVALID_ARGUMENT &&
	      v90_phase4_cp_line_rx_sample(&rx, 0, &event,
		      (struct v34_fixed_training_decision *)&rx) ==
		      V90_PHASE4_CP_LINE_RX_INVALID_ARGUMENT &&
	      v90_phase4_cp_line_rx_sample(&rx, 0, &event,
		      (struct v34_fixed_training_decision *)&event) ==
		      V90_PHASE4_CP_LINE_RX_INVALID_ARGUMENT &&
	      v90_phase4_cp_line_rx_continue(&rx) ==
		      V90_PHASE4_CP_LINE_RX_INVALID_STATE &&
	      memcmp(&rx, &saved, sizeof(rx)) == 0 &&
	      !v90_phase4_cp_line_rx_has_failed(&rx) &&
	      !v90_phase4_cp_line_rx_has_pending_record(&rx) &&
	      !v90_phase4_cp_line_rx_pending_record_is_valid(&rx),
	      "rejected CP line operation changed state");
	rx.symbols.previous_clockwise_quarter_turns = 4;
	CHECK(!v90_phase4_cp_line_rx_is_valid(&rx) &&
	      strcmp(v90_phase4_cp_line_rx_result_name(
		     V90_PHASE4_CP_LINE_RX_RECORD_READY),
		     "record ready") == 0 &&
	      strcmp(v90_phase4_cp_line_rx_result_name(
		     V90_PHASE4_CP_LINE_RX_NUMERIC_FAILURE),
		     "numeric failure") == 0 &&
	      strcmp(v90_phase4_cp_line_rx_result_name(
		     (enum v90_phase4_cp_line_rx_result)99),
		     "unknown") == 0,
	      "CP line validation or result names changed");
	return true;
}

static bool test_fixed_handoff_contract(void)
{
	struct v90_cp_parameters training =
		parameters(V90_CP_TRAINING, G711_LAW_PCMU, 0);
	struct v90_phase3_return_line_handoff handoff;
	struct v90_phase3_return_line_handoff invalid;
	struct v90_phase4_cp_line_tx tx;
	struct v90_phase4_cp_line_rx rx;
	struct v90_phase4_cp_line_rx saved;
	struct v34_adaptive_equalizer_observation observation = { 0 };
	bool observed = false;
	union {
		struct v90_phase3_return_line_handoff handoff;
		struct v90_phase4_cp_line_rx receiver;
	} aliased;

	CHECK(v90_phase4_line_fixture_init(G711_LAW_PCMU,
		      V34_CONSTELLATION_4_POINT, &training, &tx, &handoff),
	      "fixed handoff fixture did not reach CPt");
	memset(&rx, 0xa5, sizeof(rx));
	saved = rx;
	for (unsigned int fault = 0; fault < 8u; ++fault) {
		invalid = handoff;
		switch (fault) {
		case 0: invalid.upstream.line.initialized = false; break;
		case 1: invalid.upstream.failed = true; break;
		case 2: invalid.marker.clockwise_quarter_turn_offset = 4; break;
		case 3: invalid.precoder.coefficients[0].real = INT16_MIN; break;
		case 4:
			invalid.upstream.recent_decision_count =
				V90_UPSTREAM_TRAINING_RX_HISTORY_SYMBOLS + 1u;
			break;
		case 5:
			invalid.upstream.transmitting_profile.local_role =
				V34_CALLING_STATION;
			break;
		case 6: ++invalid.upstream.transmitting_profile.md_symbol_count; break;
		case 7: invalid.upstream.line.timing.s_detector.detected = false; break;
		}
		CHECK(!v90_phase3_return_line_handoff_is_valid(&invalid) &&
		      v90_phase4_cp_line_rx_init(&rx, &invalid,
			      V34_CONSTELLATION_4_POINT) ==
			      V90_PHASE4_CP_LINE_RX_INVALID_ARGUMENT &&
		      memcmp(&rx, &saved, sizeof(rx)) == 0,
		      "fixed handoff fault %u changed the CP receiver", fault);
	}
	aliased.handoff = handoff;
	CHECK(v90_phase4_cp_line_rx_init(&aliased.receiver, &aliased.handoff,
		      V34_CONSTELLATION_4_POINT) ==
		      V90_PHASE4_CP_LINE_RX_INVALID_ARGUMENT &&
	      memcmp(&aliased.handoff, &handoff, sizeof(handoff)) == 0,
	      "CP handoff overwrote its aliased input");

	/* Produce a real unresolved observation, then retain it. Both are valid
	 * equalizer states, but neither transfers ownership of the next sample. */
	invalid = handoff;
	for (unsigned int index = 0; index < 8u && !observed; ++index) {
		int16_t sample;
		enum v34_line_receiver_result result;

		CHECK(v90_phase4_cp_line_tx_sample(&tx, &sample) ==
			      V90_PHASE4_CP_LINE_TX_OK,
		      "fixed handoff could not produce a CP sample");
		result = v34_line_receiver_sample(&invalid.upstream.line,
			quantize(G711_LAW_PCMU, sample), &observation);
		CHECK(result == V34_LINE_RECEIVER_OK ||
		      result == V34_LINE_RECEIVER_OBSERVATION_READY,
		      "fixed handoff could not produce a pending observation");
		observed = result == V34_LINE_RECEIVER_OBSERVATION_READY;
	}
	CHECK(observed && invalid.upstream.line.equalizer.decision_pending &&
	      v90_upstream_training_rx_is_tracking(&invalid.upstream) &&
	      !v90_phase3_return_line_handoff_is_valid(&invalid) &&
	      v90_phase4_cp_line_rx_init(&rx, &invalid, V34_CONSTELLATION_4_POINT) ==
		      V90_PHASE4_CP_LINE_RX_INVALID_ARGUMENT &&
	      memcmp(&rx, &saved, sizeof(rx)) == 0,
	      "CP accepted an unresolved fixed observation");
	CHECK(v34_adaptive_equalizer_retain_observation(
		      &invalid.upstream.line.equalizer, &observation) ==
		      V34_ADAPTIVE_EQUALIZER_OK &&
	      invalid.upstream.line.equalizer.retained.pending &&
	      !invalid.upstream.line.equalizer.decision_pending &&
	      v90_upstream_training_rx_is_tracking(&invalid.upstream) &&
	      !v90_phase3_return_line_handoff_is_valid(&invalid) &&
	      v90_phase4_cp_line_rx_init(&rx, &invalid, V34_CONSTELLATION_4_POINT) ==
		      V90_PHASE4_CP_LINE_RX_INVALID_ARGUMENT &&
	      memcmp(&rx, &saved, sizeof(rx)) == 0,
	      "CP accepted a retained fixed observation");
	return true;
}

/* Build marker decisions independently of the line receiver. The second
 * marker may be newer but incomplete or beyond the detector's error bound. */
static bool put_history_marker(
	struct v34_analytic_sample *history, size_t first, size_t count,
	uint8_t rotation, uint8_t damage)
{
	const size_t s = V34_PHASE3_S_RECEIVER_MIN_S_SUFFIX_SYMBOLS;

	for (size_t index = 0; index < s + V34_PHASE3_S_BAR_SYMBOLS &&
	     first + index < count; ++index) {
		uint8_t turn = (rotation + (index < s ? 0u : 2u) +
			       4u - (index & 1u)) & 3u;

		if (index >= s && index < s + damage)
			turn = (turn + 1u) & 3u;
		CHECK(v34_training_constellation_fixed_point(
			      V34_CONSTELLATION_4_POINT, 0, turn,
			      &history[first + index]) == V34_CONSTELLATION_OK,
		      "history marker point was invalid");
	}
	return true;
}

static bool test_latest_history_boundary(void)
{
	static const size_t counts[] = { 128u, 256u };
	static const size_t cursors[] = { 0u, 17u, 255u };

	for (unsigned int law = 0; law < 2u; ++law) {
		for (unsigned int size = 0; size < 2u; ++size) {
			enum v34_constellation_size constellation = size == 0u ?
				V34_CONSTELLATION_4_POINT : V34_CONSTELLATION_16_POINT;
			struct v90_cp_parameters training =
				parameters(V90_CP_TRAINING, law, 0);
			struct v90_phase3_return_line_handoff base;
			struct v90_phase4_cp_line_tx tx;

			CHECK(v90_phase4_line_fixture_init(
				      law, constellation, &training, &tx, &base),
			      "history fixture did not reach CPt");
			for (size_t ci = 0; ci < ARRAY_LENGTH(counts); ++ci) {
				for (size_t wrap = 0; wrap < ARRAY_LENGTH(cursors); ++wrap) {
					for (unsigned int mode = 0; mode < 5u; ++mode) {
						struct v90_phase3_return_line_handoff handoff = base;
						struct v90_phase3_return_line_handoff saved;
						struct v34_analytic_sample history[256];
						struct v90_phase4_cp_line_rx rx;
						struct v90_phase4_cp_rx expected;
						size_t count = counts[ci];
						size_t cursor = count < 256u ? count : cursors[wrap];
						size_t boundary = mode == 1u ? count : mode == 4u ? count - 4u : 48u;
						uint8_t rotation = mode == 1u || mode == 4u ? 3u : 0u;

						for (size_t index = 0; index < count; ++index)
							CHECK(v34_training_constellation_fixed_point(
								      V34_CONSTELLATION_4_POINT, 0, 2,
								      &history[index]) == V34_CONSTELLATION_OK,
							      "history filler point was invalid");
						CHECK(put_history_marker(history, 0, count, 0, 0),
						      "older history marker failed");
						if (mode != 0u)
							CHECK(put_history_marker(history,
								      count - (mode == 2u ? 40u : mode == 4u ? 52u : 48u), count,
								      3, mode == 3u ? 6u : mode == 4u ? 4u : 0u),
							      "newer history marker failed");
						handoff.upstream.recent_decision_count = count;
						handoff.upstream.next_recent_decision = cursor;
						for (size_t index = 0; index < count; ++index)
							handoff.upstream.recent_decisions[
								(cursor + 256u - count + index) % 256u] = history[index];
						saved = handoff;
						CHECK(v90_phase4_cp_rx_init(&expected,
							      handoff.upstream.transmitting_profile.local_role,
							      constellation) == V90_CP_OK,
						      "expected CP symbol receiver failed");
						expected.previous_clockwise_quarter_turns = rotation;
						for (size_t index = boundary; index < count; ++index) {
							struct v34_fixed_training_decision hard;
							struct v90_cp_receive_event event;

							CHECK(v34_training_constellation_fixed_slice(
								      constellation, history[index], &hard) == V34_CONSTELLATION_OK &&
							      v90_phase4_cp_rx_receive_decision(&expected,
								      hard.quarter_point, hard.clockwise_quarter_turns,
								      &event) == V90_CP_OK &&
							      event.type == V90_CP_RECEIVE_NO_EVENT,
							      "expected historical CP replay failed");
						}
						CHECK(v90_phase4_cp_line_rx_init(&rx, &handoff, constellation) ==
							      V90_PHASE4_CP_LINE_RX_OK && rx.boundary_complete &&
							      memcmp(&rx.symbols, &expected, sizeof(expected)) == 0 &&
							      memcmp(&handoff, &saved, sizeof(saved)) == 0,
						      "wrong history boundary law=%u size=%u mode=%u count=%zu cursor=%zu",
						      law, size, mode, count, cursor);
					}
				}
			}
		}
	}
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "continuous G.711 lines", test_continuous_g711_lines },
		{ "transactional contracts", test_transactional_contracts },
		{ "fixed handoff contract", test_fixed_handoff_contract },
		{ "latest history boundary", test_latest_history_boundary },
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
