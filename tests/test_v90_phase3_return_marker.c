// SPDX-License-Identifier: BSD-3-Clause

#include <v34_constellation.h>
#include <v90_phase3_analog_tx.h>
#include <v90_phase3_return_marker.h>
#include <v90_phase3_return_tx.h>

#include "test.h"

#include <string.h>

enum {
	JA_REPETITIONS = 4,
	TRN_SYMBOLS = V34_TRAINING_SEQUENCE_MIN_SYMBOLS + 97,
};

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

static uint8_t s_rotation(size_t symbol, unsigned int offset)
{
	return (uint8_t)((offset + 4u - (symbol & 1u)) & 3u);
}

static bool run_timeline(
	unsigned int offset, uint32_t finish_after, bool confirm_onset)
{
	struct v90_phase3_analog_tx ja;
	struct v90_phase3_return_tx tx;
	struct v90_phase3_return_marker rx;
	struct v34_phase3_s_detection detection;
	unsigned int s_events = 0;
	unsigned int s_bar_events = 0;
	unsigned int onset = confirm_onset ?
		V90_PHASE3_RETURN_RX_TRACKED_ONSET_SYMBOLS :
		V90_PHASE3_RETURN_RX_ONSET_SYMBOLS;

	CHECK(start_ja(&ja) &&
	      v90_phase3_return_tx_init(&tx) == V90_PHASE3_RETURN_TX_OK &&
	      v90_phase3_return_marker_init(&rx, confirm_onset) ==
		      V90_PHASE3_RETURN_RX_OK,
	      "phase %u return marker did not initialize", offset);
	for (size_t symbol = 0;
	     symbol < JA_REPETITIONS * V90_PHASE3_JA_DESCRIPTOR_SYMBOLS;
	     ++symbol) {
		struct qam_point point;
		uint8_t quarter;
		uint8_t rotation;

		CHECK(v90_phase3_analog_tx_next(&ja, &point) ==
			      V90_PHASE3_ANALOG_TX_OK &&
		      v34_training_constellation_slice(V34_CONSTELLATION_4_POINT,
			      &point, &quarter, &rotation) == V34_CONSTELLATION_OK &&
		      quarter == 0u &&
		      v90_phase3_return_marker_observe(
			      &rx, (uint8_t)((rotation + offset) & 3u)) ==
			      V90_PHASE3_RETURN_RX_OK,
		      "phase %u repeated Ja caused a false S event", offset);
	}
	/* A quiet hard slice is a constant rotation, not a marker. */
	for (size_t symbol = 0; symbol < 73u; ++symbol) {
		struct qam_point point;

		CHECK(v90_phase3_return_tx_next(&tx, &point) ==
			      V90_PHASE3_RETURN_TX_OK &&
		      point.in_phase == 0.0 && point.quadrature == 0.0 &&
		      v90_phase3_return_marker_observe(&rx, 0) ==
			      V90_PHASE3_RETURN_RX_OK,
		      "phase %u quiet decisions caused a false S event", offset);
	}
	CHECK(v90_phase3_return_tx_start_s(&tx) == V90_PHASE3_RETURN_TX_OK,
	      "phase %u could not start S", offset);
	for (size_t emitted = 0;
	     emitted < finish_after + V34_PHASE3_S_BAR_SYMBOLS + 1u &&
	     !v90_phase3_return_tx_is_complete(&tx); ++emitted) {
		struct qam_point point;
		enum v90_phase3_return_tx_stage stage = tx.stage;
		uint32_t s_symbol = tx.s_symbols;
		uint8_t quarter;
		uint8_t rotation;
		enum v90_phase3_return_rx_result result;

		if (stage == V90_PHASE3_RETURN_TX_S && s_symbol == finish_after)
			CHECK(v90_phase3_return_tx_finish_s(&tx) ==
				      V90_PHASE3_RETURN_TX_OK,
			      "phase %u could not finish S", offset);
		CHECK(v90_phase3_return_tx_next(&tx, &point) ==
			      V90_PHASE3_RETURN_TX_OK,
		      "phase %u return source stopped", offset);
		if (stage == V90_PHASE3_RETURN_TX_S &&
		    (s_symbol == 19u || s_symbol == 47u))
			point = v34_rotated_point_zero(2);
		CHECK(v34_training_constellation_slice(V34_CONSTELLATION_4_POINT,
			      &point, &quarter, &rotation) == V34_CONSTELLATION_OK &&
		      quarter == 0u,
		      "phase %u return point did not slice", offset);
		result = v90_phase3_return_marker_observe(
			&rx, (uint8_t)((rotation + offset) & 3u));
		if (result == V90_PHASE3_RETURN_RX_S_DETECTED) {
			s_events++;
			CHECK(tx.s_symbols == onset,
			      "phase %u onset moved to symbol %u", offset, tx.s_symbols);
		} else if (result == V90_PHASE3_RETURN_RX_S_BAR_DETECTED) {
			s_bar_events++;
			CHECK(v90_phase3_return_tx_is_complete(&tx),
			      "phase %u S-bar consumed a post-marker decision", offset);
		} else {
			CHECK(result == V90_PHASE3_RETURN_RX_OK,
			      "phase %u return marker returned %d", offset, result);
		}
	}
	CHECK(v90_phase3_return_tx_is_complete(&tx) &&
	      s_events == 1u && s_bar_events == 1u &&
	      tx.s_symbols == finish_after + (finish_after & 1u) &&
	      v90_phase3_return_marker_get_detection(&rx, &detection) &&
	      detection.clockwise_quarter_turn_offset == offset &&
	      rx.marker.post_marker_symbols == 0u,
	      "phase %u did not retain the exact marker boundary", offset);
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
	struct v90_phase3_return_marker rx;

	CHECK(v90_phase3_return_marker_init(&rx, true) ==
		      V90_PHASE3_RETURN_RX_OK,
	      "tracked marker did not initialize");
	for (size_t symbol = 0;
	     symbol < V90_PHASE3_RETURN_RX_TRACKED_ONSET_SYMBOLS; ++symbol)
		CHECK(v90_phase3_return_marker_observe(&rx, 2) ==
			      V90_PHASE3_RETURN_RX_OK,
		      "tracked pre-S fill reported an event at %zu", symbol);
	for (size_t symbol = 0; symbol < V90_PHASE3_RETURN_RX_ONSET_SYMBOLS;
	     ++symbol)
		CHECK(v90_phase3_return_marker_observe(&rx, s_rotation(symbol, 0)) ==
			      V90_PHASE3_RETURN_RX_OK,
		      "tracked tentative S reported an early event at %zu", symbol);
	CHECK(rx.stage == V90_PHASE3_RETURN_RX_STAGE_SEARCH_S &&
	      rx.confirmed_s_symbols == V90_PHASE3_RETURN_RX_ONSET_SYMBOLS,
	      "tentative S did not await its extra confirmation");
	for (size_t symbol = 0;
	     symbol < V90_PHASE3_RETURN_RX_ONSET_SYMBOLS + 3u; ++symbol)
		CHECK(v90_phase3_return_marker_observe(&rx, 2) ==
			      V90_PHASE3_RETURN_RX_OK &&
		      rx.stage == V90_PHASE3_RETURN_RX_STAGE_SEARCH_S &&
		      rx.confirmed_s_symbols == 0u,
		      "false tracked S survived its failed confirmation");
	for (size_t symbol = 0;
	     symbol < V90_PHASE3_RETURN_RX_TRACKED_ONSET_SYMBOLS; ++symbol) {
		enum v90_phase3_return_rx_result expected =
			symbol + 1u == V90_PHASE3_RETURN_RX_TRACKED_ONSET_SYMBOLS ?
				V90_PHASE3_RETURN_RX_S_DETECTED : V90_PHASE3_RETURN_RX_OK;

		CHECK(v90_phase3_return_marker_observe(&rx, s_rotation(symbol, 0)) ==
			      expected,
		      "confirmed tracked S changed its onset at %zu", symbol);
	}
	CHECK(rx.stage == V90_PHASE3_RETURN_RX_STAGE_S &&
	      rx.confirmed_s_symbols == V90_PHASE3_RETURN_RX_TRACKED_ONSET_SYMBOLS,
	      "tracked S did not retain its confirmed onset");
	return true;
}

