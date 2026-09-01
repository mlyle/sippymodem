// SPDX-License-Identifier: BSD-3-Clause

#include <v34_constellation.h>
#include <v90_phase3_analog_tx.h>
#include <v90_phase3_return_rx.h>
#include <v90_phase3_return_tx.h>

#include "test.h"

#include <float.h>
#include <math.h>
#include <string.h>

enum {
	JA_REPETITIONS = 4,
	TRN_SYMBOLS = V34_TRAINING_SEQUENCE_MIN_SYMBOLS + 97,
};

static struct qam_point rotate(
	struct qam_point point, unsigned int quarter_turns)
{
	switch (quarter_turns & 3u) {
	case 0:
		return point;
	case 1:
		return (struct qam_point){ -point.quadrature, point.in_phase };
	case 2:
		return (struct qam_point){ -point.in_phase, -point.quadrature };
	case 3:
		return (struct qam_point){ point.quadrature, -point.in_phase };
	}
	return (struct qam_point){ 0 };
}

static struct qam_point channel_point(
	struct qam_point point, unsigned int quarter_turns)
{
	point = rotate(point, quarter_turns);
	point.in_phase *= 0.83;
	point.quadrature *= 0.83;
	return point;
}

static bool seed_handoff(
	unsigned int quarter_turns,
	struct v34_equalizer_handoff *handoff)
{
	struct qam_point reference = v34_rotated_point_zero(0);
	struct qam_point received = channel_point(reference, quarter_turns);

	if (qam_equalizer_init_geometry(
		    &handoff->equalizer, &received, &reference,
		    V34_EQUALIZER_TAP_COUNT,
		    V34_EQUALIZER_DECISION_DELAY) !=
	    QAM_EQUALIZER_OK)
		return false;
	for (unsigned int index = 0;
	     index <= V34_EQUALIZER_DECISION_DELAY; ++index) {
		struct qam_equalizer_observation observation = { 0 };
		struct qam_point point = v34_rotated_point_zero((int)index);
		bool ready;

		point = channel_point(point, quarter_turns);
		if (qam_equalizer_observe(
			    &handoff->equalizer, &point,
			    &observation, &ready) != QAM_EQUALIZER_OK ||
		    ready != (index == V34_EQUALIZER_DECISION_DELAY))
			return false;
	}
	return v34_equalizer_handoff_is_valid(handoff);
}

static bool seed_tracked_handoff(
	unsigned int quarter_turns,
	struct v34_equalizer_handoff *handoff)
{
	return seed_handoff(quarter_turns, handoff) &&
	       qam_equalizer_enable_carrier_tracking(
		       &handoff->equalizer, 1.0 / 4.0, 1.0 / 8192.0) ==
		       QAM_EQUALIZER_OK &&
	       v34_equalizer_handoff_is_valid(handoff);
}

static bool start_ja(struct v90_phase3_analog_tx *tx)
{
	if (v90_phase3_analog_tx_init(tx) != V90_PHASE3_ANALOG_TX_OK)
		return false;
	for (size_t emitted = 0; emitted < 2000; ++emitted) {
		struct qam_point point;

		if (tx->stage == V90_PHASE3_ANALOG_TX_PREFIX &&
		    tx->sequence.prefix.stage == V34_PHASE3_TRANSMITTER_TRN &&
		    tx->sequence.prefix.trn_symbols == TRN_SYMBOLS)
			return v90_phase3_analog_tx_start_ja(tx) ==
			       V90_PHASE3_ANALOG_TX_OK;
		if (v90_phase3_analog_tx_next(tx, &point) !=
		    V90_PHASE3_ANALOG_TX_OK)
			return false;
	}
	return false;
}

