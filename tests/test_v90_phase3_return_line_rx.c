// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>
#include <v34_constellation.h>
#include <v90_phase3_analog_line_rx.h>
#include <v90_phase3_analog_line_tx.h>
#include <v90_phase3_return_line_rx.h>
#include <v90_phase3_return_line_tx.h>

#include "test.h"

#include <math.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

enum {
	TRN_SYMBOLS = V34_TRAINING_SEQUENCE_MIN_SYMBOLS + 173,
	RETURN_SILENCE_SYMBOLS = 350,
	RETURN_S_FINISH_SYMBOL = V34_PHASE3_S_SYMBOLS + 17,
	TEST_SYMBOL_RATE = 3000,
	/* TX pulse half-span, 30-sample analytic group delay, half of the
	 * four-samples/symbol timing FIR, fixed EQ delay, and one boundary
	 * observation. The old floating receiver's 44T allowance omitted the
	 * fixed front end. This profile emits its exact final marker at 50T. */
	POST_POINT_LIMIT =
		V34_PULSE_TAPS / 2 +
		(V34_ANALYTIC_FILTER_GROUP_DELAY_SAMPLES * TEST_SYMBOL_RATE +
		 V90_PHASE3_SAMPLE_RATE - 1) / V90_PHASE3_SAMPLE_RATE +
		(V34_TIMING_FILTER_FIR_TAPS - 1 +
		 2 * V34_RECEIVE_TIMING_OUTPUTS - 1) /
			(2 * V34_RECEIVE_TIMING_OUTPUTS) +
		V90_UPSTREAM_TRAINING_RX_DECISION_DELAY + 1,
	SAMPLE_LIMIT = 40000,
	NOMINAL_AMPLITUDE = 6000,
	TEST_INITIAL_RECEIVE_GAIN = 0x0600,
};

