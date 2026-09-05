// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>
#include <v90_phase3_analog_line_rx.h>
#include <v90_phase3_analog_line_tx.h>

#include "test.h"

#include <math.h>
#include <string.h>

enum {
	TRN_SYMBOLS = V34_TRAINING_SEQUENCE_MIN_SYMBOLS + 173,
	SAMPLE_LIMIT = 40000,
	NOMINAL_AMPLITUDE = 6000,
	TEST_INITIAL_RECEIVE_GAIN = 0x0600,
};

static struct v34_phase3_profile profile(void)
{
	return (struct v34_phase3_profile){
		.symbol_rate_numerator = 3000,
		.symbol_rate_denominator = 1,
		.carrier_frequency_numerator = 2000,
		.carrier_frequency_denominator = 1,
		.symbol_rate = V34_SYMBOL_RATE_3000,
		.local_role = V34_ANSWERING_STATION,
		.preemphasis_filter_index = 0,
		.projected_data_rate_2400_bps_units = 8,
		.high_carrier = true,
	};
}

static int16_t quantize(enum g711_law law, int16_t sample)
{
	return law == G711_LAW_PCMA ?
		       g711_decode_pcma(g711_encode_pcma(sample)) :
		       g711_decode_pcmu(g711_encode_pcmu(sample));
}