static bool run_timeline(
	unsigned int quarter_turns, uint32_t finish_after, bool tracked)
{
	static const unsigned int post_rotations[] = {
		0, 2, 1, 3, 3, 1, 2, 0,
	};
	struct v34_equalizer_handoff initial;
	struct v90_phase3_analog_tx ja;
	struct v90_phase3_return_tx tx;
	struct v90_phase3_return_rx rx;
	struct v90_phase3_return_handoff handoff;
	enum v90_phase3_return_rx_result handoff_result;
	unsigned int s_events = 0;
	unsigned int s_bar_events = 0;
	unsigned int post_consumed = 0;
	uint32_t s_at_detection = 0;

	CHECK((tracked ? seed_tracked_handoff(quarter_turns, &initial) :
			 seed_handoff(quarter_turns, &initial)) &&
	      start_ja(&ja) &&
	      v90_phase3_return_rx_init(&rx, &initial, 0.05) ==
		      V90_PHASE3_RETURN_RX_OK,
	      "phase %u return receiver did not initialize", quarter_turns);
	for (size_t symbol = 0;
	     symbol < JA_REPETITIONS * V90_PHASE3_JA_DESCRIPTOR_SYMBOLS;
	     ++symbol) {
		struct qam_point point;

		CHECK(v90_phase3_analog_tx_next(&ja, &point) ==
			      V90_PHASE3_ANALOG_TX_OK,
		      "phase %u Ja source stopped", quarter_turns);
		point = channel_point(point, quarter_turns);
		CHECK(v90_phase3_return_rx_point(&rx, &point) ==
			      V90_PHASE3_RETURN_RX_OK,
		      "phase %u Ja caused a false S event", quarter_turns);
	}
	CHECK(v90_phase3_return_tx_init(&tx) == V90_PHASE3_RETURN_TX_OK,
	      "phase %u return source did not initialize", quarter_turns);
	for (size_t symbol = 0; symbol < 73; ++symbol) {
		struct qam_point point;

		CHECK(v90_phase3_return_tx_next(&tx, &point) ==
			      V90_PHASE3_RETURN_TX_OK,
		      "phase %u silence source stopped", quarter_turns);
		point = channel_point(point, quarter_turns);
		CHECK(v90_phase3_return_rx_point(&rx, &point) ==
			      V90_PHASE3_RETURN_RX_OK,
		      "phase %u silence caused a false S event", quarter_turns);
	}
	CHECK(v90_phase3_return_tx_start_s(&tx) == V90_PHASE3_RETURN_TX_OK,
	      "phase %u could not start S", quarter_turns);
	while (!v90_phase3_return_tx_is_complete(&tx)) {
		struct qam_point point;
		enum v90_phase3_return_tx_stage stage = tx.stage;
		uint32_t s_symbol = tx.s_symbols;
		enum v90_phase3_return_rx_result result;

		if (stage == V90_PHASE3_RETURN_TX_S &&
		    s_symbol == finish_after)
			CHECK(v90_phase3_return_tx_finish_s(&tx) ==
				      V90_PHASE3_RETURN_TX_OK,
			      "phase %u could not finish S", quarter_turns);
		CHECK(v90_phase3_return_tx_next(&tx, &point) ==
			      V90_PHASE3_RETURN_TX_OK,
		      "phase %u return source stopped", quarter_turns);
		if (stage == V90_PHASE3_RETURN_TX_S &&
		    (s_symbol == 19u || s_symbol == 47u))
			point = v34_rotated_point_zero(2);
		point = channel_point(point, quarter_turns);
		result = v90_phase3_return_rx_point(&rx, &point);
		if (result == V90_PHASE3_RETURN_RX_S_DETECTED) {
			s_events++;
			s_at_detection = tx.s_symbols;
		} else if (result == V90_PHASE3_RETURN_RX_S_BAR_DETECTED) {
			s_bar_events++;
		} else {
			CHECK(result == V90_PHASE3_RETURN_RX_OK,
			      "phase %u receiver returned %s",
			      quarter_turns,
			      v90_phase3_return_rx_result_name(result));
		}
	}
	while (s_bar_events == 0 &&
	       post_consumed < V34_EQUALIZER_DECISION_DELAY) {
		struct qam_point point = v34_rotated_point_zero(
			(int)post_rotations[
				post_consumed % ARRAY_LENGTH(post_rotations)]);
		enum v90_phase3_return_rx_result result;

		point = channel_point(point, quarter_turns);
		post_consumed++;
		result = v90_phase3_return_rx_point(&rx, &point);
		if (result == V90_PHASE3_RETURN_RX_S_BAR_DETECTED)
			s_bar_events++;
		else
			CHECK(result == V90_PHASE3_RETURN_RX_OK,
			      "phase %u boundary returned %s", quarter_turns,
			      v90_phase3_return_rx_result_name(result));
	}
	handoff_result = v90_phase3_return_rx_handoff(&rx, &handoff);
	CHECK(s_events == 1 && s_bar_events == 1 &&
	      s_at_detection ==
		      (tracked ?
			       V90_PHASE3_RETURN_RX_TRACKED_ONSET_SYMBOLS :
			       V90_PHASE3_RETURN_RX_ONSET_SYMBOLS) +
					V34_EQUALIZER_DECISION_DELAY &&
	      rx.marker.s_symbols ==
		      V90_PHASE3_RETURN_RX_MARKER_SUFFIX_SYMBOLS &&
	      post_consumed == V34_EQUALIZER_DECISION_DELAY &&
	      handoff_result == V90_PHASE3_RETURN_RX_OK &&
	      v90_phase3_return_handoff_is_valid(&handoff),
	      "phase %u boundaries: S=%u Sbar=%u onset=%u post=%u "
	      "stage=%d handoff=%s",
	      quarter_turns, s_events, s_bar_events, s_at_detection,
	      post_consumed, rx.stage,
	      v90_phase3_return_rx_result_name(handoff_result));
	{
		struct qam_point point = v34_rotated_point_zero(
			(int)post_rotations[
				post_consumed % ARRAY_LENGTH(post_rotations)]);
		struct qam_point output;
		uint8_t quarter_point;
		uint8_t rotation;
		bool ready;

		point = channel_point(point, quarter_turns);
		CHECK(qam_equalizer_push(
			      &handoff.equalizer.equalizer, &point,
			      &output, &ready) == QAM_EQUALIZER_OK &&
		      ready &&
		      v34_training_constellation_slice(
			      V34_CONSTELLATION_4_POINT, &output,
			      &quarter_point, &rotation) ==
			      V34_CONSTELLATION_OK &&
		      quarter_point == 0 && rotation == post_rotations[0],
		      "phase %u handoff replayed or skipped a point",
		      quarter_turns);
	}
	{
		struct v90_phase3_return_rx saved = rx;
		struct qam_point point = { 1.0, 1.0 };

		CHECK(v90_phase3_return_rx_point(&rx, &point) ==
			      V90_PHASE3_RETURN_RX_COMPLETE &&
		      memcmp(&rx, &saved, sizeof(rx)) == 0,
		      "phase %u complete receiver consumed a point",
		      quarter_turns);
	}
	return true;
}

