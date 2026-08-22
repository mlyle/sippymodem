// SPDX-License-Identifier: BSD-3-Clause

#include <v34_constellation.h>
#include <v34_phase3_s_receiver.h>

#include "test.h"

#include <string.h>

static bool generate_marker(uint8_t rotations[V34_PHASE3_S_SEQUENCE_SYMBOLS])
{
	struct v34_phase3_s_sequence sequence;

	if (v34_phase3_s_sequence_init(&sequence) !=
	    V34_PHASE3_S_SEQUENCE_OK)
		return false;
	for (size_t symbol = 0;
	     symbol < V34_PHASE3_S_SEQUENCE_SYMBOLS; ++symbol) {
		struct qam_point point;
		enum v34_phase3_s_section section;
		uint8_t quarter_point;

		if (v34_phase3_s_sequence_next(
			    &sequence, &point, &section) !=
			    V34_PHASE3_S_SEQUENCE_OK ||
		    v34_training_constellation_slice(
			    V34_CONSTELLATION_4_POINT, &point, &quarter_point,
			    &rotations[symbol]) != V34_CONSTELLATION_OK ||
		    quarter_point != 0)
			return false;
	}
	return v34_phase3_s_sequence_is_complete(&sequence);
}

static bool receive_marker(
	struct v34_phase3_s_receiver *receiver,
	const uint8_t rotations[V34_PHASE3_S_SEQUENCE_SYMBOLS],
	uint8_t offset)
{
	for (size_t symbol = 0;
	     symbol < V34_PHASE3_S_SEQUENCE_SYMBOLS; ++symbol) {
		enum v34_phase3_s_receiver_result result =
			v34_phase3_s_receiver_receive(
				receiver, 0,
				(uint8_t)((rotations[symbol] + offset) & 3u));

		if (result !=
		    (symbol + 1u == V34_PHASE3_S_SEQUENCE_SYMBOLS ?
			     V34_PHASE3_S_RECEIVER_DETECTED :
			     V34_PHASE3_S_RECEIVER_OK))
			return false;
	}
	return true;
}

static bool test_generated_rotations(void)
{
	uint8_t rotations[V34_PHASE3_S_SEQUENCE_SYMBOLS];

	CHECK(generate_marker(rotations), "S/S-bar marker did not generate");
	for (uint8_t offset = 0; offset < 4u; ++offset) {
		struct v34_phase3_s_receiver receiver;
		struct v34_phase3_s_detection detection = { 0xff, 0xff, 0xff };

		CHECK(v34_phase3_s_receiver_init(&receiver) ==
			      V34_PHASE3_S_RECEIVER_OK &&
		      receive_marker(&receiver, rotations, offset) &&
		      v34_phase3_s_receiver_is_valid(&receiver) &&
		      v34_phase3_s_receiver_is_complete(&receiver) &&
		      v34_phase3_s_receiver_get_detection(
			      &receiver, &detection) &&
		      detection.clockwise_quarter_turn_offset == offset &&
		      detection.s_errors == 0 &&
		      detection.s_bar_errors == 0,
		      "generated marker offset %u was not detected exactly", offset);
	}
	return true;
}

static bool test_sliding_window_and_tolerance(void)
{
	uint8_t rotations[V34_PHASE3_S_SEQUENCE_SYMBOLS];
	struct v34_phase3_s_receiver receiver;
	struct v34_phase3_s_detection detection;
	uint32_t noise = UINT32_C(0x243f6a88);

	CHECK(generate_marker(rotations) &&
	      v34_phase3_s_receiver_init(&receiver) ==
		      V34_PHASE3_S_RECEIVER_OK,
	      "sliding marker fixture did not initialize");
	for (size_t symbol = 0; symbol < 197; ++symbol) {
		noise ^= noise << 13;
		noise ^= noise >> 17;
		noise ^= noise << 5;
		CHECK(v34_phase3_s_receiver_receive(
			      &receiver, (uint8_t)((noise >> 2) & 3u),
			      (uint8_t)(noise & 3u)) ==
			      V34_PHASE3_S_RECEIVER_OK,
		      "noise prefix falsely detected at symbol %zu", symbol);
	}
	for (unsigned int error = 0;
	     error < V34_PHASE3_S_RECEIVER_MAX_S_ERRORS; ++error) {
		size_t symbol = 7u + error * 13u;

		rotations[symbol] = (uint8_t)((rotations[symbol] + 1u) & 3u);
	}
	for (unsigned int error = 0;
	     error < V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS; ++error) {
		size_t symbol = V34_PHASE3_S_SYMBOLS + 4u + error * 3u;

		rotations[symbol] = (uint8_t)((rotations[symbol] + 1u) & 3u);
	}
	CHECK(receive_marker(&receiver, rotations, 3) &&
	      v34_phase3_s_receiver_get_detection(&receiver, &detection) &&
	      detection.clockwise_quarter_turn_offset == 3 &&
	      detection.s_errors == V34_PHASE3_S_RECEIVER_MAX_S_ERRORS &&
	      detection.s_bar_errors ==
		      V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS,
	      "sliding marker did not retain the bounded error counts");
	return true;
}