static bool run_line(enum g711_law law)
{
	struct v34_phase3_profile selected = profile();
	struct v34_training_line pre_s_line;
	struct v90_phase3_analog_line_tx tx;
	struct v90_phase3_analog_line_rx rx;
	struct v90_phase3_analog_line_handoff handoff;
	unsigned int timing_events = 0;
	unsigned int s_events = 0;
	unsigned int pp_events = 0;
	uint32_t pre_s_symbols = 0;
	bool started_ja = false;
	bool detected = false;

	CHECK(v90_phase3_analog_line_tx_init(
		      &tx, &selected, NOMINAL_AMPLITUDE) ==
		      V90_PHASE3_ANALOG_LINE_TX_OK &&
	      v34_training_line_init(
		      &pre_s_line, &selected, V90_PHASE3_SAMPLE_RATE,
		      NOMINAL_AMPLITUDE) == V34_TRAINING_LINE_OK &&
	      v90_phase3_analog_line_rx_init(
		      &rx, &selected, TEST_INITIAL_RECEIVE_GAIN) ==
		      V90_PHASE3_ANALOG_LINE_RX_OK,
	      "law %d analogue line pair did not initialize", law);
	/* Timing may lock on the tail of Phase 2. A shaped QAM sequence with
	 * repeated points is not the alternating S prefix, though, and must not
	 * complete S/PP or seed Ja/decision history. */
	for (uint32_t index = 0; index < 1600u; ++index) {
		static const uint8_t rotations[4] = { 0, 0, 1, 1 };
		struct qam_point point;
		const struct qam_point *offered = NULL;
		int16_t sample;
		bool accepting = v34_training_line_needs_point(&pre_s_line);
		bool accepted;
		enum v90_phase3_analog_line_rx_result result =
			V90_PHASE3_ANALOG_LINE_RX_OK;

		if (accepting) {
			point = v34_rotated_point_zero(
				rotations[pre_s_symbols & 3u]);
			offered = &point;
		}
		CHECK(v34_training_line_sample(
			      &pre_s_line, offered, &sample, &accepted) ==
			      V34_TRAINING_LINE_OK &&
		      accepted == accepting,
		      "law %d pre-S source stopped at sample %u", law, index);
		if (accepted)
			pre_s_symbols++;
		result = v90_phase3_analog_line_rx_sample(
			&rx, quantize(law, sample));

		CHECK(result == V90_PHASE3_ANALOG_LINE_RX_OK ||
			      result == V90_PHASE3_ANALOG_LINE_RX_TIMING_ACQUIRED,
		      "law %d pre-S QAM returned %s at sample %u",
		      law, v90_phase3_analog_line_rx_result_name(result),
		      index);
		if (result == V90_PHASE3_ANALOG_LINE_RX_TIMING_ACQUIRED)
			timing_events++;
	}
	CHECK(timing_events == 1u &&
	      v34_receive_timing_is_acquired(&rx.upstream.line.timing) &&
	      rx.upstream.initial.stage == V34_PHASE3_DECISION_RECEIVER_S &&
	      rx.ja.precoder.observed_symbols == 0u &&
	      rx.upstream.recent_decision_count == 0u,
	      "law %d pre-S state timing=%u acquired=%u stage=%u Ja=%u history=%u",
	      law, timing_events,
	      v34_receive_timing_is_acquired(&rx.upstream.line.timing),
	      rx.upstream.initial.stage, rx.ja.precoder.observed_symbols,
	      rx.upstream.recent_decision_count);
	/* The two independently initialized transmitters do not share a carrier
	 * phase timeline. Reset after the rejection check; retained call replay
	 * covers continuous recovery across the real Phase 2/Phase 3 boundary. */
	CHECK(v90_phase3_analog_line_rx_init(
		      &rx, &selected, TEST_INITIAL_RECEIVE_GAIN) ==
		      V90_PHASE3_ANALOG_LINE_RX_OK,
	      "law %d receiver did not reset after the pre-S check", law);
	timing_events = 0;
	for (uint32_t index = 0; index < SAMPLE_LIMIT; ++index) {
		int16_t sample;
		enum v90_phase3_analog_line_rx_result result;

		if (!started_ja &&
		    tx.symbols.stage == V90_PHASE3_ANALOG_TX_PREFIX &&
		    tx.symbols.sequence.prefix.stage ==
			    V34_PHASE3_TRANSMITTER_TRN &&
		    tx.symbols.sequence.prefix.trn_symbols == TRN_SYMBOLS) {
			CHECK(v90_phase3_analog_line_tx_start_ja(&tx) ==
				      V90_PHASE3_ANALOG_LINE_TX_OK,
			      "law %d could not start Ja", law);
			started_ja = true;
		}
		CHECK(v90_phase3_analog_line_tx_sample(&tx, &sample) ==
			      V90_PHASE3_ANALOG_LINE_TX_OK,
		      "law %d analogue source stopped at sample %u", law, index);
		result = v90_phase3_analog_line_rx_sample(
			&rx, quantize(law, sample));
		switch (result) {
		case V90_PHASE3_ANALOG_LINE_RX_OK:
		case V90_PHASE3_ANALOG_LINE_RX_JA_REJECTED:
			break;
		case V90_PHASE3_ANALOG_LINE_RX_TIMING_ACQUIRED:
			timing_events++;
			break;
		case V90_PHASE3_ANALOG_LINE_RX_S_DETECTED:
			s_events++;
			break;
		case V90_PHASE3_ANALOG_LINE_RX_PP_COMPLETE:
			pp_events++;
			break;
		case V90_PHASE3_ANALOG_LINE_RX_JA_DETECTED:
			detected = true;
			break;
		default:
			CHECK(false, "law %d receiver returned %s at sample %u",
			      law, v90_phase3_analog_line_rx_result_name(result),
			      index);
		}
		if (detected)
			break;
	}
	CHECK(started_ja && detected && timing_events == 1 &&
	      s_events == 1 && pp_events == 1 &&
	      !v90_phase3_analog_line_rx_has_failed(&rx) &&
	      v90_phase3_analog_line_rx_handoff(&rx, &handoff) ==
		      V90_PHASE3_ANALOG_LINE_RX_OK &&
	      v90_phase3_analog_line_handoff_is_valid(&handoff),
	      "law %d did not recover the line-level Ja boundary "
	      "(started %u detected %u timing %u S %u PP %u stage %u TRN %u)",
	      law, started_ja, detected, timing_events, s_events, pp_events,
	      rx.upstream.initial.stage, rx.upstream.initial.j.received_symbols);
	{
		struct v90_phase3_analog_line_rx saved = rx;

		CHECK(v90_phase3_analog_line_rx_sample(&rx, 0) ==
			      V90_PHASE3_ANALOG_LINE_RX_COMPLETE &&
		      memcmp(&rx, &saved, sizeof(rx)) == 0,
		      "law %d complete line receiver consumed a sample", law);
	}
	return true;
}