static bool feed_first_return(
	struct v90_phase3_return_marker *rx, unsigned int offset,
	bool distinct_s_bar)
{
	for (size_t symbol = 0; symbol < 192u; ++symbol) {
		enum v90_phase3_return_rx_result expected =
			symbol + 1u == V90_PHASE3_RETURN_RX_ONSET_SYMBOLS ?
				V90_PHASE3_RETURN_RX_S_DETECTED : V90_PHASE3_RETURN_RX_OK;

		CHECK(v90_phase3_return_marker_observe(rx, s_rotation(symbol, offset)) ==
			      expected,
		      "first-return S changed at symbol %zu", symbol);
	}
	for (size_t symbol = 0; symbol < V34_PHASE3_S_BAR_SYMBOLS; ++symbol) {
		uint8_t rotation = distinct_s_bar ?
			(uint8_t)((s_rotation(symbol, offset) + 2u) & 3u) :
			(uint8_t)((offset + 1u) & 3u);
		enum v90_phase3_return_rx_result expected = distinct_s_bar &&
			symbol + 1u == V34_PHASE3_S_BAR_SYMBOLS ?
				V90_PHASE3_RETURN_RX_S_BAR_DETECTED : V90_PHASE3_RETURN_RX_OK;

		CHECK(v90_phase3_return_marker_observe(rx, rotation) == expected,
		      "first-return boundary changed at symbol %zu", symbol);
	}
	return true;
}