static struct v34_phase3_profile profile(void)
{
	return (struct v34_phase3_profile){
		.symbol_rate_numerator = TEST_SYMBOL_RATE,
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

static bool establish_ja(
	enum g711_law law, struct v90_phase3_analog_line_tx *tx,
	struct v90_phase3_analog_line_handoff *handoff)
{
	struct v34_phase3_profile selected = profile();
	struct v90_phase3_analog_line_rx rx;
	bool started_ja = false;

	if (v90_phase3_analog_line_tx_init(
		    tx, &selected, NOMINAL_AMPLITUDE) !=
		    V90_PHASE3_ANALOG_LINE_TX_OK ||
	    v90_phase3_analog_line_rx_init(
		    &rx, &selected, TEST_INITIAL_RECEIVE_GAIN) !=
		    V90_PHASE3_ANALOG_LINE_RX_OK)
		return false;
	for (uint32_t index = 0; index < SAMPLE_LIMIT; ++index) {
		int16_t sample;
		enum v90_phase3_analog_line_rx_result result;

		if (!started_ja &&
		    tx->symbols.stage == V90_PHASE3_ANALOG_TX_PREFIX &&
		    tx->symbols.sequence.prefix.stage ==
			    V34_PHASE3_TRANSMITTER_TRN &&
		    tx->symbols.sequence.prefix.trn_symbols == TRN_SYMBOLS) {
			if (v90_phase3_analog_line_tx_start_ja(tx) !=
			    V90_PHASE3_ANALOG_LINE_TX_OK)
				return false;
			started_ja = true;
		}
		if (v90_phase3_analog_line_tx_sample(tx, &sample) !=
		    V90_PHASE3_ANALOG_LINE_TX_OK)
			return false;
		result = v90_phase3_analog_line_rx_sample(
			&rx, quantize(law, sample));
		if (result == V90_PHASE3_ANALOG_LINE_RX_JA_DETECTED)
			return started_ja &&
			       v90_phase3_analog_line_rx_handoff(
				       &rx, handoff) ==
				       V90_PHASE3_ANALOG_LINE_RX_OK;
		if (result != V90_PHASE3_ANALOG_LINE_RX_OK &&
		    result != V90_PHASE3_ANALOG_LINE_RX_TIMING_ACQUIRED &&
		    result != V90_PHASE3_ANALOG_LINE_RX_S_DETECTED &&
		    result != V90_PHASE3_ANALOG_LINE_RX_PP_COMPLETE &&
		    result != V90_PHASE3_ANALOG_LINE_RX_JA_REJECTED)
			return false;
	}
	return false;
}

static bool receive_return(
	enum g711_law law, struct v90_phase3_return_line_tx *tx,
	struct v90_phase3_return_line_rx *rx, struct v34_training_line *line,
	struct v90_phase3_return_line_handoff *handoff, uint16_t gain_q15)
{
	unsigned int s_events = 0;
	unsigned int s_bar_events = 0;
	unsigned int post_points = 0;
	bool started_s = false;
	bool finished_s = false;
	const unsigned int idle_symbols = tx->scr_during_idle ? 6000u :
		RETURN_SILENCE_SYMBOLS;

	for (uint32_t index = 0; index < SAMPLE_LIMIT; ++index) {
		int16_t sample;
		enum v90_phase3_return_line_rx_result result;

		if (!started_s &&
		    tx->points.silence_symbols == idle_symbols) {
			if (tx->scr_during_idle) {
				uint64_t error = 0, power = 0;

				/* On this clean codec path, long SCR must preserve the
				 * negotiated constellation, not converge to four points. */
				for (size_t symbol = 0;
				     symbol < rx->upstream.recent_decision_count;
				     ++symbol) {
					struct v34_analytic_sample decision;
					struct v34_fixed_training_decision hard;

					CHECK(v90_upstream_training_rx_recent_decision(
						      &rx->upstream, symbol, &decision) &&
					      v34_training_constellation_fixed_slice(
						      tx->idle_scr.constellation, decision, &hard) ==
						      V34_CONSTELLATION_OK,
					      "SCR decision unavailable");
					int64_t i = (int32_t)decision.in_phase -
						hard.target.in_phase;
					int64_t q = (int32_t)decision.quadrature -
						hard.target.quadrature;

					error += (uint64_t)(i * i + q * q);
					power += (uint64_t)(
						(int64_t)hard.target.in_phase * hard.target.in_phase +
						(int64_t)hard.target.quadrature * hard.target.quadrature);
				}
				CHECK(power > 0 && 100u * error < power,
				      "law %u SCR error %.4f exceeds clean-line bound", law,
				      power ? (double)error / power : 1.0);
			}
			CHECK(v90_phase3_return_line_tx_start_s(tx) ==
				      V90_PHASE3_RETURN_LINE_TX_OK,
			      "law %d could not start return S", law);
			started_s = true;
		}
		if (started_s && !finished_s &&
		    tx->points.s_symbols == RETURN_S_FINISH_SYMBOL) {
			CHECK(v90_phase3_return_line_tx_finish_s(tx) ==
				      V90_PHASE3_RETURN_LINE_TX_OK,
			      "law %d could not finish return S", law);
			finished_s = true;
		}
		if (v90_phase3_return_line_tx_is_handoff_ready(tx))
			break;
		CHECK(v90_phase3_return_line_tx_sample(tx, &sample) ==
			      V90_PHASE3_RETURN_LINE_TX_OK,
		      "law %d return line stopped", law);
		result = v90_phase3_return_line_rx_sample(
			rx, quantize(law, (int16_t)((int32_t)sample * gain_q15 / 32768)));
		if (result == V90_PHASE3_RETURN_LINE_RX_S_DETECTED) {
			CHECK(rx->upstream.tracking_constellation == V34_CONSTELLATION_4_POINT,
			      "return S retained the SCR constellation");
			s_events++;
		} else
			CHECK(result == V90_PHASE3_RETURN_LINE_RX_OK,
			      "law %d return line reported %s early", law,
			      v90_phase3_return_line_rx_result_name(result));
	}
	CHECK(started_s && finished_s && s_events == 1 &&
	      v90_phase3_return_line_tx_handoff(tx, line) ==
		      V90_PHASE3_RETURN_LINE_TX_OK,
	      "law %d did not preserve the S-bar line boundary", law);
	for (uint32_t sample_index = 0;
	     sample_index < SAMPLE_LIMIT && s_bar_events == 0; ++sample_index) {
		struct qam_point point;
		const struct qam_point *supplied = NULL;
		int16_t sample;
		bool accepting = v34_training_line_needs_point(line);
		bool accepted;
		enum v90_phase3_return_line_rx_result result;

		if (accepting) {
			CHECK(post_points < POST_POINT_LIMIT,
			      "law %d required too many Phase 4 points", law);
			point = v34_rotated_point_zero(
				(int)((post_points * 3u) & 3u));
			supplied = &point;
		}
		CHECK(v34_training_line_sample(
			      line, supplied, &sample, &accepted) ==
			      V34_TRAINING_LINE_OK &&
		      accepted == accepting,
		      "law %d could not continue the Phase 4 line", law);
		if (accepted)
			post_points++;
		result = v90_phase3_return_line_rx_sample(
			rx, quantize(law, (int16_t)((int32_t)sample * gain_q15 / 32768)));
		if (result == V90_PHASE3_RETURN_LINE_RX_S_BAR_DETECTED)
			s_bar_events++;
		else
			CHECK(result == V90_PHASE3_RETURN_LINE_RX_OK,
			      "law %d Phase 4 boundary reported %s", law,
			      v90_phase3_return_line_rx_result_name(result));
	}
	CHECK(s_bar_events == 1 &&
	      post_points >= V90_UPSTREAM_TRAINING_RX_DECISION_DELAY &&
	      !v90_phase3_return_line_rx_has_failed(rx) &&
	      v90_phase3_return_line_rx_handoff(rx, handoff) ==
		      V90_PHASE3_RETURN_LINE_RX_OK &&
	      v90_phase3_return_line_handoff_is_valid(handoff),
	      "law %d did not recover the complete return line", law);
	return true;
}

static bool run_line(enum g711_law law, bool scr)
{
	struct v90_phase3_analog_line_tx prefix_tx;
	struct v90_phase3_analog_line_handoff prefix_handoff;
	struct v90_phase3_return_line_tx return_tx;
	struct v90_phase3_return_line_rx return_rx;
	struct v90_phase3_return_line_handoff return_handoff;
	struct v34_training_line line;
	uint32_t finish_ja_at;
	bool finish_ja_requested = false;

	CHECK(establish_ja(law, &prefix_tx, &prefix_handoff) &&
	      v90_phase3_return_line_rx_init(
		      &return_rx, &prefix_handoff) ==
		      V90_PHASE3_RETURN_LINE_RX_OK,
	      "law %d could not hand Ja to the return receiver", law);
	finish_ja_at = prefix_tx.symbols.ja_symbols +
		V90_PHASE3_JA_DESCRIPTOR_SYMBOLS;
	for (uint32_t index = 0; index < SAMPLE_LIMIT; ++index) {
		int16_t sample;
		enum v90_phase3_return_line_rx_result result;

		if (!finish_ja_requested &&
		    prefix_tx.symbols.ja_symbols >= finish_ja_at) {
			CHECK(v90_phase3_analog_line_tx_finish_ja(&prefix_tx) ==
				      V90_PHASE3_ANALOG_LINE_TX_OK,
			      "law %d could not finish Ja", law);
			finish_ja_requested = true;
		}
		if (v90_phase3_analog_line_tx_is_handoff_ready(&prefix_tx))
			break;
		CHECK(v90_phase3_analog_line_tx_sample(&prefix_tx, &sample) ==
			      V90_PHASE3_ANALOG_LINE_TX_OK,
		      "law %d Ja line stopped", law);
		result = v90_phase3_return_line_rx_sample(
			&return_rx, quantize(law, sample));
		CHECK(result == V90_PHASE3_RETURN_LINE_RX_OK,
		      "law %d Ja caused false return event %s", law,
		      v90_phase3_return_line_rx_result_name(result));
	}
	CHECK(finish_ja_requested &&
	      v90_phase3_analog_line_tx_handoff(&prefix_tx, &line) ==
		      V90_PHASE3_ANALOG_LINE_TX_OK &&
	      v90_phase3_return_line_tx_init(&return_tx, &line) ==
		      V90_PHASE3_RETURN_LINE_TX_OK,
	      "law %d did not preserve the Ja line boundary", law);
	CHECK(receive_return(
		      law, &return_tx, &return_rx, &line, &return_handoff, 32768),
	      "law %d first return failed", law);
	{
		struct v90_phase3_return_line_rx saved = return_rx;

		CHECK(v90_phase3_return_line_rx_sample(&return_rx, 0) ==
			      V90_PHASE3_RETURN_LINE_RX_COMPLETE &&
		      memcmp(&return_rx, &saved, sizeof(return_rx)) == 0,
		      "law %d complete receiver consumed a sample", law);
	}
	{
		struct v90_phase3_return_line_rx saved = return_rx;

		CHECK(v90_phase3_return_line_rx_restart(&return_rx) ==
			      V90_PHASE3_RETURN_LINE_RX_OK &&
		      v90_phase3_return_line_rx_is_valid(&return_rx) &&
		      !v90_phase3_return_marker_is_complete(&return_rx.marker) &&
		      memcmp(&return_rx.upstream, &saved.upstream,
			     sizeof(saved.upstream)) == 0 &&
		      memcmp(&return_rx.precoder, &saved.precoder,
			     sizeof(saved.precoder)) == 0,
		      "law %d marker restart changed its physical owner", law);
	}
	for (uint32_t index = 0;
	     index < SAMPLE_LIMIT && !v34_training_line_needs_point(&line);
	     ++index) {
		int16_t sample;
		bool accepted;

		CHECK(v34_training_line_sample(
			      &line, NULL, &sample, &accepted) ==
			      V34_TRAINING_LINE_OK &&
		      !accepted &&
		      v90_phase3_return_line_rx_sample(
			      &return_rx, quantize(law, sample)) ==
			      V90_PHASE3_RETURN_LINE_RX_OK,
		      "law %d could not preserve the inter-return boundary", law);
	}
	CHECK(v34_training_line_needs_point(&line) &&
	      v90_upstream_training_rx_is_tracking(&return_rx.upstream) &&
	      !return_rx.upstream.line.equalizer.decision_pending,
	      "law %d did not reach the second-return boundary", law);
	/* Change the physical receive level before the second return, without
	 * replacing any learned timing, carrier, equalizer, or sample history. */
	if (scr)
		return_rx.upstream.tracking_constellation = V34_CONSTELLATION_16_POINT;
	CHECK(v90_phase3_return_line_rx_is_valid(&return_rx) &&
	      (scr ? v90_phase3_return_line_tx_init_scr(
		      &return_tx, &line, V34_CONSTELLATION_16_POINT) :
		      v90_phase3_return_line_tx_init(&return_tx, &line)) ==
		      V90_PHASE3_RETURN_LINE_TX_OK &&
	      receive_return(
		      law, &return_tx, &return_rx, &line, &return_handoff, 24576),
	      "law %d did not reacquire the second return", law);
	CHECK(return_rx.upstream.line.equalizer.equalizer.tap_count ==
		      prefix_handoff.upstream.line.equalizer.equalizer.tap_count &&
	      return_rx.upstream.line.equalizer.equalizer.identity_tap ==
		      return_rx.upstream.line.equalizer.equalizer.tap_count -
		      2u * V90_UPSTREAM_TRAINING_RX_DECISION_DELAY &&
	      memcmp(&return_handoff.upstream, &return_rx.upstream,
		     sizeof(return_handoff.upstream)) == 0 &&
	      memcmp(&return_handoff.precoder, &return_rx.precoder,
		     sizeof(return_handoff.precoder)) == 0 &&
	      memcmp(&return_handoff.marker, &return_rx.marker.marker.detection,
		     sizeof(return_handoff.marker)) == 0,
	      "law %d did not preserve and export its continuous fixed state", law);
	{
		struct v90_phase3_return_line_rx saved = return_rx;

		CHECK(v90_phase3_return_line_rx_handoff(&return_rx,
			      (struct v90_phase3_return_line_handoff *)&return_rx) ==
			      V90_PHASE3_RETURN_LINE_RX_INVALID_ARGUMENT &&
		      memcmp(&return_rx, &saved, sizeof(return_rx)) == 0,
		      "law %d return handoff overwrote its source receiver", law);
	}
	{
		struct v90_phase3_return_line_rx saved = return_rx;
		struct v90_upstream_training_rx expected = return_rx.upstream;

		for (unsigned int index = 0; index < 32; ++index) {
			struct qam_point point = v34_rotated_point_zero(
				(int)((index * 3u) & 3u));
			int16_t sample;
			bool accepted;

			CHECK(v34_training_line_sample(
				      &line,
				      v34_training_line_needs_point(&line) ?
					      &point : NULL,
				      &sample, &accepted) ==
				      V34_TRAINING_LINE_OK &&
			      v90_upstream_training_rx_advance(
				      &expected, quantize(law, sample)) ==
				      V90_UPSTREAM_TRAINING_RX_OK &&
			      v90_phase3_return_line_rx_advance_complete(
				      &return_rx, quantize(law, sample)) ==
				      V90_PHASE3_RETURN_LINE_RX_OK,
			      "law %d could not drain post-S-bar samples", law);
		}
		CHECK(
		      return_rx.handoff_draining &&
		      memcmp(&return_rx.marker, &saved.marker,
			     sizeof(saved.marker)) == 0 &&
		      memcmp(&return_rx.precoder, &saved.precoder,
			     sizeof(saved.precoder)) == 0 &&
		      memcmp(&return_rx.upstream, &expected,
			     sizeof(expected)) == 0 &&
		      memcmp(&return_rx.upstream.line, &saved.upstream.line,
			     sizeof(saved.upstream.line)) != 0 &&
		      memcmp(&return_rx.upstream.initial, &saved.upstream.initial,
			     sizeof(saved.upstream.initial)) == 0 &&
		      return_rx.upstream.tracking_symbols ==
			      saved.upstream.tracking_symbols &&
		      v90_phase3_return_line_rx_handoff(
			      &return_rx, &return_handoff) ==
			      V90_PHASE3_RETURN_LINE_RX_OK &&
		      memcmp(&return_handoff.upstream, &expected,
			     sizeof(expected)) == 0 &&
		      memcmp(&return_handoff.precoder, &saved.precoder,
			     sizeof(return_handoff.precoder)) == 0 &&
		      memcmp(&return_handoff.marker, &saved.marker.marker.detection,
			     sizeof(return_handoff.marker)) == 0,
		      "law %d completed return did not advance only its fixed line",
		      law);
		saved = return_rx;
		CHECK(v90_phase3_return_line_rx_restart(&return_rx) ==
			      V90_PHASE3_RETURN_LINE_RX_INVALID_STATE &&
		      memcmp(&return_rx, &saved, sizeof(return_rx)) == 0,
		      "law %d restarted the frozen completed parser", law);
	}
	return true;
}

static bool test_g711_lines(void)
{
	CHECK(run_line(G711_LAW_PCMU, false) && run_line(G711_LAW_PCMA, false),
	      "G.711 return lines failed");
	return true;
}

static bool test_negotiated_scr(void)
{
	CHECK(run_line(G711_LAW_PCMU, true) && run_line(G711_LAW_PCMA, true),
	      "negotiated SCR corrupted the continuous return receiver");
	return true;
}

/* The fixed line is trained once by S/PP and never restarted at either return.
 * The independently observed marker must match the runtime semantic owner.
 * The nonperiodic suffix detects a lost or repeated first post-S-bar decision
 * independently of a CP frame resynchronizer. */
static bool run_fixed_returns(enum g711_law law, unsigned int silence_symbols)
{
	static const uint8_t post_rotations[] = {
		0, 2, 1, 3, 3, 1, 2, 0, 1, 1, 3, 0, 2, 3, 2, 0,
	};
	const unsigned int first_s_end = silence_symbols + 192;
	const unsigned int second_s_start = first_s_end + 192;
	const unsigned int second_s_end = second_s_start + V34_PHASE3_S_SYMBOLS;
	const unsigned int post_start = second_s_end + V34_PHASE3_S_BAR_SYMBOLS;
	struct v90_phase3_analog_line_tx prefix;
	struct v90_phase3_analog_line_handoff handoff;
	struct v90_phase3_return_marker marker;
	struct v34_training_line tx;
	struct v34_phase3_s_detection detection = { 0 };
	unsigned int supplied = 0;
	unsigned int post_received = 0;
	unsigned int onset_events = 0;
	unsigned int unsaturated_quiet_decisions = 0;
	unsigned int held_samples = 0;
	unsigned int advancing_held_samples = 0;
	bool first_ended = false;
	bool second_ended = false;

	CHECK(establish_ja(law, &prefix, &handoff) &&
	      v90_phase3_analog_line_tx_finish_ja(&prefix) ==
		      V90_PHASE3_ANALOG_LINE_TX_OK,
	      "law %d fixed return could not finish Ja", law);
	for (unsigned int index = 0; index < SAMPLE_LIMIT &&
	     !v90_phase3_analog_line_tx_is_handoff_ready(&prefix); ++index) {
		struct v34_analytic_sample decision;
		enum v90_upstream_training_rx_result sampled;
		int16_t sample;

		CHECK(v90_phase3_analog_line_tx_sample(&prefix, &sample) ==
			      V90_PHASE3_ANALOG_LINE_TX_OK,
		      "law %d fixed return could not finish its Ja source", law);
		sampled = v90_upstream_training_rx_sample(
			&handoff.upstream, quantize(law, sample), &decision);
		CHECK(sampled == V90_UPSTREAM_TRAINING_RX_OK ||
		      sampled == V90_UPSTREAM_TRAINING_RX_DECISION_READY,
		      "law %d fixed return lost its Ja line", law);
	}
	CHECK(v90_phase3_analog_line_tx_handoff(&prefix, &tx) ==
		      V90_PHASE3_ANALOG_LINE_TX_OK &&
	      v90_phase3_return_marker_init(&marker, true) ==
		      V90_PHASE3_RETURN_RX_OK,
	      "law %d fixed return did not initialize", law);
	for (unsigned int index = 0;
	     index < SAMPLE_LIMIT && post_received < ARRAY_LENGTH(post_rotations);
	     ++index) {
		struct qam_point point = { 0 };
		struct v34_analytic_sample decision = { INT16_MIN, INT16_MAX };
		struct v34_analytic_sample saved_decision = decision;
		struct v34_analytic_sample retained;
		struct v34_fixed_training_decision hard;
		uint16_t previous = handoff.upstream.next_recent_decision;
		uint32_t previous_tracking = handoff.upstream.tracking_symbols;
		struct v34_timing_control timing_before =
			handoff.upstream.line.timing.timing;
		bool accepting = v34_training_line_needs_point(&tx);
		bool accepted;
		int16_t sample;
		enum v90_upstream_training_rx_result sampled;
		enum v90_phase3_return_rx_result result;

		if (supplied >= post_start)
			point = v34_rotated_point_zero(post_rotations[
				(supplied - post_start) % ARRAY_LENGTH(post_rotations)]);
		else if (supplied >= second_s_end)
			point = v34_rotated_point_zero(
				(int)(2u + ((supplied - second_s_end) & 1u)));
		else if (supplied >= second_s_start)
			point = v34_rotated_point_zero(
				(int)((supplied - second_s_start) & 1u));
		else if (supplied >= first_s_end + V34_PHASE3_S_BAR_SYMBOLS)
			point = v34_rotated_point_zero(2);
		else if (supplied >= first_s_end)
			point = v34_rotated_point_zero(
				(int)(2u + ((supplied - first_s_end) & 1u)));
		else if (supplied >= silence_symbols)
			point = v34_rotated_point_zero(
				(int)((supplied - silence_symbols) & 1u));
		CHECK(v34_training_line_sample(
			      &tx, accepting ? &point : NULL, &sample, &accepted) ==
			      V34_TRAINING_LINE_OK && accepted == accepting,
		      "law %d fixed return source stopped at sample %u", law, index);
		sampled = v90_upstream_training_rx_sample(
			&handoff.upstream, quantize(law, sample), &decision);
		CHECK(sampled == V90_UPSTREAM_TRAINING_RX_OK ||
		      sampled == V90_UPSTREAM_TRAINING_RX_DECISION_READY,
		      "law %d fixed return stopped at sample %u", law, index);
		if (accepted)
			supplied++;
		if (timing_before.feedback_held) {
			const struct v34_timing_control *timing =
				&handoff.upstream.line.timing.timing;
			struct v34_timing_control expected = timing_before;

			held_samples++;
			advancing_held_samples += timing->phase != timing_before.phase;
			expected.phase = timing->phase;
			expected.phase_increment = timing->phase_increment;
			expected.phase_residue = timing->phase_residue;
			expected.accumulated_delta = timing->accumulated_delta;
			expected.accumulated_count = timing->accumulated_count;
			expected.offset_ppm = timing->offset_ppm;
			expected.feedback_held = timing->feedback_held;
			/* A copied subobject need not preserve its trailing padding. */
			CHECK(memcmp(timing, &expected,
				      offsetof(struct v34_timing_control, initialized) +
				      sizeof(expected.initialized)) == 0,
			      "law %d quiet feedback changed timing: mode=%u/%u "
			      "count=%u/%u integrator=%d/%d held=%u/%u gains=%d,%d/%d,%d",
			      law, timing->mode, expected.mode,
			      timing->state_count, expected.state_count,
			      timing->integrator, expected.integrator,
			      timing->feedback_held, expected.feedback_held,
			      timing->proportional_gain, timing->integral_gain,
			      expected.proportional_gain, expected.integral_gain);
		}
		if (sampled == V90_UPSTREAM_TRAINING_RX_OK) {
			CHECK(handoff.upstream.next_recent_decision == previous &&
			      memcmp(&decision, &saved_decision, sizeof(decision)) == 0,
			      "law %d fixed return changed a nonready decision", law);
			continue;
		}
		CHECK(handoff.upstream.next_recent_decision == (previous + 1u) %
			      V90_UPSTREAM_TRAINING_RX_HISTORY_SYMBOLS &&
		      v90_upstream_training_rx_recent_decision(
			      &handoff.upstream,
			      handoff.upstream.recent_decision_count - 1u, &retained) &&
		      memcmp(&decision, &retained, sizeof(decision)) == 0 &&
		      v34_training_constellation_fixed_slice(
			      V34_CONSTELLATION_4_POINT, decision, &hard) ==
			      V34_CONSTELLATION_OK,
		      "law %d fixed return did not export a decision", law);
		/* Quiet decisions still belong to the semantic timeline and history,
		 * even though they must not advance decision-directed adaptation. */
		if (abs(decision.in_phase) < V34_TRAINING_FIXED_AMPLITUDE / 4 &&
		    abs(decision.quadrature) < V34_TRAINING_FIXED_AMPLITUDE / 4) {
			if (previous_tracking <
			    handoff.upstream.line.timing.timing.nominal_symbol_rate)
				unsaturated_quiet_decisions++;
			CHECK(handoff.upstream.tracking_symbols == previous_tracking &&
			      handoff.upstream.line.timing.timing.feedback_held,
			      "law %d quiet decision advanced tracking", law);
		}
		if (second_ended) {
			CHECK(((hard.clockwise_quarter_turns + 4u -
				detection.clockwise_quarter_turn_offset) & 3u) ==
				      ((4u - post_rotations[post_received]) & 3u),
			      "law %d fixed return displaced post decision %u",
			      law, post_received);
			post_received++;
			continue;
		}
		result = v90_phase3_return_marker_observe(
			&marker, hard.clockwise_quarter_turns);
		if (result == V90_PHASE3_RETURN_RX_S_DETECTED)
			onset_events++;
		else if (result == V90_PHASE3_RETURN_RX_S_BAR_DETECTED) {
			if (!first_ended) {
				CHECK(onset_events == 1u &&
				      v90_phase3_return_marker_restart(&marker) ==
					      V90_PHASE3_RETURN_RX_OK,
				      "law %d fixed first return lost its S-bar", law);
				first_ended = true;
				continue;
			}
			CHECK(first_ended && onset_events == 2u &&
			      marker.confirmed_s_symbols == V34_PHASE3_S_SYMBOLS &&
			      marker.marker.post_marker_symbols == 0u &&
			      v90_phase3_return_marker_get_detection(&marker, &detection),
			      "law %d fixed second return lost its exact boundary", law);
			second_ended = true;
		} else {
			CHECK(result == V90_PHASE3_RETURN_RX_OK,
			      "law %d fixed return reported %d", law, result);
		}
	}
	CHECK(first_ended && second_ended && unsaturated_quiet_decisions > 0 &&
	      held_samples > silence_symbols &&
	      advancing_held_samples > silence_symbols &&
	      !handoff.upstream.line.timing.timing.feedback_held &&
	      post_received == ARRAY_LENGTH(post_rotations),
	      "law %d fixed line did not complete both return epochs", law);
	return true;
}

static bool test_fixed_returns(void)
{
	CHECK(run_fixed_returns(G711_LAW_PCMU, RETURN_SILENCE_SYMBOLS) &&
	      run_fixed_returns(G711_LAW_PCMA, RETURN_SILENCE_SYMBOLS) &&
	      run_fixed_returns(G711_LAW_PCMU, 6000u) &&
	      run_fixed_returns(G711_LAW_PCMA, 6000u),
	      "fixed G.711 return semantics or long quiet hold failed");
	return true;
}

static bool test_validation(void)
{
	struct v90_phase3_analog_line_tx tx;
	struct v90_phase3_analog_line_handoff source;
	struct v90_phase3_return_line_rx rx;
	struct v90_phase3_return_line_rx saved;
	struct v90_phase3_return_line_handoff handoff;
	struct v90_phase3_return_line_handoff saved_handoff;

	CHECK(establish_ja(G711_LAW_PCMU, &tx, &source),
	      "validation source did not establish Ja");
	memset(&rx, 0xa5, sizeof(rx));
	saved = rx;
	{
		struct v90_phase3_analog_line_handoff invalid = source;

		invalid.upstream.line.initialized = false;
		CHECK(v90_phase3_return_line_rx_init(&rx, &invalid) ==
			      V90_PHASE3_RETURN_LINE_RX_INVALID_ARGUMENT &&
		      memcmp(&rx, &saved, sizeof(rx)) == 0,
		      "invalid fixed handoff changed state");
	}
	CHECK(v90_phase3_return_line_rx_init(&rx, &source) ==
		      V90_PHASE3_RETURN_LINE_RX_OK,
	      "valid return line receiver did not initialize");
	saved = rx;
	CHECK(v90_phase3_return_line_rx_restart(NULL) ==
		      V90_PHASE3_RETURN_LINE_RX_INVALID_ARGUMENT &&
	      v90_phase3_return_line_rx_restart(&rx) ==
		      V90_PHASE3_RETURN_LINE_RX_INVALID_STATE &&
	      memcmp(&rx, &saved, sizeof(rx)) == 0,
	      "invalid restart changed receiver state");
	memset(&handoff, 0x5a, sizeof(handoff));
	saved_handoff = handoff;
	CHECK(v90_phase3_return_line_rx_sample(NULL, 0) ==
		      V90_PHASE3_RETURN_LINE_RX_INVALID_ARGUMENT &&
	      v90_phase3_return_line_rx_advance_complete(NULL, 0) ==
		      V90_PHASE3_RETURN_LINE_RX_INVALID_ARGUMENT &&
	      v90_phase3_return_line_rx_advance_complete(&rx, 0) ==
		      V90_PHASE3_RETURN_LINE_RX_INVALID_STATE &&
	      memcmp(&rx, &saved, sizeof(rx)) == 0 &&
	      v90_phase3_return_line_rx_handoff(&rx, &handoff) ==
		      V90_PHASE3_RETURN_LINE_RX_INVALID_STATE &&
	      memcmp(&handoff, &saved_handoff, sizeof(handoff)) == 0,
	      "invalid return-line call changed its destination");
	rx.handoff_draining = true;
	CHECK(!v90_phase3_return_line_rx_is_valid(&rx),
	      "incomplete return was allowed to drain its handoff");
	rx = saved;
	rx.upstream.line.initialized = false;
	CHECK(!v90_phase3_return_line_rx_is_valid(&rx) &&
	      !v90_phase3_return_line_rx_has_failed(&rx) &&
	      !v90_phase3_return_line_handoff_is_valid(NULL) &&
	      strcmp(v90_phase3_return_line_rx_result_name(
		     V90_PHASE3_RETURN_LINE_RX_S_BAR_DETECTED),
		     "S-bar detected") == 0 &&
	      strcmp(v90_phase3_return_line_rx_result_name(
		     (enum v90_phase3_return_line_rx_result)99),
		     "unknown") == 0,
	      "return-line validation or names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "G.711 lines", test_g711_lines },
		{ "negotiated SCR", test_negotiated_scr },
		{ "fixed return decisions", test_fixed_returns },
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