static bool test_generated_timelines(void)
{
	CHECK(run_timeline(0, V34_PHASE3_S_SYMBOLS, false) &&
	      run_timeline(1, V34_PHASE3_S_SYMBOLS + 53u, false) &&
	      run_timeline(3, V34_PHASE3_S_SYMBOLS + 18u, false) &&
	      run_timeline(2, V34_PHASE3_S_SYMBOLS + 31u, true),
	      "generated return timelines failed");
	return true;
}

static bool test_tracked_onset_confirmation(void)
{
	const unsigned int quarter_turns = 2;
	struct v34_equalizer_handoff equalizer;
	struct v90_phase3_return_rx rx;
	unsigned int s_events = 0;

	CHECK(seed_tracked_handoff(quarter_turns, &equalizer) &&
	      v90_phase3_return_rx_init(&rx, &equalizer, 0.05) ==
		      V90_PHASE3_RETURN_RX_OK &&
	      rx.equalizer.carrier_tracking,
	      "tracked return receiver did not preserve carrier tracking");
	for (size_t symbol = 0;
	     symbol < V90_PHASE3_RETURN_RX_TRACKED_ONSET_SYMBOLS; ++symbol) {
		struct qam_point point = v34_rotated_point_zero(2);

		point = channel_point(point, quarter_turns);
		CHECK(v90_phase3_return_rx_point(&rx, &point) ==
			      V90_PHASE3_RETURN_RX_OK,
		      "tracked pre-S fill reported an event at %zu", symbol);
	}
	CHECK(rx.confirmed_s_symbols == 0,
	      "tracked pre-S fill created an onset candidate");
	for (size_t symbol = 0;
	     symbol < V90_PHASE3_RETURN_RX_ONSET_SYMBOLS;
	     ++symbol) {
		struct qam_point point = v34_rotated_point_zero((int)(symbol & 1u));

		point = channel_point(point, quarter_turns);
		CHECK(v90_phase3_return_rx_point(&rx, &point) ==
			      V90_PHASE3_RETURN_RX_OK,
		      "tracked tentative S reported an early event at %zu", symbol);
	}
	CHECK(rx.stage == V90_PHASE3_RETURN_RX_STAGE_SEARCH_S &&
	      rx.confirmed_s_symbols == 0,
	      "delayed tracked S produced an early onset candidate");
	for (size_t symbol = 0;
	     symbol < V34_EQUALIZER_DECISION_DELAY +
		      V90_PHASE3_RETURN_RX_ONSET_SYMBOLS + 3u;
	     ++symbol) {
		struct qam_point point = v34_rotated_point_zero(2);
		enum v90_phase3_return_rx_result result;

		point = channel_point(point, quarter_turns);
		result = v90_phase3_return_rx_point(&rx, &point);
		CHECK(result == V90_PHASE3_RETURN_RX_OK,
		      "rejected tentative S returned %s at %zu",
		      v90_phase3_return_rx_result_name(result), symbol);
	}
	CHECK(rx.stage == V90_PHASE3_RETURN_RX_STAGE_SEARCH_S &&
	      rx.confirmed_s_symbols == 0,
	      "false tracked S was not rejected transactionally");
	for (size_t symbol = 0; symbol < 64u && s_events == 0; ++symbol) {
		struct qam_point point = v34_rotated_point_zero((int)(symbol & 1u));
		enum v90_phase3_return_rx_result result;

		point = channel_point(point, quarter_turns);
		result = v90_phase3_return_rx_point(&rx, &point);
		if (result == V90_PHASE3_RETURN_RX_S_DETECTED)
			s_events++;
		else
			CHECK(result == V90_PHASE3_RETURN_RX_OK,
			      "confirmed tracked S reported %s",
			      v90_phase3_return_rx_result_name(result));
	}
	CHECK(s_events == 1 && rx.stage == V90_PHASE3_RETURN_RX_STAGE_S &&
	      rx.confirmed_s_symbols ==
		      V90_PHASE3_RETURN_RX_TRACKED_ONSET_SYMBOLS,
	      "tracked S was not reported after confirmation");
	return true;
}