static bool marker_with_errors_is_rejected(bool s_bar)
{
	uint8_t rotations[V34_PHASE3_S_SEQUENCE_SYMBOLS];
	struct v34_phase3_s_receiver receiver;
	unsigned int errors = s_bar ?
			      V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS + 1u :
			      V34_PHASE3_S_RECEIVER_MAX_S_ERRORS + 1u;
	size_t first = s_bar ? V34_PHASE3_S_SYMBOLS + 1u : 0u;

	if (!generate_marker(rotations) ||
	    v34_phase3_s_receiver_init(&receiver) !=
		    V34_PHASE3_S_RECEIVER_OK)
		return false;
	for (unsigned int error = 0; error < errors; ++error) {
		size_t symbol = first + error * (s_bar ? 2u : 11u);

		rotations[symbol] = (uint8_t)((rotations[symbol] + 1u) & 3u);
	}
	for (size_t symbol = 0;
	     symbol < V34_PHASE3_S_SEQUENCE_SYMBOLS; ++symbol)
		if (v34_phase3_s_receiver_receive(
			    &receiver, 0, rotations[symbol]) !=
		    V34_PHASE3_S_RECEIVER_OK)
			return false;
	return !v34_phase3_s_receiver_is_complete(&receiver);
}

static bool test_rejection_thresholds(void)
{
	uint8_t rotations[V34_PHASE3_S_SEQUENCE_SYMBOLS];
	struct v34_phase3_s_receiver receiver;

	CHECK(marker_with_errors_is_rejected(false) &&
	      marker_with_errors_is_rejected(true),
	      "marker beyond an error threshold was accepted");
	CHECK(generate_marker(rotations) &&
	      v34_phase3_s_receiver_init(&receiver) ==
		      V34_PHASE3_S_RECEIVER_OK,
	      "constant-S rejection fixture did not initialize");
	for (size_t symbol = 0;
	     symbol < V34_PHASE3_S_SEQUENCE_SYMBOLS; ++symbol) {
		uint8_t rotation =
			rotations[symbol % V34_PHASE3_S_SYMBOLS];

		CHECK(v34_phase3_s_receiver_receive(&receiver, 0, rotation) ==
			      V34_PHASE3_S_RECEIVER_OK,
		      "unbarred S falsely completed at symbol %zu", symbol);
	}
	CHECK(!v34_phase3_s_receiver_is_complete(&receiver),
	      "unbarred S was accepted as a complete marker");
	return true;
}

static bool test_point_zero_requirement(void)
{
	uint8_t rotations[V34_PHASE3_S_SEQUENCE_SYMBOLS];
	struct v34_phase3_s_receiver receiver;
	struct v34_phase3_s_detection detection;

	CHECK(generate_marker(rotations) &&
	      v34_phase3_s_receiver_init(&receiver) ==
		      V34_PHASE3_S_RECEIVER_OK,
	      "point-zero fixture did not initialize");
	for (size_t symbol = 0;
	     symbol < V34_PHASE3_S_SEQUENCE_SYMBOLS; ++symbol) {
		uint8_t quarter_point = symbol == 37u ? 1u : 0u;
		enum v34_phase3_s_receiver_result expected =
			symbol + 1u == V34_PHASE3_S_SEQUENCE_SYMBOLS ?
				V34_PHASE3_S_RECEIVER_DETECTED :
				V34_PHASE3_S_RECEIVER_OK;

		CHECK(v34_phase3_s_receiver_receive(
			      &receiver, quarter_point, rotations[symbol]) ==
			      expected,
		      "point-zero marker stopped at symbol %zu", symbol);
	}
	CHECK(v34_phase3_s_receiver_get_detection(&receiver, &detection) &&
	      detection.s_errors == 1 && detection.s_bar_errors == 0,
	      "nonzero quarter point was not counted as an S error");
	return true;
}