static bool prepare_second_return(
	unsigned int offset, struct v90_phase3_return_marker *rx)
{
	return v90_phase3_return_marker_init(rx, false) == V90_PHASE3_RETURN_RX_OK &&
	       feed_first_return(rx, offset, true) &&
	       v90_phase3_return_marker_restart(rx) == V90_PHASE3_RETURN_RX_OK &&
	       rx->second_return;
}

static bool test_missing_s_bar(void)
{
	struct v90_phase3_return_marker rx;
	struct v90_phase3_return_marker saved;

	CHECK(v90_phase3_return_marker_init(&rx, false) == V90_PHASE3_RETURN_RX_OK &&
	      feed_first_return(&rx, 1, false) &&
	      !v90_phase3_return_marker_is_complete(&rx),
	      "first return accepted an arbitrary S ending");
	saved = rx;
	CHECK(v90_phase3_return_marker_restart(&rx) ==
		      V90_PHASE3_RETURN_RX_INVALID_STATE &&
	      memcmp(&rx, &saved, sizeof(rx)) == 0,
	      "first return restarted before its complete S-bar");
	CHECK(prepare_second_return(1, &rx) &&
	      feed_first_return(&rx, 1, false) &&
	      rx.stage == V90_PHASE3_RETURN_RX_STAGE_S &&
	      !v90_phase3_return_marker_is_complete(&rx),
	      "second return accepted an arbitrary S ending");
	return true;
}

static bool test_legacy_return_phase_change(void)
{
	struct v90_phase3_return_marker rx;
	struct v90_phase3_return_marker saved;
	struct v34_phase3_s_detection detection;

	CHECK(v90_phase3_return_marker_init(&rx, false) == V90_PHASE3_RETURN_RX_OK &&
	      v90_phase3_return_marker_allow_implicit_s_end(&rx) ==
		      V90_PHASE3_RETURN_RX_OK,
	      "legacy first-return receiver did not initialize");
	for (unsigned symbol = 0; symbol < 320u; ++symbol) {
		unsigned offset = symbol < 160u ? 0u : 1u;
		enum v90_phase3_return_rx_result expected =
			symbol + 1u == V90_PHASE3_RETURN_RX_ONSET_SYMBOLS ?
				V90_PHASE3_RETURN_RX_S_DETECTED : V90_PHASE3_RETURN_RX_OK;

		CHECK(v90_phase3_return_marker_observe(
			      &rx, s_rotation(symbol, offset)) == expected,
		      "carrier phase change ended S at symbol %u", symbol);
	}
	for (unsigned symbol = 0; symbol < V34_PHASE3_S_BAR_SYMBOLS; ++symbol)
		CHECK(v90_phase3_return_marker_observe(
			      &rx, (uint8_t)((s_rotation(symbol, 1u) + 2u) & 3u)) ==
			      (symbol + 1u == V34_PHASE3_S_BAR_SYMBOLS ?
			       V90_PHASE3_RETURN_RX_S_BAR_DETECTED : V90_PHASE3_RETURN_RX_OK),
		      "carrier phase change displaced S-bar at symbol %u", symbol);
	CHECK(v90_phase3_return_marker_get_detection(&rx, &detection) &&
	      detection.clockwise_quarter_turn_offset == 1u &&
	      v90_phase3_return_marker_restart(&rx) == V90_PHASE3_RETURN_RX_OK &&
	      rx.second_return && !rx.allow_implicit_s_end,
	      "legacy permission changed the distinct second-return contract");
	saved = rx;
	CHECK(v90_phase3_return_marker_allow_implicit_s_end(&rx) ==
		      V90_PHASE3_RETURN_RX_INVALID_STATE &&
	      memcmp(&rx, &saved, sizeof(rx)) == 0,
	      "legacy permission was enabled for the second return");
	rx.allow_implicit_s_end = true;
	CHECK(!v90_phase3_return_marker_is_valid(&rx),
	      "second return accepted legacy compatibility state");
	return true;
}