static bool feed_implicit_return(
	struct v90_phase3_return_rx *rx, unsigned int quarter_turns,
	bool expect_implicit_end)
{
	unsigned int s_events = 0;
	unsigned int end_events = 0;

	for (size_t symbol = 0; symbol < 192u; ++symbol) {
		struct qam_point point = v34_rotated_point_zero((int)(symbol & 1u));
		enum v90_phase3_return_rx_result result;

		point = channel_point(point, quarter_turns);
		result = v90_phase3_return_rx_point(rx, &point);
		if (result == V90_PHASE3_RETURN_RX_S_DETECTED)
			s_events++;
		else
			CHECK(result == V90_PHASE3_RETURN_RX_OK,
			      "implicit-return S reported %s",
			      v90_phase3_return_rx_result_name(result));
	}
	for (size_t symbol = 0; symbol < 64u && end_events == 0; ++symbol) {
		struct qam_point point = v34_rotated_point_zero(2);
		enum v90_phase3_return_rx_result result;

		point = channel_point(point, quarter_turns);
		result = v90_phase3_return_rx_point(rx, &point);
		if (result == V90_PHASE3_RETURN_RX_S_ENDED)
			end_events++;
		else
			CHECK(result == V90_PHASE3_RETURN_RX_OK,
			      "implicit-return exit reported %s",
			      v90_phase3_return_rx_result_name(result));
	}
	CHECK(s_events == 1 && end_events == (expect_implicit_end ? 1u : 0u),
	      "implicit-return events changed: S=%u end=%u expected=%u",
	      s_events, end_events, expect_implicit_end ? 1u : 0u);
	return true;
}