static bool test_exact_transition_anchor(void)
{
	static const size_t ANCHORS[] = {
		V34_PHASE3_S_SYMBOLS - 2u,
		V34_PHASE3_S_SYMBOLS - 1u,
		V34_PHASE3_S_SYMBOLS,
		V34_PHASE3_S_SYMBOLS + 1u,
	};
	uint8_t generated[V34_PHASE3_S_SEQUENCE_SYMBOLS];
	struct v34_phase3_s_receiver receiver;

	CHECK(generate_marker(generated),
	      "transition-anchor marker did not generate");
	CHECK(v34_phase3_s_receiver_init(&receiver) ==
		      V34_PHASE3_S_RECEIVER_OK &&
	      v34_phase3_s_receiver_receive(
		      &receiver, 0, generated[0]) ==
		      V34_PHASE3_S_RECEIVER_OK &&
	      v34_phase3_s_receiver_receive(
		      &receiver, 0, generated[1]) ==
		      V34_PHASE3_S_RECEIVER_OK &&
	      receive_marker(&receiver, generated, 0),
	      "S-like prefix shifted the exact transition boundary");
	for (size_t anchor = 0; anchor < ARRAY_LENGTH(ANCHORS); ++anchor) {
		uint8_t rotations[V34_PHASE3_S_SEQUENCE_SYMBOLS];

		memcpy(rotations, generated, sizeof(rotations));
		rotations[ANCHORS[anchor]] =
			(uint8_t)((rotations[ANCHORS[anchor]] + 1u) & 3u);
		CHECK(v34_phase3_s_receiver_init(&receiver) ==
			      V34_PHASE3_S_RECEIVER_OK,
		      "transition-anchor receiver did not initialize");
		for (size_t symbol = 0;
		     symbol < V34_PHASE3_S_SEQUENCE_SYMBOLS; ++symbol)
			CHECK(v34_phase3_s_receiver_receive(
				      &receiver, 0, rotations[symbol]) ==
				      V34_PHASE3_S_RECEIVER_OK,
			      "corrupt anchor %zu detected at symbol %zu",
			      ANCHORS[anchor], symbol);
		CHECK(!v34_phase3_s_receiver_is_complete(&receiver),
		      "corrupt transition anchor %zu was accepted",
		      ANCHORS[anchor]);
	}
	return true;
}

static bool test_transition_error_confirmation(void)
{
	uint8_t rotations[V34_PHASE3_S_SEQUENCE_SYMBOLS];
	struct v34_phase3_s_receiver receiver;
	struct v34_phase3_s_detection detection;

	CHECK(generate_marker(rotations),
	      "transition-error marker did not generate");
	for (size_t error = 0;
	     error < V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS; ++error) {
		size_t symbol = V34_PHASE3_S_SYMBOLS + error;

		rotations[symbol] =
			(uint8_t)((rotations[symbol] + 1u) & 3u);
	}
	CHECK(v34_phase3_s_receiver_init(&receiver) ==
		      V34_PHASE3_S_RECEIVER_OK,
	      "transition-error receiver did not initialize");
	for (size_t symbol = 0;
	     symbol < V34_PHASE3_S_SEQUENCE_SYMBOLS; ++symbol)
		CHECK(v34_phase3_s_receiver_receive(
			      &receiver, 0, rotations[symbol]) ==
			      V34_PHASE3_S_RECEIVER_OK,
		      "transition-error marker completed early at %zu", symbol);
	for (unsigned int confirmation = 0;
	     confirmation < V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS;
	     ++confirmation) {
		enum v34_phase3_s_receiver_result expected =
			confirmation + 1u ==
				V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS ?
				V34_PHASE3_S_RECEIVER_DETECTED :
				V34_PHASE3_S_RECEIVER_OK;

		CHECK(v34_phase3_s_receiver_receive(
			      &receiver, 1, (uint8_t)confirmation) == expected,
		      "transition-error confirmation %u had wrong result",
		      confirmation);
	}
	CHECK(v34_phase3_s_receiver_get_detection(&receiver, &detection) &&
	      detection.s_errors == 0 &&
	      detection.s_bar_errors ==
		      V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS &&
	      receiver.post_marker_symbols ==
		      V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS,
	      "transition-error counts were not retained");
	return true;
}