static bool test_legacy_return_nonperiodic_end(void)
{
	struct v90_phase3_return_marker rx;
	struct v90_phase3_return_marker saved;
	struct v34_phase3_s_detection detection;

	CHECK(v90_phase3_return_marker_init(&rx, false) == V90_PHASE3_RETURN_RX_OK &&
	      v90_phase3_return_marker_allow_implicit_s_end(&rx) == V90_PHASE3_RETURN_RX_OK,
	      "legacy nonperiodic receiver did not initialize");
	for (unsigned symbol = 0; symbol < V34_PHASE3_S_SYMBOLS; ++symbol)
		CHECK(v90_phase3_return_marker_observe(&rx, s_rotation(symbol, 0u)) ==
			      (symbol + 1u == V90_PHASE3_RETURN_RX_ONSET_SYMBOLS ?
			       V90_PHASE3_RETURN_RX_S_DETECTED : V90_PHASE3_RETURN_RX_OK),
		      "legacy S changed at symbol %u", symbol);
	for (unsigned symbol = 0; symbol < V90_PHASE3_RETURN_RX_EXIT_SYMBOLS; ++symbol)
		CHECK(v90_phase3_return_marker_observe(&rx, 1u) ==
			      (symbol + 1u == V90_PHASE3_RETURN_RX_EXIT_SYMBOLS ?
			       V90_PHASE3_RETURN_RX_S_ENDED : V90_PHASE3_RETURN_RX_OK),
		      "legacy nonperiodic exit changed at symbol %u", symbol);
	saved = rx;
	CHECK(!v90_phase3_return_marker_get_detection(&rx, &detection) &&
	      v90_phase3_return_marker_observe(&rx, 0u) == V90_PHASE3_RETURN_RX_INVALID_STATE &&
	      memcmp(&rx, &saved, sizeof(rx)) == 0 &&
	      v90_phase3_return_marker_restart(&rx) == V90_PHASE3_RETURN_RX_OK &&
	      rx.second_return && !rx.allow_implicit_s_end &&
	      feed_first_return(&rx, 0u, false) &&
	      !v90_phase3_return_marker_is_complete(&rx),
	      "legacy first end relaxed the second S-bar or published a handoff");
	return true;
}

static bool feed_second_marker(
	struct v90_phase3_return_marker *rx, unsigned int offset,
	size_t s_symbols, unsigned int *s_events, unsigned int *s_bar_events)
{
	*s_events = 0;
	*s_bar_events = 0;
	for (size_t symbol = 0; symbol < s_symbols + V34_PHASE3_S_BAR_SYMBOLS;
	     ++symbol) {
		uint8_t rotation = symbol < s_symbols ? s_rotation(symbol, offset) :
			(uint8_t)((s_rotation(symbol - s_symbols, offset) + 2u) & 3u);
		enum v90_phase3_return_rx_result result =
			v90_phase3_return_marker_observe(rx, rotation);

		if (result == V90_PHASE3_RETURN_RX_S_DETECTED)
			(*s_events)++;
		else if (result == V90_PHASE3_RETURN_RX_S_BAR_DETECTED)
			(*s_bar_events)++;
		else if (result != V90_PHASE3_RETURN_RX_OK)
			return false;
	}
	return true;
}