static bool test_g711_lines(void)
{
	CHECK(run_line(G711_LAW_PCMU) && run_line(G711_LAW_PCMA),
	      "G.711 analogue line timelines failed");
	return true;
}

static bool check_fixed_sample_rejection(
	const struct v90_upstream_training_rx *trained)
{
	struct v90_upstream_training_rx rx = *trained;
	struct v90_upstream_training_rx saved = rx;
	struct v34_analytic_sample decision = { INT16_MIN, INT16_MAX };
	struct v34_analytic_sample saved_decision = decision;
	struct v34_adaptive_equalizer_observation observation = { 0 };
	bool pending = false;

	CHECK(v90_upstream_training_rx_sample(NULL, 0, &decision) ==
		      V90_UPSTREAM_TRAINING_RX_INVALID_ARGUMENT &&
	      v90_upstream_training_rx_sample(&rx, 0, NULL) ==
		      V90_UPSTREAM_TRAINING_RX_INVALID_ARGUMENT &&
	      v90_upstream_training_rx_sample(
		      &rx, 0, &rx.recent_decisions[0]) ==
		      V90_UPSTREAM_TRAINING_RX_INVALID_ARGUMENT &&
	      memcmp(&rx, &saved, sizeof(rx)) == 0 &&
	      memcmp(&decision, &saved_decision, sizeof(decision)) == 0,
	      "invalid or aliased fixed sample changed owner or output");
	rx.line.initialized = false;
	saved = rx;
	CHECK(v90_upstream_training_rx_sample(&rx, 0, &decision) ==
		      V90_UPSTREAM_TRAINING_RX_INVALID_ARGUMENT &&
	      memcmp(&rx, &saved, sizeof(rx)) == 0 &&
	      memcmp(&decision, &saved_decision, sizeof(decision)) == 0,
	      "invalid fixed receiver changed owner or output");
	rx = *trained;
	rx.failed = true;
	saved = rx;
	CHECK(v90_upstream_training_rx_sample(&rx, 0, &decision) ==
		      V90_UPSTREAM_TRAINING_RX_FAILED &&
	      memcmp(&rx, &saved, sizeof(rx)) == 0 &&
	      memcmp(&decision, &saved_decision, sizeof(decision)) == 0,
	      "failed fixed receiver published or consumed another decision");

	/* Leave a real physical observation unresolved. The existing numeric
	 * failure latches on the next sample; it must not publish that decision. */
	rx = *trained;
	for (unsigned int index = 0; index < 8u && !pending; ++index) {
		enum v34_line_receiver_result result =
			v34_line_receiver_sample(&rx.line, 0, &observation);

		CHECK(result == V34_LINE_RECEIVER_OK ||
		      result == V34_LINE_RECEIVER_OBSERVATION_READY,
		      "could not leave a fixed observation pending");
		pending = result == V34_LINE_RECEIVER_OBSERVATION_READY;
	}
	CHECK(pending && v90_upstream_training_rx_is_tracking(&rx),
	      "unresolved fixed observation did not remain valid");
	saved = rx;
	saved.failed = true;
	CHECK(v90_upstream_training_rx_sample(&rx, 0, &decision) ==
		      V90_UPSTREAM_TRAINING_RX_NUMERIC_FAILURE &&
	      memcmp(&rx, &saved, sizeof(rx)) == 0 &&
	      memcmp(&decision, &saved_decision, sizeof(decision)) == 0,
	      "numeric fixed failure changed its latch or published a decision");
	return true;
}