static bool receive_suffix(
	struct v34_phase3_s_receiver *receiver,
	const uint8_t rotations[V34_PHASE3_S_SEQUENCE_SYMBOLS],
	uint8_t offset, uint8_t s_symbols, unsigned int s_errors)
{
	size_t first = V34_PHASE3_S_SYMBOLS - s_symbols;
	size_t count = s_symbols + V34_PHASE3_S_BAR_SYMBOLS;

	for (size_t local = 0; local < count; ++local) {
		size_t symbol = first + local;
		uint8_t rotation =
			(uint8_t)((rotations[symbol] + offset) & 3u);
		enum v34_phase3_s_receiver_result expected =
			local + 1u == count ? V34_PHASE3_S_RECEIVER_DETECTED :
					      V34_PHASE3_S_RECEIVER_OK;

		if (local < s_symbols - 2u && local < s_errors)
			rotation = (uint8_t)((rotation + 1u) & 3u);
		if (v34_phase3_s_receiver_receive(
			    receiver, 0, rotation) != expected)
			return false;
	}
	return true;
}

static bool test_suffix_acquisition(void)
{
	uint8_t rotations[V34_PHASE3_S_SEQUENCE_SYMBOLS];
	struct v34_phase3_s_receiver receiver;
	struct v34_phase3_s_receiver saved;
	struct v34_phase3_s_detection detection;
	const uint8_t suffix = V34_PHASE3_S_RECEIVER_MIN_S_SUFFIX_SYMBOLS;

	CHECK(generate_marker(rotations),
	      "suffix marker did not generate");
	for (uint8_t offset = 0; offset < 4u; ++offset) {
		CHECK(v34_phase3_s_receiver_init_suffix(
			      &receiver, suffix) == V34_PHASE3_S_RECEIVER_OK,
		      "suffix receiver %u did not initialize", offset);
		for (unsigned int prefix = 0; prefix < 61; ++prefix)
			CHECK(v34_phase3_s_receiver_receive(
				      &receiver, 3,
				      (uint8_t)((prefix * 3u) & 3u)) ==
				      V34_PHASE3_S_RECEIVER_OK,
			      "suffix receiver %u accepted noise at %u",
			      offset, prefix);
		CHECK(receive_suffix(
			      &receiver, rotations, offset, suffix, 2) &&
		      v34_phase3_s_receiver_get_detection(
			      &receiver, &detection) &&
		      detection.clockwise_quarter_turn_offset == offset &&
		      detection.s_errors == 2 &&
		      detection.s_bar_errors == 0,
		      "suffix receiver did not recover offset %u", offset);
	}
	CHECK(v34_phase3_s_receiver_init_suffix(
		      &receiver, suffix + 1u) == V34_PHASE3_S_RECEIVER_OK &&
	      receive_suffix(&receiver, rotations, 2, suffix + 1u, 0) &&
	      v34_phase3_s_receiver_get_detection(&receiver, &detection) &&
	      detection.clockwise_quarter_turn_offset == 2,
	      "odd-length suffix lost its absolute marker phase");
	CHECK(v34_phase3_s_receiver_init_suffix(
		      &receiver, suffix) == V34_PHASE3_S_RECEIVER_OK,
	      "suffix rejection receiver did not initialize");
	for (size_t symbol = V34_PHASE3_S_SYMBOLS - suffix;
	     symbol < V34_PHASE3_S_SEQUENCE_SYMBOLS; ++symbol) {
		uint8_t rotation = rotations[symbol];
		size_t local = symbol - (V34_PHASE3_S_SYMBOLS - suffix);

		if (local < 3u)
			rotation = (uint8_t)((rotation + 1u) & 3u);
		CHECK(v34_phase3_s_receiver_receive(
			      &receiver, 0, rotation) ==
			      V34_PHASE3_S_RECEIVER_OK,
		      "excess-error suffix changed result at %zu", local);
	}
	CHECK(!v34_phase3_s_receiver_is_complete(&receiver),
	      "suffix accepted more than its proportional S error bound");
	memset(&receiver, 0xa5, sizeof(receiver));
	saved = receiver;
	CHECK(v34_phase3_s_receiver_init_suffix(
		      &receiver, suffix - 1u) ==
		      V34_PHASE3_S_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase3_s_receiver_init_suffix(
		      &receiver, V34_PHASE3_S_SYMBOLS + 1u) ==
		      V34_PHASE3_S_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(saved)) == 0,
	      "invalid suffix length changed the receiver");
	return true;
}