static bool test_second_return_bounds(void)
{
	static const struct {
		size_t s_symbols;
		bool accepted;
	} cases[] = {
		{ V90_PHASE3_RETURN_RX_MIN_SECOND_S_SYMBOLS - 32u, false },
		{ V90_PHASE3_RETURN_RX_MIN_SECOND_S_SYMBOLS, true },
		{ V90_PHASE3_RETURN_RX_MAX_SECOND_S_SYMBOLS, true },
		{ V90_PHASE3_RETURN_RX_MAX_SECOND_S_SYMBOLS + 32u, false },
	};
	const unsigned int offset = 3;
	struct v90_phase3_return_marker rx;
	unsigned int s_events = 0;
	unsigned int s_bar_events = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index)
		CHECK(prepare_second_return(offset, &rx) &&
		      feed_second_marker(&rx, offset, cases[index].s_symbols,
			      &s_events, &s_bar_events) &&
		      (index == ARRAY_LENGTH(cases) - 1u || s_events == 1u) &&
		      s_bar_events == (cases[index].accepted ? 1u : 0u) &&
		      v90_phase3_return_marker_is_complete(&rx) == cases[index].accepted,
		      "second S length %zu returned S=%u Sbar=%u stage=%d",
		      cases[index].s_symbols, s_events, s_bar_events, rx.stage);
	CHECK(feed_second_marker(&rx, offset, V34_PHASE3_S_SYMBOLS,
		      &s_events, &s_bar_events) &&
	      s_events == 1u && s_bar_events == 1u &&
	      v90_phase3_return_marker_is_complete(&rx),
	      "overlong second S poisoned reacquisition");
	return true;
}

static bool run_marker_timeline(bool confirm_onset)
{
	struct v90_phase3_return_marker rx;
	struct v90_phase3_return_marker saved;
	struct v34_phase3_s_detection detection;
	const unsigned int onset = confirm_onset ?
		V90_PHASE3_RETURN_RX_TRACKED_ONSET_SYMBOLS :
		V90_PHASE3_RETURN_RX_ONSET_SYMBOLS;

	CHECK(v90_phase3_return_marker_init(&rx, confirm_onset) ==
		      V90_PHASE3_RETURN_RX_OK,
	      "return marker did not initialize");
	for (unsigned int epoch = 0; epoch < 2u; ++epoch) {
		for (unsigned int symbol = 0; symbol < V34_PHASE3_S_SYMBOLS;
		     ++symbol) {
			uint8_t rotation = (uint8_t)((4u - (symbol & 1u)) & 3u);
			enum v90_phase3_return_rx_result expected =
				symbol + 1u == onset ? V90_PHASE3_RETURN_RX_S_DETECTED :
				V90_PHASE3_RETURN_RX_OK;

			CHECK(v90_phase3_return_marker_observe(&rx, rotation) ==
				      expected,
			      "epoch %u onset changed at symbol %u", epoch, symbol);
		}
		for (unsigned int symbol = 0;
		     symbol < V34_PHASE3_S_BAR_SYMBOLS; ++symbol) {
			uint8_t rotation = (uint8_t)((6u - (symbol & 1u)) & 3u);
			enum v90_phase3_return_rx_result expected =
				symbol + 1u != V34_PHASE3_S_BAR_SYMBOLS ?
					V90_PHASE3_RETURN_RX_OK :
					V90_PHASE3_RETURN_RX_S_BAR_DETECTED;

			CHECK(v90_phase3_return_marker_observe(&rx, rotation) ==
				      expected,
			      "epoch %u return boundary changed at symbol %u", epoch, symbol);
		}
		saved = rx;
		CHECK(v90_phase3_return_marker_observe(&rx, 0) ==
			      V90_PHASE3_RETURN_RX_COMPLETE &&
		      memcmp(&rx, &saved, sizeof(rx)) == 0,
		      "epoch %u terminal marker consumed the next decision", epoch);
		CHECK(v90_phase3_return_marker_get_detection(&rx, &detection),
		      "epoch %u marker handoff lost S-bar", epoch);
		if (epoch == 0u)
			CHECK(v90_phase3_return_marker_restart(&rx) ==
				      V90_PHASE3_RETURN_RX_OK && rx.second_return &&
			      rx.confirm_onset == confirm_onset &&
			      rx.confirmed_s_symbols == 0,
			      "first return did not restart with strict second semantics");
	}
	CHECK(detection.clockwise_quarter_turn_offset == 0 &&
	      rx.marker.post_marker_symbols == 0 &&
	      v90_phase3_return_marker_is_complete(&rx),
	      "second return did not retain the exact first CPt decision boundary");
	return true;
}