static bool test_implicit_first_return_end(void)
{
	struct v34_equalizer_handoff equalizer;
	struct v90_phase3_return_rx rx;
	struct v90_phase3_return_rx saved;
	struct v90_phase3_return_handoff handoff;
	struct v90_phase3_return_handoff saved_handoff;
	struct qam_point point = v34_rotated_point_zero(0);

	CHECK(seed_handoff(1, &equalizer) &&
	      v90_phase3_return_rx_init(&rx, &equalizer, 0.05) ==
		      V90_PHASE3_RETURN_RX_OK &&
	      v90_phase3_return_rx_allow_implicit_s_end(&rx) ==
		      V90_PHASE3_RETURN_RX_OK &&
	      feed_implicit_return(&rx, 1, true),
	      "implicit first return did not initialize or finish");
	CHECK(rx.stage == V90_PHASE3_RETURN_RX_STAGE_IMPLICIT_END &&
	      !rx.allow_implicit_s_end && !v90_phase3_return_rx_is_complete(&rx),
	      "implicit first return exposed the wrong terminal state");
	saved = rx;
	memset(&handoff, 0x5a, sizeof(handoff));
	saved_handoff = handoff;
	point = channel_point(point, 1);
	CHECK(v90_phase3_return_rx_point(&rx, &point) ==
		      V90_PHASE3_RETURN_RX_INVALID_STATE &&
	      v90_phase3_return_rx_handoff(&rx, &handoff) ==
		      V90_PHASE3_RETURN_RX_INVALID_STATE &&
	      memcmp(&rx, &saved, sizeof(rx)) == 0 &&
	      memcmp(&handoff, &saved_handoff, sizeof(handoff)) == 0,
	      "implicit boundary consumed input or exposed an S-bar handoff");
	CHECK(v90_phase3_return_rx_restart(&rx) == V90_PHASE3_RETURN_RX_OK &&
	      v90_phase3_return_rx_is_valid(&rx) &&
	      !rx.allow_implicit_s_end &&
	      feed_implicit_return(&rx, 1, false) &&
	      rx.stage == V90_PHASE3_RETURN_RX_STAGE_S,
	      "restart did not restore strict second-return detection");
	return true;
}

static bool prepare_second_return(
	unsigned int quarter_turns, struct v90_phase3_return_rx *rx)
{
	struct v34_equalizer_handoff equalizer;

	return seed_handoff(quarter_turns, &equalizer) &&
	       v90_phase3_return_rx_init(rx, &equalizer, 0.05) ==
		       V90_PHASE3_RETURN_RX_OK &&
	       v90_phase3_return_rx_allow_implicit_s_end(rx) ==
		       V90_PHASE3_RETURN_RX_OK &&
	       feed_implicit_return(rx, quarter_turns, true) &&
	       v90_phase3_return_rx_restart(rx) ==
		       V90_PHASE3_RETURN_RX_OK &&
	       rx->second_return;
}

static bool feed_second_marker(
	struct v90_phase3_return_rx *rx, unsigned int quarter_turns,
	size_t s_symbols, unsigned int *s_events,
	unsigned int *s_bar_events)
{
	static const unsigned int post_rotations[] = {
		0, 2, 1, 3,
	};

	*s_events = 0;
	*s_bar_events = 0;
	for (size_t symbol = 0;
	     symbol < s_symbols + V34_PHASE3_S_BAR_SYMBOLS +
			      V34_EQUALIZER_DECISION_DELAY;
	     ++symbol) {
		struct qam_point point;
		enum v90_phase3_return_rx_result result;
		unsigned int rotation;

		if (symbol < s_symbols)
			rotation = (unsigned int)(symbol & 1u);
		else if (symbol < s_symbols + V34_PHASE3_S_BAR_SYMBOLS)
			rotation = 2u +
				(unsigned int)((symbol - s_symbols) & 1u);
		else
			rotation = post_rotations[
				(symbol - s_symbols -
				 V34_PHASE3_S_BAR_SYMBOLS) %
				ARRAY_LENGTH(post_rotations)];
		point = channel_point(
			v34_rotated_point_zero((int)rotation), quarter_turns);
		result = v90_phase3_return_rx_point(rx, &point);
		if (result == V90_PHASE3_RETURN_RX_S_DETECTED)
			(*s_events)++;
		else if (result == V90_PHASE3_RETURN_RX_S_BAR_DETECTED)
			(*s_bar_events)++;
		else if (result != V90_PHASE3_RETURN_RX_OK &&
			 result != V90_PHASE3_RETURN_RX_COMPLETE)
			return false;
	}
	return true;
}