/* Keep opposite-point metric evidence while collapsing alternating points
 * into the same hard-decision quadrants. */
static struct v34_analytic_sample collapsed_point(uint8_t rotation)
{
	static const struct v34_analytic_sample POINTS[4] = {
		{ 600, -100 },
		{ 700, -700 },
		{ -600, 100 },
		{ -700, 700 },
	};

	return POINTS[rotation & 3u];
}

static bool test_fixed_metric_fallback(void)
{
	const uint8_t suffix = V34_PHASE3_S_RECEIVER_MIN_S_SUFFIX_SYMBOLS;
	const size_t first = V34_PHASE3_S_SYMBOLS - suffix;
	uint8_t rotations[V34_PHASE3_S_SEQUENCE_SYMBOLS];
	struct v34_phase3_s_receiver metric;
	struct v34_phase3_s_receiver hard;
	struct v34_phase3_s_detection detection;
	unsigned int hard_errors = 0;

	CHECK(generate_marker(rotations) &&
	      v34_phase3_s_receiver_init_fixed_suffix(&metric, suffix) ==
		      V34_PHASE3_S_RECEIVER_OK &&
	      v34_phase3_s_receiver_init_suffix(&hard, suffix) ==
		      V34_PHASE3_S_RECEIVER_OK,
	      "fixed metric receivers did not initialize");
	for (size_t prefix = 0; prefix < 71; ++prefix)
		CHECK(v34_phase3_s_receiver_receive_fixed(
			      &metric, 0, 0,
			      (struct v34_analytic_sample){ 0, 0 }) ==
			      V34_PHASE3_S_RECEIVER_OK,
		      "fixed zero prefix falsely completed at %zu", prefix);
	for (size_t local = 0;
	     local < suffix + V34_PHASE3_S_BAR_SYMBOLS; ++local) {
		size_t symbol = first + local;
		uint8_t rotation = (uint8_t)((rotations[symbol] + 1u) & 3u);
		struct v34_analytic_sample point = collapsed_point(rotation);
		struct v34_fixed_training_decision sliced;

		if (local >= suffix && local < suffix + 3u)
			point = (struct v34_analytic_sample){ 0, 0 };
		CHECK(v34_training_constellation_fixed_slice(
			      V34_CONSTELLATION_4_POINT, point, &sliced) ==
			      V34_CONSTELLATION_OK &&
		      v34_phase3_s_receiver_receive_fixed(
			      &metric, sliced.quarter_point,
			      sliced.clockwise_quarter_turns, point) ==
			      V34_PHASE3_S_RECEIVER_OK &&
		      v34_phase3_s_receiver_receive(
			      &hard, sliced.quarter_point,
			      sliced.clockwise_quarter_turns) ==
			      V34_PHASE3_S_RECEIVER_OK,
		      "collapsed marker failed at %zu", local);
		if (sliced.clockwise_quarter_turns != rotation)
			hard_errors++;
	}
	CHECK(metric.detection_pending && metric.pending_metric &&
	      !metric.complete && !v34_phase3_s_receiver_is_complete(&hard) &&
	      hard_errors > V34_PHASE3_S_RECEIVER_MAX_S_ERRORS,
	      "fixed metric did not retain the collapsed marker");
	for (unsigned int confirmation = 0;
	     confirmation < V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS;
	     ++confirmation) {
		enum v34_phase3_s_receiver_result expected =
			confirmation + 1u ==
				V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS ?
				V34_PHASE3_S_RECEIVER_DETECTED :
				V34_PHASE3_S_RECEIVER_OK;

		CHECK(v34_phase3_s_receiver_receive_fixed(
			      &metric, 0, 0,
			      (struct v34_analytic_sample){ 0, 0 }) == expected,
		      "fixed metric confirmation %u changed", confirmation);
	}
	CHECK(v34_phase3_s_receiver_get_detection(&metric, &detection) &&
	      detection.clockwise_quarter_turn_offset == 1u &&
	      detection.s_errors == 0u && detection.s_bar_errors == 3u &&
	      metric.post_marker_symbols ==
		      V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS,
	      "fixed metric detection changed");
	return true;
}