static bool run_fixed_ja_line(enum g711_law law)
{
	struct v34_phase3_profile selected = profile();
	struct v90_phase3_analog_line_tx tx;
	struct v90_upstream_training_rx line;
	struct v90_phase3_ja_rx semantic;
	struct v90_phase3_ja_handoff handoff;
	struct v90_dil_parameters expected;
	struct v34_analytic_sample fixed_target;
	struct qam_point unit_target = v34_rotated_point_zero(0);
	uint8_t expected_bits[V90_DIL_MAX_DESCRIPTOR_BYTES];
	uint8_t received_bits[V90_DIL_MAX_DESCRIPTOR_BYTES];
	size_t expected_count;
	size_t received_count;
	double decision_scale;
	unsigned int decisions = 0;
	unsigned int pp_events = 0;
	bool started_ja = false;
	bool detected = false;

	CHECK(v90_phase3_analog_line_tx_init(
		      &tx, &selected, NOMINAL_AMPLITUDE) ==
		      V90_PHASE3_ANALOG_LINE_TX_OK &&
	      v90_upstream_training_rx_init(
		      &line, &selected, TEST_INITIAL_RECEIVE_GAIN) ==
		      V90_UPSTREAM_TRAINING_RX_OK &&
	      v90_phase3_ja_rx_init(&semantic) == V90_PHASE3_JA_RX_OK &&
	      v90_dil_profile_init(&expected, V90_DIL_PROFILE_CONVENTIONAL) ==
		      V90_DIL_OK &&
	      v34_training_constellation_fixed_point(
		      V34_CONSTELLATION_4_POINT, 0, 0, &fixed_target) ==
		      V34_CONSTELLATION_OK,
	      "law %d fixed Ja receive path did not initialize", law);
	/* Use the matching generators' actual four-point scale, not the rounded
	 * nominal receive radius. This is also the precoder learner's scale. */
	decision_scale = fixed_target.in_phase / unit_target.in_phase;
	/* Physical acquisition belongs solely to the fixed receiver. Observe each
	 * new post-PP decision once; its final PP decision is not a TRN symbol. */
	for (uint32_t index = 0; index < SAMPLE_LIMIT; ++index) {
		struct v34_analytic_sample decision = { INT16_MIN, INT16_MAX };
		struct v34_analytic_sample saved_decision = decision;
		struct v34_analytic_sample retained;
		struct qam_point normalized;
		struct qam_point target;
		uint16_t previous = line.next_recent_decision;
		uint16_t previous_trn = line.initial.j.received_symbols;
		bool was_tracking = v90_upstream_training_rx_is_tracking(&line);
		int16_t sample;
		enum v90_upstream_training_rx_result sampled;
		enum v90_phase3_ja_rx_result result;

		if (!started_ja &&
		    tx.symbols.stage == V90_PHASE3_ANALOG_TX_PREFIX &&
		    tx.symbols.sequence.prefix.stage ==
			    V34_PHASE3_TRANSMITTER_TRN &&
		    tx.symbols.sequence.prefix.trn_symbols == TRN_SYMBOLS) {
			CHECK(v90_phase3_analog_line_tx_start_dil_ja(
				      &tx, &expected) == V90_PHASE3_ANALOG_LINE_TX_OK,
			      "law %d fixed path could not start Ja", law);
			started_ja = true;
		}
		CHECK(v90_phase3_analog_line_tx_sample(&tx, &sample) ==
			      V90_PHASE3_ANALOG_LINE_TX_OK,
		      "law %d fixed source stopped at sample %u", law, index);
		sampled = v90_upstream_training_rx_sample(
			&line, quantize(law, sample), &decision);
		CHECK(sampled == V90_UPSTREAM_TRAINING_RX_OK ||
		      sampled == V90_UPSTREAM_TRAINING_RX_DECISION_READY,
		      "law %d fixed path stopped at sample %u", law, index);
		if (!was_tracking && v90_upstream_training_rx_is_tracking(&line)) {
			pp_events++;
			CHECK(sampled == V90_UPSTREAM_TRAINING_RX_OK &&
			      line.initial.j.received_symbols == 0 && decisions == 0,
			      "law %d final PP observation was published as TRN", law);
		}
		if (sampled == V90_UPSTREAM_TRAINING_RX_OK) {
			CHECK(previous == line.next_recent_decision &&
			      memcmp(&decision, &saved_decision, sizeof(decision)) == 0,
			      "law %d nonready sample published a decision", law);
			continue;
		}
		decisions++;
		CHECK(was_tracking && v90_upstream_training_rx_is_tracking(&line) &&
		      line.next_recent_decision == (previous + 1u) %
			      V90_UPSTREAM_TRAINING_RX_HISTORY_SYMBOLS &&
		      v90_upstream_training_rx_recent_decision(
			      &line, line.recent_decision_count - 1u, &retained) &&
		      memcmp(&decision, &retained, sizeof(decision)) == 0,
		      "law %d fixed event and retained decision differ", law);
		CHECK(line.initial.j.received_symbols ==
			      (decisions < V34_TRAINING_SEQUENCE_MIN_SYMBOLS ?
			       decisions : V34_TRAINING_SEQUENCE_MIN_SYMBOLS) &&
		      (decisions > V34_TRAINING_SEQUENCE_MIN_SYMBOLS ||
		       previous_trn + 1u == decisions),
		      "law %d fixed decision %u displaced known TRN", law, decisions);
		normalized = (struct qam_point){
			decision.in_phase / decision_scale,
			decision.quadrature / decision_scale,
		};
		result = v90_phase3_ja_rx_observe(&semantic, &normalized, &target);
		CHECK(result == V90_PHASE3_JA_RX_OK ||
		      result == V90_PHASE3_JA_RX_REJECTED ||
		      result == V90_PHASE3_JA_RX_DETECTED,
		      "law %d fixed Ja returned %d at sample %u", law, result, index);
		if (semantic.precoder.observed_symbols <=
		    V34_TRAINING_SEQUENCE_MIN_SYMBOLS)
			CHECK(line.initial.j.received_symbols ==
				      semantic.precoder.observed_symbols,
			      "law %d fixed and semantic TRN boundaries differ", law);
		if (result == V90_PHASE3_JA_RX_DETECTED) {
			detected = true;
			break;
		}
	}
	CHECK(started_ja && detected && pp_events == 1u &&
	      decisions > V34_TRAINING_SEQUENCE_MIN_SYMBOLS &&
	      v90_phase3_ja_rx_handoff(&semantic, &handoff) ==
		      V90_PHASE3_JA_RX_OK &&
	      v34_precoder_recommendation_is_valid(&handoff.precoder) &&
	      v90_dil_encode(&expected, expected_bits, sizeof(expected_bits),
		      &expected_count) == V90_DIL_OK &&
	      v90_dil_encode(&handoff.dil, received_bits, sizeof(received_bits),
		      &received_count) == V90_DIL_OK &&
	      expected_count == 1702u && received_count == expected_count &&
	      memcmp(expected_bits, received_bits, (expected_count + 7u) / 8u) == 0,
	      "law %d fixed decisions did not recover the exact Ja descriptor",
	      law);
	{
		struct v90_phase3_ja_rx saved = semantic;
		struct qam_point decision = { 0.5, 0.5 };
		struct qam_point target = { 9.0, 7.0 };
		struct qam_point saved_target = target;

		CHECK(v90_phase3_ja_rx_observe(&semantic, &decision, &target) ==
			      V90_PHASE3_JA_RX_COMPLETE &&
		      memcmp(&semantic, &saved, sizeof(semantic)) == 0 &&
		      memcmp(&target, &saved_target, sizeof(target)) == 0,
		      "law %d complete Ja consumed another decision", law);
	}
	CHECK(check_fixed_sample_rejection(&line),
	      "law %d fixed sample rejection contract failed", law);
	return true;
}