static bool test_marker_semantics(void)
{
	struct v90_phase3_return_marker rx;
	struct v90_phase3_return_marker saved;
	struct v34_phase3_s_detection detection = { 2, 1, 1 };
	struct v34_phase3_s_detection saved_detection = detection;

	CHECK(run_marker_timeline(false) && run_marker_timeline(true),
	      "return marker timelines failed");
	CHECK(v90_phase3_return_marker_init(&rx, true) ==
		      V90_PHASE3_RETURN_RX_OK,
	      "marker validation receiver did not initialize");
	saved = rx;
	CHECK(v90_phase3_return_marker_init(NULL, false) ==
		      V90_PHASE3_RETURN_RX_INVALID_ARGUMENT &&
	      v90_phase3_return_marker_observe(NULL, 0) ==
		      V90_PHASE3_RETURN_RX_INVALID_ARGUMENT &&
	      v90_phase3_return_marker_observe(&rx, 4) ==
		      V90_PHASE3_RETURN_RX_INVALID_ARGUMENT &&
	      v90_phase3_return_marker_restart(&rx) ==
		      V90_PHASE3_RETURN_RX_INVALID_STATE &&
	      !v90_phase3_return_marker_get_detection(&rx, &detection) &&
	      memcmp(&rx, &saved, sizeof(rx)) == 0 &&
	      memcmp(&detection, &saved_detection, sizeof(detection)) == 0,
	      "invalid marker call changed state or output");
	rx.stage = (enum v90_phase3_return_rx_stage)99;
	saved = rx;
	CHECK(!v90_phase3_return_marker_is_valid(NULL) &&
	      !v90_phase3_return_marker_is_complete(NULL) &&
	      !v90_phase3_return_marker_is_valid(&rx) &&
	      !v90_phase3_return_marker_is_complete(&rx) &&
	      v90_phase3_return_marker_observe(&rx, 0) ==
		      V90_PHASE3_RETURN_RX_INVALID_ARGUMENT &&
	      memcmp(&rx, &saved, sizeof(rx)) == 0,
	      "invalid marker stage was accepted or changed");
	return true;
}

static bool test_marker_confirmation_tail(void)
{
	struct v90_phase3_return_marker rx;
	struct v90_phase3_return_marker saved;
	struct v34_phase3_s_detection detection;

	CHECK(v90_phase3_return_marker_init(&rx, true) == V90_PHASE3_RETURN_RX_OK,
	      "confirmation-tail marker did not initialize");
	for (unsigned int symbol = 0;
	     symbol < V34_PHASE3_S_SEQUENCE_SYMBOLS +
		     V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS; ++symbol) {
		uint8_t rotation = (uint8_t)((4u - (symbol & 1u)) & 3u);
		enum v90_phase3_return_rx_result expected = V90_PHASE3_RETURN_RX_OK;

		if (symbol >= V34_PHASE3_S_SEQUENCE_SYMBOLS)
			rotation = (uint8_t)(symbol - V34_PHASE3_S_SEQUENCE_SYMBOLS);
		else if (symbol >= V34_PHASE3_S_SYMBOLS) {
			rotation = (uint8_t)((rotation + 2u) & 3u);
			if (symbol < V34_PHASE3_S_SYMBOLS +
				     V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS)
				rotation = (uint8_t)((rotation + 1u) & 3u);
		}
		if (symbol + 1u == V90_PHASE3_RETURN_RX_TRACKED_ONSET_SYMBOLS)
			expected = V90_PHASE3_RETURN_RX_S_DETECTED;
		if (symbol + 1u == V34_PHASE3_S_SEQUENCE_SYMBOLS +
				   V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS)
			expected = V90_PHASE3_RETURN_RX_S_BAR_DETECTED;
		CHECK(v90_phase3_return_marker_observe(&rx, rotation) == expected,
		      "confirmation-tail ownership changed at symbol %u", symbol);
	}
	CHECK(v90_phase3_return_marker_get_detection(&rx, &detection) &&
	      detection.clockwise_quarter_turn_offset == 0u &&
	      detection.s_errors == 0u &&
	      detection.s_bar_errors == V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS &&
	      rx.marker.post_marker_symbols == V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS,
	      "damaged S-bar did not retain its bounded post-marker tail");
	saved = rx;
	CHECK(!v90_phase3_return_marker_get_detection(&rx, &rx.marker.detection) &&
	      memcmp(&rx, &saved, sizeof(rx)) == 0,
	      "marker handoff accepted a state-aliasing destination");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated marker timelines", test_generated_timelines },
		{ "tracked onset confirmation", test_tracked_onset_confirmation },
		{ "missing S-bar", test_missing_s_bar },
		{ "legacy return carrier phase", test_legacy_return_phase_change },
		{ "legacy nonperiodic return", test_legacy_return_nonperiodic_end },
		{ "second-return bounds", test_second_return_bounds },
		{ "marker semantics", test_marker_semantics },
		{ "marker confirmation tail", test_marker_confirmation_tail },
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