static bool test_fixed_metric_api_ownership(void)
{
	const uint8_t suffix = V34_PHASE3_S_RECEIVER_MIN_S_SUFFIX_SYMBOLS;
	struct v34_phase3_s_receiver hard;
	struct v34_phase3_s_receiver fixed;
	struct v34_phase3_s_receiver saved;
	struct v34_analytic_sample point = { 600, -100 };

	CHECK(v34_phase3_s_receiver_init_suffix(&hard, suffix) ==
		      V34_PHASE3_S_RECEIVER_OK &&
	      v34_phase3_s_receiver_init_fixed_suffix(&fixed, suffix) ==
		      V34_PHASE3_S_RECEIVER_OK,
	      "fixed API ownership receivers did not initialize");
	saved = hard;
	CHECK(v34_phase3_s_receiver_receive_fixed(&hard, 0, 0, point) ==
		      V34_PHASE3_S_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&hard, &saved, sizeof(hard)) == 0,
	      "hard owner accepted a fixed observation");
	saved = fixed;
	CHECK(v34_phase3_s_receiver_receive(&fixed, 0, 0) ==
		      V34_PHASE3_S_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&fixed, &saved, sizeof(fixed)) == 0 &&
	      v34_phase3_s_receiver_init_fixed_suffix(NULL, suffix) ==
		      V34_PHASE3_S_RECEIVER_INVALID_ARGUMENT,
	      "fixed owner accepted the hard-only API");
	return true;
}

static struct v34_analytic_sample margin_point(
	uint8_t rotation, bool above)
{
	static const int16_t EXACT_MARGIN_COORDINATE[4] = {
		228, 229, 230, 229,
	};
	int16_t coordinate = (int16_t)(
		EXACT_MARGIN_COORDINATE[rotation & 3u] + (above ? 1 : 0));

	switch (rotation & 3u) {
	case 0:
		return (struct v34_analytic_sample){ coordinate, 0 };
	case 1:
		return (struct v34_analytic_sample){ 0, (int16_t)-coordinate };
	case 2:
		return (struct v34_analytic_sample){ (int16_t)-coordinate, 0 };
	default:
		return (struct v34_analytic_sample){ 0, coordinate };
	}
}

static bool receive_metric_margin(
	struct v34_phase3_s_receiver *receiver, bool above,
	bool expected_detection)
{
	const size_t first = V34_PHASE3_S_SYMBOLS -
		V34_PHASE3_S_RECEIVER_MIN_S_SUFFIX_SYMBOLS;
	const size_t count = V34_PHASE3_S_RECEIVER_MIN_S_SUFFIX_SYMBOLS +
		V34_PHASE3_S_BAR_SYMBOLS;
	uint8_t rotations[V34_PHASE3_S_SEQUENCE_SYMBOLS];

	if (!generate_marker(rotations) ||
	    v34_phase3_s_receiver_init_fixed_suffix(
		    receiver,
		    V34_PHASE3_S_RECEIVER_MIN_S_SUFFIX_SYMBOLS) !=
		    V34_PHASE3_S_RECEIVER_OK)
		return false;
	for (size_t local = 0; local < count; ++local)
		if (v34_phase3_s_receiver_receive_fixed(
			    receiver, 1, 0,
			    margin_point(
				    rotations[first + local], above)) !=
		    V34_PHASE3_S_RECEIVER_OK)
			return false;
	for (unsigned int wait = 0;
	     wait < V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS; ++wait) {
		enum v34_phase3_s_receiver_result expected =
			expected_detection &&
				wait + 1u ==
					V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS ?
				V34_PHASE3_S_RECEIVER_DETECTED :
				V34_PHASE3_S_RECEIVER_OK;

		if (v34_phase3_s_receiver_receive_fixed(
			    receiver, 1, 0,
			    (struct v34_analytic_sample){ 0, 0 }) != expected)
			return false;
	}
	return receiver->complete == expected_detection;
}