static bool test_fixed_ja_lines(void)
{
	CHECK(run_fixed_ja_line(G711_LAW_PCMU) &&
	      run_fixed_ja_line(G711_LAW_PCMA),
	      "fixed G.711 Ja timelines failed");
	return true;
}

static bool test_validation_and_transactionality(void)
{
	struct v34_phase3_profile selected = profile();
	struct v90_phase3_analog_line_rx rx;
	struct v90_phase3_analog_line_rx saved;
	struct v90_phase3_analog_line_handoff handoff;
	struct v90_phase3_analog_line_handoff saved_handoff;

	memset(&rx, 0xa5, sizeof(rx));
	saved = rx;
	selected.local_role = V34_CALLING_STATION;
	CHECK(v90_phase3_analog_line_rx_init(
		      &rx, &selected, TEST_INITIAL_RECEIVE_GAIN) ==
		      V90_PHASE3_ANALOG_LINE_RX_INVALID_ARGUMENT &&
	      memcmp(&rx, &saved, sizeof(rx)) == 0,
	      "line receiver accepted the digital station profile");
	selected = profile();
	selected.md_length_35_ms_units = 1;
	selected.md_symbol_count = 105;
	CHECK(v90_phase3_analog_line_rx_init(
		      &rx, &selected, TEST_INITIAL_RECEIVE_GAIN) ==
		      V90_PHASE3_ANALOG_LINE_RX_INVALID_ARGUMENT,
	      "line receiver accepted nonzero MD");
	selected = profile();
	CHECK(v90_phase3_analog_line_rx_init(
		      &rx, NULL, TEST_INITIAL_RECEIVE_GAIN) ==
		      V90_PHASE3_ANALOG_LINE_RX_INVALID_ARGUMENT,
	      "line receiver accepted a missing profile");
	CHECK(v90_phase3_analog_line_rx_init(
		      &rx, &selected, 0) ==
		      V90_PHASE3_ANALOG_LINE_RX_INVALID_ARGUMENT &&
	      memcmp(&rx, &saved, sizeof(rx)) == 0,
	      "line receiver accepted zero gain or changed state");
	CHECK(v90_phase3_analog_line_rx_init(
		      &rx, &selected, TEST_INITIAL_RECEIVE_GAIN) ==
		      V90_PHASE3_ANALOG_LINE_RX_OK &&
	      v90_phase3_analog_line_rx_is_valid(&rx),
	      "valid line receiver did not initialize");
	memset(&handoff, 0x5a, sizeof(handoff));
	saved_handoff = handoff;
	CHECK(v90_phase3_analog_line_rx_sample(NULL, 0) ==
		      V90_PHASE3_ANALOG_LINE_RX_INVALID_ARGUMENT &&
	      v90_phase3_analog_line_rx_handoff(&rx, &handoff) ==
		      V90_PHASE3_ANALOG_LINE_RX_INVALID_STATE &&
	      memcmp(&handoff, &saved_handoff, sizeof(handoff)) == 0,
	      "invalid line receive calls changed their destination");
	rx.upstream.line.initialized = false;
	CHECK(!v90_phase3_analog_line_rx_is_valid(&rx) &&
	      !v90_phase3_analog_line_rx_has_failed(&rx) &&
	      !v90_phase3_analog_line_handoff_is_valid(NULL) &&
	      strcmp(v90_phase3_analog_line_rx_result_name(
		     V90_PHASE3_ANALOG_LINE_RX_TIMING_ACQUIRED),
		     "timing acquired") == 0 &&
	      strcmp(v90_phase3_analog_line_rx_result_name(
		     (enum v90_phase3_analog_line_rx_result)99),
		     "unknown") == 0,
	      "line receiver validation or result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "G.711 lines", test_g711_lines },
		{ "fixed G.711 Ja lines", test_fixed_ja_lines },
		{ "validation and transactionality",
		  test_validation_and_transactionality },
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