static bool test_second_return_bounds(void)
{
	const unsigned int quarter_turns = 3;
	struct v90_phase3_return_rx rx = { 0 };
	unsigned int s_events = 0;
	unsigned int s_bar_events = 0;

	CHECK(prepare_second_return(quarter_turns, &rx) &&
	      feed_second_marker(
		      &rx, quarter_turns,
		      V90_PHASE3_RETURN_RX_MIN_SECOND_S_SYMBOLS - 32u,
		      &s_events, &s_bar_events) &&
	      s_events == 1 && s_bar_events == 0 &&
	      !v90_phase3_return_rx_is_complete(&rx),
	      "short second S was accepted: S=%u Sbar=%u stage=%d",
	      s_events, s_bar_events, rx.stage);
	CHECK(prepare_second_return(quarter_turns, &rx) &&
	      feed_second_marker(
		      &rx, quarter_turns,
		      V90_PHASE3_RETURN_RX_MIN_SECOND_S_SYMBOLS,
		      &s_events, &s_bar_events) &&
	      s_events == 1 && s_bar_events == 1 &&
	      v90_phase3_return_rx_is_complete(&rx),
	      "minimum second S was rejected: S=%u Sbar=%u stage=%d",
	      s_events, s_bar_events, rx.stage);
	CHECK(prepare_second_return(quarter_turns, &rx) &&
	      feed_second_marker(
		      &rx, quarter_turns,
		      V90_PHASE3_RETURN_RX_MAX_SECOND_S_SYMBOLS,
		      &s_events, &s_bar_events) &&
	      s_events == 1 && s_bar_events == 1 &&
	      v90_phase3_return_rx_is_complete(&rx),
	      "maximum second S was rejected: S=%u Sbar=%u stage=%d",
	      s_events, s_bar_events, rx.stage);
	CHECK(prepare_second_return(quarter_turns, &rx) &&
	      feed_second_marker(
		      &rx, quarter_turns,
		      V90_PHASE3_RETURN_RX_MAX_SECOND_S_SYMBOLS + 32u,
		      &s_events, &s_bar_events) &&
	      s_bar_events == 0 && !v90_phase3_return_rx_is_complete(&rx),
	      "overlong second S was accepted: S=%u Sbar=%u stage=%d",
	      s_events, s_bar_events, rx.stage);
	CHECK(feed_second_marker(
		      &rx, quarter_turns, V34_PHASE3_S_SYMBOLS,
		      &s_events, &s_bar_events) &&
	      s_events == 1 && s_bar_events == 1 &&
	      v90_phase3_return_rx_is_complete(&rx),
	      "overlong second S poisoned reacquisition: S=%u Sbar=%u stage=%d",
	      s_events, s_bar_events, rx.stage);
	return true;
}

static bool test_reacquisition_contract(void)
{
	const unsigned int quarter_turns = 2;
	struct v34_equalizer_handoff equalizer;
	struct v90_phase3_return_rx rx;
	struct v90_phase3_return_rx saved;
	struct qam_point point = channel_point(
		v34_rotated_point_zero(0), quarter_turns);
	struct qam_point invalid = { NAN, 0.0 };
	struct qam_point unscalable = { DBL_MAX, DBL_MAX };

	CHECK(seed_tracked_handoff(quarter_turns, &equalizer) &&
	      v90_phase3_return_rx_init(&rx, &equalizer, 0.05) ==
		      V90_PHASE3_RETURN_RX_OK,
	      "reacquisition receiver did not initialize");
	saved = rx;
	CHECK(v90_phase3_return_rx_reacquire_s(NULL, &point) ==
		      V90_PHASE3_RETURN_RX_INVALID_ARGUMENT &&
	      v90_phase3_return_rx_reacquire_s(&rx, NULL) ==
		      V90_PHASE3_RETURN_RX_INVALID_ARGUMENT &&
	      v90_phase3_return_rx_reacquire_s(&rx, &invalid) ==
		      V90_PHASE3_RETURN_RX_INVALID_ARGUMENT &&
	      v90_phase3_return_rx_reacquire_s(&rx, &point) ==
		      V90_PHASE3_RETURN_RX_INVALID_STATE &&
	      memcmp(&rx, &saved, sizeof(rx)) == 0,
	      "invalid or first-return reacquisition changed state");
	CHECK(v90_phase3_return_rx_allow_implicit_s_end(&rx) ==
		      V90_PHASE3_RETURN_RX_OK &&
	      feed_implicit_return(&rx, quarter_turns, true) &&
	      v90_phase3_return_rx_restart(&rx) ==
		      V90_PHASE3_RETURN_RX_OK &&
	      rx.second_return && rx.equalizer.carrier_tracking,
	      "reacquisition receiver did not reach the second return");
	saved = rx;
	CHECK(v90_phase3_return_rx_reacquire_s(&rx, &unscalable) ==
		      V90_PHASE3_RETURN_RX_NUMERIC_FAILURE &&
	      memcmp(&rx, &saved, sizeof(rx)) == 0,
	      "failed equalizer reacquisition was not transactional");
	CHECK(v90_phase3_return_rx_reacquire_s(&rx, &point) ==
		      V90_PHASE3_RETURN_RX_OK &&
	      v90_phase3_return_rx_is_valid(&rx) &&
	      rx.equalizer_reacquiring && rx.equalizer.fill == 0 &&
	      rx.stage == V90_PHASE3_RETURN_RX_STAGE_SEARCH_S &&
	      rx.confirmed_s_symbols == 0 &&
	      rx.buffered_onset_decisions == 0,
	      "valid S point did not begin bounded equalizer reacquisition");
	saved = rx;
	CHECK(v90_phase3_return_rx_reacquire_s(&rx, &point) ==
		      V90_PHASE3_RETURN_RX_INVALID_STATE &&
	      memcmp(&rx, &saved, sizeof(rx)) == 0,
	      "duplicate equalizer reacquisition reset its fill window");
	return true;
}