static bool test_fixed_metric_margin(void)
{
	struct v34_phase3_s_receiver receiver;

	/* At these coordinates the shifted opposite-hypothesis advantage is
	 * exactly 128 and 129 respectively. */
	CHECK(receive_metric_margin(&receiver, false, false),
	      "relative metric accepted its exact margin");
	CHECK(receive_metric_margin(&receiver, true, true),
	      "relative metric rejected the first value above its margin");
	return true;
}

static bool test_completion_and_validation(void)
{
	uint8_t rotations[V34_PHASE3_S_SEQUENCE_SYMBOLS];
	struct v34_phase3_s_receiver receiver;
	struct v34_phase3_s_receiver saved;
	struct v34_phase3_s_receiver corrupt;
	struct v34_phase3_s_detection detection = { 7, 8, 9 };
	struct v34_phase3_s_detection saved_detection = detection;

	CHECK(generate_marker(rotations) &&
	      v34_phase3_s_receiver_init(&receiver) ==
		      V34_PHASE3_S_RECEIVER_OK &&
	      receive_marker(&receiver, rotations, 0),
	      "completion fixture did not detect its marker");
	saved = receiver;
	CHECK(v34_phase3_s_receiver_receive(&receiver, 0, 0) ==
		      V34_PHASE3_S_RECEIVER_COMPLETE &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
	      "completed receiver consumed another rotation");
	CHECK(v34_phase3_s_receiver_init(&receiver) ==
		      V34_PHASE3_S_RECEIVER_OK,
	      "validation receiver did not reset");
	saved = receiver;
	CHECK(v34_phase3_s_receiver_receive(&receiver, 4, 0) ==
		      V34_PHASE3_S_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
	      !v34_phase3_s_receiver_get_detection(&receiver, &detection) &&
	      memcmp(&detection, &saved_detection, sizeof(detection)) == 0,
	      "invalid input changed receiver or detection output");
	corrupt = receiver;
	corrupt.next_decision = V34_PHASE3_S_SEQUENCE_SYMBOLS;
	detection = (struct v34_phase3_s_detection){
		.clockwise_quarter_turn_offset = 3,
		.s_errors = V34_PHASE3_S_RECEIVER_MAX_S_ERRORS,
		.s_bar_errors = V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS,
	};
	CHECK(!v34_phase3_s_receiver_is_valid(NULL) &&
	      !v34_phase3_s_receiver_is_valid(&corrupt) &&
	      !v34_phase3_s_receiver_is_complete(&corrupt) &&
	      v34_phase3_s_detection_is_valid(&detection) &&
	      !v34_phase3_s_detection_is_valid(NULL) &&
	      v34_phase3_s_receiver_init(NULL) ==
		      V34_PHASE3_S_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase3_s_receiver_receive(NULL, 0, 0) ==
		      V34_PHASE3_S_RECEIVER_INVALID_ARGUMENT &&
	      !v34_phase3_s_receiver_get_detection(&saved, NULL),
	      "invalid receiver arguments were accepted");
	CHECK(strcmp(v34_phase3_s_receiver_result_name(
			     V34_PHASE3_S_RECEIVER_DETECTED),
		     "detected") == 0 &&
	      strcmp(v34_phase3_s_receiver_result_name(
			     V34_PHASE3_S_RECEIVER_COMPLETE),
		     "complete") == 0 &&
	      strcmp(v34_phase3_s_receiver_result_name(
			     (enum v34_phase3_s_receiver_result)99),
		     "unknown") == 0,
	      "marker result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated rotations", test_generated_rotations },
		{ "sliding window and tolerance",
		  test_sliding_window_and_tolerance },
		{ "rejection thresholds", test_rejection_thresholds },
		{ "point-zero requirement", test_point_zero_requirement },
		{ "exact transition anchor", test_exact_transition_anchor },
		{ "transition error confirmation",
		  test_transition_error_confirmation },
		{ "suffix acquisition", test_suffix_acquisition },
		{ "fixed metric fallback", test_fixed_metric_fallback },
		{ "fixed metric API ownership",
		  test_fixed_metric_api_ownership },
		{ "fixed metric margin", test_fixed_metric_margin },
		{ "completion and validation", test_completion_and_validation },
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