static bool test_validation(void)
{
	struct v34_equalizer_handoff equalizer;
	struct v90_phase3_return_rx rx;
	struct v90_phase3_return_rx saved;
	struct v90_phase3_return_handoff handoff;
	struct v90_phase3_return_handoff saved_handoff;
	struct qam_point invalid = { NAN, 0.0 };

	CHECK(seed_handoff(0, &equalizer),
	      "validation handoff did not initialize");
	memset(&rx, 0xa5, sizeof(rx));
	saved = rx;
	CHECK(v90_phase3_return_rx_init(&rx, &equalizer, NAN) ==
		      V90_PHASE3_RETURN_RX_INVALID_ARGUMENT &&
	      memcmp(&rx, &saved, sizeof(rx)) == 0,
	      "invalid return configuration changed state");
	CHECK(v90_phase3_return_rx_init(&rx, &equalizer, 0.05) ==
		      V90_PHASE3_RETURN_RX_OK,
	      "valid return receiver did not initialize");
	saved = rx;
	memset(&handoff, 0x5a, sizeof(handoff));
	saved_handoff = handoff;
	CHECK(v90_phase3_return_rx_point(NULL, &invalid) ==
		      V90_PHASE3_RETURN_RX_INVALID_ARGUMENT &&
	      v90_phase3_return_rx_point(&rx, NULL) ==
		      V90_PHASE3_RETURN_RX_INVALID_ARGUMENT &&
	      v90_phase3_return_rx_point(&rx, &invalid) ==
		      V90_PHASE3_RETURN_RX_INVALID_ARGUMENT &&
	      v90_phase3_return_rx_handoff(&rx, &handoff) ==
		      V90_PHASE3_RETURN_RX_INVALID_STATE &&
	      memcmp(&rx, &saved, sizeof(rx)) == 0 &&
	      memcmp(&handoff, &saved_handoff, sizeof(handoff)) == 0,
	      "rejected return operation changed state or output");
	rx.stage = (enum v90_phase3_return_rx_stage)99;
	CHECK(!v90_phase3_return_rx_is_valid(NULL) &&
	      !v90_phase3_return_rx_is_valid(&rx) &&
	      !v90_phase3_return_rx_is_complete(&rx) &&
	      !v90_phase3_return_handoff_is_valid(NULL) &&
	      strcmp(v90_phase3_return_rx_result_name(
		     V90_PHASE3_RETURN_RX_S_BAR_DETECTED),
		     "S-bar detected") == 0 &&
	      strcmp(v90_phase3_return_rx_result_name(
		     (enum v90_phase3_return_rx_result)99),
		     "unknown") == 0,
	      "return receiver validation or names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
		} tests[] = {
			{ "generated timelines", test_generated_timelines },
			{ "tracked onset confirmation", test_tracked_onset_confirmation },
			{ "implicit first-return end", test_implicit_first_return_end },
			{ "second-return bounds", test_second_return_bounds },
			{ "reacquisition contract", test_reacquisition_contract },
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
