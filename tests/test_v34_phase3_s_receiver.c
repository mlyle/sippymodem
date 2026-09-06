// SPDX-License-Identifier: BSD-3-Clause

#include <v34_constellation.h>
#include <v34_phase3_s_receiver.h>

#include "test.h"

#include <math.h>
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

static enum v34_phase3_s_receiver_result receive_adaptive_marker(
	struct v34_phase3_s_receiver *receiver, uint8_t quarter_point,
	uint8_t clockwise_quarter_turns,
	struct v34_analytic_sample observation)
{
	struct v34_phase3_s_adaptation ignored;

	return v34_phase3_s_receiver_receive_adaptive(
		receiver, quarter_point, clockwise_quarter_turns,
		observation, &ignored);
}

static bool test_prefix_qualification(void)
{
	struct v34_phase3_s_prefix prefix;
	struct v34_phase3_s_prefix saved;
	struct v34_phase3_s_prefix corrupt;
	struct qam_point quiet = { 0.1, 0.1 };
	struct qam_point invalid = { NAN, 0.0 };
	struct qam_point point = v34_rotated_point_zero(0);

	CHECK(v34_phase3_s_prefix_init(&prefix) == V34_PHASE3_S_PREFIX_OK &&
	      v34_phase3_s_prefix_is_valid(&prefix),
	      "S prefix detector did not initialize");
	for (unsigned int symbol = 0; symbol < 32; ++symbol)
		CHECK(v34_phase3_s_prefix_receive(&prefix, &point, 0.5) ==
			      V34_PHASE3_S_PREFIX_OK,
		      "constant carrier qualified at symbol %u", symbol);
	CHECK(!prefix.complete && prefix.alternating_turns == 0u,
	      "constant carrier changed S prefix progress");
	for (unsigned int symbol = 0;
	     symbol <= V34_PHASE3_S_PREFIX_TURNS; ++symbol) {
		point = v34_rotated_point_zero((int)(symbol & 1u));
		CHECK(v34_phase3_s_prefix_receive(&prefix, &point, 0.5) ==
			      (symbol == V34_PHASE3_S_PREFIX_TURNS ?
				       V34_PHASE3_S_PREFIX_DETECTED :
				       V34_PHASE3_S_PREFIX_OK),
		      "alternating S prefix stopped at symbol %u", symbol);
	}
	CHECK(prefix.complete &&
	      prefix.alternating_turns == V34_PHASE3_S_PREFIX_TURNS,
	      "alternating S prefix did not qualify");
	saved = prefix;
	CHECK(v34_phase3_s_prefix_receive(&prefix, &point, 0.5) ==
		      V34_PHASE3_S_PREFIX_COMPLETE &&
	      memcmp(&prefix, &saved, sizeof(prefix)) == 0,
	      "completed S prefix detector consumed another point");

	CHECK(v34_phase3_s_prefix_init_recurrent(&prefix) ==
		      V34_PHASE3_S_PREFIX_OK &&
	      prefix.require_recurrence &&
	      v34_phase3_s_prefix_is_valid(&prefix),
	      "recurrent S prefix detector did not initialize");
	for (unsigned int symbol = 0;
	     symbol < V34_PHASE3_S_PREFIX_RECURRENCE_LAG +
			      V34_PHASE3_S_PREFIX_RECURRENCE_HITS;
	     ++symbol) {
		point = v34_rotated_point_zero((int)(symbol & 1u));
		CHECK(v34_phase3_s_prefix_receive(&prefix, &point, 0.5) ==
			      (symbol + 1u ==
				       V34_PHASE3_S_PREFIX_RECURRENCE_LAG +
				       V34_PHASE3_S_PREFIX_RECURRENCE_HITS ?
				       V34_PHASE3_S_PREFIX_DETECTED :
				       V34_PHASE3_S_PREFIX_OK),
		      "recurrent S prefix stopped at symbol %u", symbol);
	}
	CHECK(prefix.complete && prefix.recurrence_qualified &&
	      prefix.alternating_turns == V34_PHASE3_S_PREFIX_TURNS,
	      "recurrent S prefix did not require both forms of evidence");
	CHECK(v34_phase3_s_prefix_init_recurrent(&prefix) ==
		      V34_PHASE3_S_PREFIX_OK,
	      "drifting prefix fixture did not initialize");
	for (unsigned int symbol = 0; symbol < 32u; ++symbol) {
		double degrees = (double)(symbol / 2u) * 20.0 +
				 (symbol & 1u ? 100.0 : 0.0);
		double radians = degrees * 3.14159265358979323846 / 180.0;
		struct qam_point drifting = { cos(radians), sin(radians) };

		CHECK(v34_phase3_s_prefix_receive(
			      &prefix, &drifting, 0.5) ==
			      V34_PHASE3_S_PREFIX_OK,
		      "non-recurrent alternating prefix qualified at %u",
		      symbol);
	}
	CHECK(!prefix.complete && !prefix.recurrence_qualified &&
	      prefix.alternating_turns == V34_PHASE3_S_PREFIX_TURNS,
	      "alternating DIL-like fragment bypassed recurrence");
	CHECK(v34_phase3_s_prefix_init_recurrent(&prefix) ==
		      V34_PHASE3_S_PREFIX_OK,
	      "stale recurrence fixture did not initialize");
	for (unsigned int symbol = 0;
	     symbol < V34_PHASE3_S_PREFIX_RECURRENCE_LAG +
			      V34_PHASE3_S_PREFIX_RECURRENCE_HITS;
	     ++symbol) {
		static const struct qam_point recurring[] = {
			{ 3.0, 1.0 },
			{ 2.0, 2.0 },
		};

		CHECK(v34_phase3_s_prefix_receive(
			      &prefix, &recurring[symbol & 1u], 0.5) ==
			      (symbol + 1u ==
				       V34_PHASE3_S_PREFIX_RECURRENCE_LAG +
					       V34_PHASE3_S_PREFIX_RECURRENCE_HITS ?
				       V34_PHASE3_S_PREFIX_REBASE_TIMING :
				       V34_PHASE3_S_PREFIX_OK),
		      "stale recurrence fixture stopped at %u", symbol);
	}
	CHECK(prefix.recurrence_qualified && !prefix.complete,
	      "non-alternating recurrence did not qualify independently");
	for (unsigned int symbol = 0;
	     symbol <= V34_PHASE3_S_PREFIX_TURNS; ++symbol) {
		point = v34_rotated_point_zero((int)(symbol & 1u));
		CHECK(v34_phase3_s_prefix_receive(&prefix, &point, 0.5) ==
			      V34_PHASE3_S_PREFIX_OK,
		      "stale recurrence qualified later turns at %u", symbol);
	}
	CHECK(!prefix.complete && !prefix.recurrence_qualified,
	      "recurrent gate retained unrelated earlier evidence");

	CHECK(v34_phase3_s_prefix_init(&prefix) == V34_PHASE3_S_PREFIX_OK,
	      "S prefix reset fixture did not initialize");
	point = v34_rotated_point_zero(0);
	CHECK(v34_phase3_s_prefix_receive(&prefix, &point, 0.5) ==
		      V34_PHASE3_S_PREFIX_OK,
	      "S prefix reset fixture rejected its first point");
	point = v34_rotated_point_zero(1);
	CHECK(v34_phase3_s_prefix_receive(&prefix, &point, 0.5) ==
		      V34_PHASE3_S_PREFIX_OK &&
	      prefix.alternating_turns == 1u &&
	      v34_phase3_s_prefix_receive(&prefix, &quiet, 0.5) ==
		      V34_PHASE3_S_PREFIX_OK &&
	      v34_phase3_s_prefix_is_valid(&prefix) &&
	      !prefix.previous_valid,
	      "low-power point did not reset S prefix progress");
	saved = prefix;
	CHECK(v34_phase3_s_prefix_receive(&prefix, &invalid, 0.5) ==
		      V34_PHASE3_S_PREFIX_INVALID_ARGUMENT &&
	      memcmp(&prefix, &saved, sizeof(prefix)) == 0 &&
	      v34_phase3_s_prefix_receive(&prefix, &point, NAN) ==
		      V34_PHASE3_S_PREFIX_INVALID_ARGUMENT &&
	      memcmp(&prefix, &saved, sizeof(prefix)) == 0 &&
	      v34_phase3_s_prefix_receive(NULL, &point, 0.5) ==
		      V34_PHASE3_S_PREFIX_INVALID_ARGUMENT &&
	      v34_phase3_s_prefix_init(NULL) ==
		      V34_PHASE3_S_PREFIX_INVALID_ARGUMENT &&
	      v34_phase3_s_prefix_init_recurrent(NULL) ==
		      V34_PHASE3_S_PREFIX_INVALID_ARGUMENT,
	      "invalid S prefix call changed detector state");
	corrupt = prefix;
	corrupt.alternating_turns = V34_PHASE3_S_PREFIX_TURNS + 1u;
	CHECK(!v34_phase3_s_prefix_is_valid(NULL) &&
	      !v34_phase3_s_prefix_is_valid(&corrupt),
	      "invalid S prefix state was accepted");
	return true;
}

static bool test_prefix_timing_ambiguity(void)
{
	static const struct qam_point SAME_PHASE_PAIR[] = {
		{ 3.0, 1.0 },
		{ 2.0, 2.0 },
	};
	struct v34_phase3_s_prefix prefix;

	CHECK(v34_phase3_s_prefix_init(&prefix) == V34_PHASE3_S_PREFIX_OK,
	      "ambiguous S prefix detector did not initialize");
	for (unsigned int symbol = 0;
	     symbol < V34_PHASE3_S_PREFIX_RECURRENCE_LAG +
		      V34_PHASE3_S_PREFIX_RECURRENCE_HITS;
	     ++symbol) {
		enum v34_phase3_s_prefix_result expected =
			symbol + 1u ==
				V34_PHASE3_S_PREFIX_RECURRENCE_LAG +
					V34_PHASE3_S_PREFIX_RECURRENCE_HITS ?
				V34_PHASE3_S_PREFIX_REBASE_TIMING :
				V34_PHASE3_S_PREFIX_OK;

		CHECK(v34_phase3_s_prefix_receive(
			      &prefix, &SAME_PHASE_PAIR[symbol & 1u], 0.5) ==
			      expected,
		      "ambiguous S timing stopped at symbol %u", symbol);
	}
	CHECK(prefix.recurrence_qualified && !prefix.complete &&
	      prefix.alternating_turns == 0u &&
	      v34_phase3_s_prefix_receive(
		      &prefix, &SAME_PHASE_PAIR[0], 0.5) ==
		      V34_PHASE3_S_PREFIX_OK,
	      "ambiguous S timing did not request exactly one rebase");
	return true;
}

static bool test_generated_rotations(void)
{
	uint8_t rotations[V34_PHASE3_S_SEQUENCE_SYMBOLS];

	CHECK(generate_marker(rotations), "S/S-bar marker did not generate");
	for (uint8_t offset = 0; offset < 4u; ++offset) {
		struct v34_phase3_s_receiver receiver;
		struct v34_phase3_s_detection detection = { 0xff, 0xff, 0xff };

		CHECK(v34_phase3_s_receiver_init_suffix(&receiver, V34_PHASE3_S_SYMBOLS) ==
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
	      v34_phase3_s_receiver_init_suffix(&receiver, V34_PHASE3_S_SYMBOLS) ==
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
	    v34_phase3_s_receiver_init_suffix(&receiver, V34_PHASE3_S_SYMBOLS) !=
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
	      v34_phase3_s_receiver_init_suffix(&receiver, V34_PHASE3_S_SYMBOLS) ==
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
	      v34_phase3_s_receiver_init_suffix(&receiver, V34_PHASE3_S_SYMBOLS) ==
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

static bool test_transition_anchor_tolerance(void)
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
	CHECK(v34_phase3_s_receiver_init_suffix(&receiver, V34_PHASE3_S_SYMBOLS) ==
		      V34_PHASE3_S_RECEIVER_OK &&
	      v34_phase3_s_receiver_receive(
		      &receiver, 0, generated[0]) ==
		      V34_PHASE3_S_RECEIVER_OK &&
	      v34_phase3_s_receiver_receive(
		      &receiver, 0, generated[1]) ==
		      V34_PHASE3_S_RECEIVER_OK &&
	      receive_marker(&receiver, generated, 0),
	      "S-like prefix shifted the transition boundary");
	for (size_t anchor = 0; anchor < ARRAY_LENGTH(ANCHORS); ++anchor) {
		uint8_t rotations[V34_PHASE3_S_SEQUENCE_SYMBOLS];
		struct v34_phase3_s_detection detection;
		bool detected = false;

		memcpy(rotations, generated, sizeof(rotations));
		rotations[ANCHORS[anchor]] =
			(uint8_t)((rotations[ANCHORS[anchor]] + 1u) & 3u);
		CHECK(v34_phase3_s_receiver_init_suffix(&receiver, V34_PHASE3_S_SYMBOLS) ==
			      V34_PHASE3_S_RECEIVER_OK,
		      "transition-anchor receiver did not initialize");
		for (size_t symbol = 0;
		     symbol < V34_PHASE3_S_SEQUENCE_SYMBOLS; ++symbol) {
			enum v34_phase3_s_receiver_result result =
				v34_phase3_s_receiver_receive(
					&receiver, 0, rotations[symbol]);

			CHECK(result == V34_PHASE3_S_RECEIVER_OK ||
				      (result == V34_PHASE3_S_RECEIVER_DETECTED &&
				       symbol + 1u +
					       V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS >=
					       V34_PHASE3_S_SEQUENCE_SYMBOLS),
			      "boundary error %zu changed result %d at symbol %zu",
			      ANCHORS[anchor], result, symbol);
			detected = result == V34_PHASE3_S_RECEIVER_DETECTED;
			if (detected)
				break;
		}
		for (unsigned int confirmation = 0;
		     !detected &&
		     confirmation <
			     V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS;
		     ++confirmation) {
			enum v34_phase3_s_receiver_result result =
				v34_phase3_s_receiver_receive(
					&receiver, 1, (uint8_t)confirmation);

			CHECK(result == V34_PHASE3_S_RECEIVER_OK ||
				      result == V34_PHASE3_S_RECEIVER_DETECTED,
			      "boundary error %zu confirmation %u changed",
			      ANCHORS[anchor], confirmation);
			detected = result == V34_PHASE3_S_RECEIVER_DETECTED;
		}
		CHECK(detected &&
		      v34_phase3_s_receiver_get_detection(
			      &receiver, &detection) &&
		      detection.clockwise_quarter_turn_offset == 0u &&
		      detection.s_errors + detection.s_bar_errors == 1u,
		      "bounded transition error %zu was not retained",
		      ANCHORS[anchor]);
	}
	return true;
}

static bool test_fixed_smeared_transition_confirmation(void)
{
	const uint8_t suffix = V34_PHASE3_S_RECEIVER_MIN_S_SUFFIX_SYMBOLS;
	const size_t first = V34_PHASE3_S_SYMBOLS - suffix;
	uint8_t rotations[V34_PHASE3_S_SEQUENCE_SYMBOLS];
	struct v34_phase3_s_receiver receiver;
	struct v34_phase3_s_detection detection;

	CHECK(generate_marker(rotations),
	      "smeared transition marker did not generate");
	CHECK(v34_phase3_s_receiver_init_fixed_suffix(&receiver, suffix) ==
		      V34_PHASE3_S_RECEIVER_OK,
	      "smeared transition receiver did not initialize");
	for (size_t local = 0;
	     local < suffix + V34_PHASE3_S_BAR_SYMBOLS; ++local) {
		size_t symbol = first + local;
		struct v34_analytic_sample point;
		uint8_t hard_rotation = rotations[symbol];
		enum v34_phase3_s_receiver_result expected =
			local + 1u == suffix + V34_PHASE3_S_BAR_SYMBOLS ?
				V34_PHASE3_S_RECEIVER_DETECTED :
				V34_PHASE3_S_RECEIVER_OK;

		CHECK(v34_training_constellation_fixed_point(
			      V34_CONSTELLATION_4_POINT, 0,
			      rotations[symbol],
			      &point) == V34_CONSTELLATION_OK,
		      "smeared transition point %zu did not generate", local);
		/* A physical 3200-symbol/s trace moved two terminal S decisions and
		 * one later S-bar decision toward the following point. */
		if (symbol == V34_PHASE3_S_SYMBOLS - 2u ||
		    symbol == V34_PHASE3_S_SYMBOLS - 1u ||
		    symbol == V34_PHASE3_S_SYMBOLS + 13u)
			hard_rotation = (uint8_t)((hard_rotation + 3u) & 3u);
		if (symbol == first + 7u)
			point = (struct v34_analytic_sample){ 0, 0 };
		CHECK(receive_adaptive_marker(
			      &receiver, 0, hard_rotation, point) == expected,
		      "smeared transition changed at %zu", local);
	}
	CHECK(v34_phase3_s_receiver_get_detection(&receiver, &detection) &&
	      detection.s_errors == 2u && detection.s_bar_errors == 1u &&
	      receiver.post_marker_symbols == 0u,
	      "smeared transition evidence was not retained");
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
	CHECK(v34_phase3_s_receiver_init_suffix(&receiver, V34_PHASE3_S_SYMBOLS) ==
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
		      &receiver, suffix) == V34_PHASE3_S_RECEIVER_OK &&
	      receive_suffix(
		      &receiver, rotations, 1, suffix,
		      V34_PHASE3_S_RECEIVER_MAX_S_ERRORS),
	      "suffix lost the bounded S error budget");
	CHECK(v34_phase3_s_receiver_init_suffix(
		      &receiver, suffix) == V34_PHASE3_S_RECEIVER_OK,
	      "suffix rejection receiver did not initialize");
	for (size_t symbol = V34_PHASE3_S_SYMBOLS - suffix;
	     symbol < V34_PHASE3_S_SEQUENCE_SYMBOLS; ++symbol) {
		uint8_t rotation = rotations[symbol];
		size_t local = symbol - (V34_PHASE3_S_SYMBOLS - suffix);

		if (local <= V34_PHASE3_S_RECEIVER_MAX_S_ERRORS)
			rotation = (uint8_t)((rotation + 1u) & 3u);
		CHECK(v34_phase3_s_receiver_receive(
			      &receiver, 0, rotation) ==
			      V34_PHASE3_S_RECEIVER_OK,
		      "excess-error suffix changed result at %zu", local);
	}
	CHECK(!v34_phase3_s_receiver_is_complete(&receiver),
	      "suffix accepted more than its bounded S error budget");
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
		CHECK(receive_adaptive_marker(
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
		      receive_adaptive_marker(
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

		CHECK(receive_adaptive_marker(
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

static bool test_fixed_metric_exact_marker(void)
{
	const uint8_t suffix = V34_PHASE3_S_RECEIVER_MIN_S_SUFFIX_SYMBOLS;
	const size_t first = V34_PHASE3_S_SYMBOLS - suffix;
	uint8_t rotations[V34_PHASE3_S_SEQUENCE_SYMBOLS];
	struct v34_phase3_s_receiver receiver;
	struct v34_phase3_s_detection detection;
	unsigned int hard_errors = 0;

	CHECK(generate_marker(rotations) &&
	      v34_phase3_s_receiver_init_fixed_suffix(&receiver, suffix) ==
		      V34_PHASE3_S_RECEIVER_OK,
	      "exact fixed marker did not initialize");
	for (size_t local = 0;
	     local < suffix + V34_PHASE3_S_BAR_SYMBOLS; ++local) {
		size_t symbol = first + local;
		uint8_t rotation = (uint8_t)((rotations[symbol] + 1u) & 3u);
		struct v34_analytic_sample point = collapsed_point(rotation);
		struct v34_fixed_training_decision sliced;
		enum v34_phase3_s_receiver_result expected =
			local + 1u == suffix + V34_PHASE3_S_BAR_SYMBOLS ?
				V34_PHASE3_S_RECEIVER_DETECTED :
				V34_PHASE3_S_RECEIVER_OK;

		CHECK(v34_training_constellation_fixed_slice(
			      V34_CONSTELLATION_4_POINT, point, &sliced) ==
			      V34_CONSTELLATION_OK &&
		      receive_adaptive_marker(
			      &receiver, sliced.quarter_point,
			      sliced.clockwise_quarter_turns, point) == expected,
		      "exact fixed marker failed at %zu", local);
		hard_errors += sliced.clockwise_quarter_turns != rotation;
	}
	CHECK(hard_errors > V34_PHASE3_S_RECEIVER_MAX_S_ERRORS &&
	      v34_phase3_s_receiver_get_detection(&receiver, &detection) &&
	      detection.clockwise_quarter_turn_offset == 1u &&
	      detection.s_errors == 0u && detection.s_bar_errors == 0u &&
	      receiver.post_marker_symbols == 0u,
	      "exact fixed S-bar did not complete at its physical boundary");
	return true;
}

static bool test_fixed_metric_api_ownership(void)
{
	const uint8_t suffix = V34_PHASE3_S_RECEIVER_MIN_S_SUFFIX_SYMBOLS;
	struct v34_phase3_s_adaptation adaptation = {
		.target = { 7, 9 },
		.adapt = false,
	};
	struct v34_phase3_s_adaptation saved_adaptation = adaptation;
	struct v34_phase3_s_receiver hard;
	struct v34_phase3_s_receiver fixed;
	struct v34_phase3_s_receiver saved;
	struct v34_analytic_sample point = { 600, -100 };
	struct v34_analytic_sample target;

	CHECK(v34_phase3_s_receiver_init_suffix(&hard, suffix) ==
		      V34_PHASE3_S_RECEIVER_OK &&
	      v34_phase3_s_receiver_init_fixed_suffix(&fixed, suffix) ==
		      V34_PHASE3_S_RECEIVER_OK,
	      "fixed API ownership receivers did not initialize");
	saved = hard;
	CHECK(receive_adaptive_marker(&hard, 0, 0, point) ==
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
	CHECK(v34_training_constellation_fixed_point(
		      V34_CONSTELLATION_4_POINT, 0u, 0u, &point) ==
		      V34_CONSTELLATION_OK &&
	      v34_phase3_s_receiver_receive_adaptive(
		      &fixed, 0u, 0u, point, &adaptation) ==
		      V34_PHASE3_S_RECEIVER_OK &&
	      adaptation.adapt &&
	      adaptation.target.in_phase == point.in_phase &&
	      adaptation.target.quadrature == point.quadrature &&
	      fixed.target_seeded && fixed.next_target_rotation == 3u,
	      "adaptive fixed owner did not publish its first S target");
	saved = fixed;
	CHECK(v34_phase3_s_receiver_receive_adaptive(
		      &fixed, 0u, 3u, point,
		      (struct v34_phase3_s_adaptation *)&fixed) ==
		      V34_PHASE3_S_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&fixed, &saved, sizeof(fixed)) == 0,
	      "adaptive owner alias changed state");
	CHECK(v34_training_constellation_fixed_point(
		      V34_CONSTELLATION_4_POINT, 0u, 1u, &point) ==
		      V34_CONSTELLATION_OK &&
	      v34_training_constellation_fixed_point(
		      V34_CONSTELLATION_4_POINT, 0u, 3u, &target) ==
		      V34_CONSTELLATION_OK,
	      "adaptive S-bar target did not generate");
	adaptation = saved_adaptation;
	CHECK(v34_phase3_s_receiver_receive_adaptive(
		      &fixed, 0u, 1u, point, &adaptation) ==
		      V34_PHASE3_S_RECEIVER_OK &&
	      !adaptation.adapt && adaptation.target.in_phase == 0 &&
	      adaptation.target.quadrature == 0 &&
	      target.in_phase != point.in_phase &&
	      target.quadrature != point.quadrature,
	      "adaptive owner trained on an S-bar opposite");
	CHECK(v34_phase3_s_receiver_init_fixed_suffix(&fixed, suffix) ==
		      V34_PHASE3_S_RECEIVER_OK,
	      "ambiguous adaptive owner did not initialize");
	adaptation = saved_adaptation;
	CHECK(v34_phase3_s_receiver_receive_adaptive(
		      &fixed, 0u, 0u,
		      (struct v34_analytic_sample){ 0, 0 }, &adaptation) ==
		      V34_PHASE3_S_RECEIVER_OK &&
	      adaptation.adapt && adaptation.target.in_phase != 0 &&
	      adaptation.target.quadrature != 0,
	      "adaptive owner discarded an ambiguous S observation");
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
	for (size_t local = 0; local < count; ++local) {
		enum v34_phase3_s_receiver_result expected =
			expected_detection && local + 1u == count ?
				V34_PHASE3_S_RECEIVER_DETECTED :
				V34_PHASE3_S_RECEIVER_OK;

		if (receive_adaptive_marker(
			    receiver, 1, 0,
			    margin_point(
				    rotations[first + local], above)) != expected)
			return false;
	}
	if (expected_detection)
		return receiver->complete;
	for (unsigned int wait = 0;
	     wait < V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS; ++wait) {
		enum v34_phase3_s_receiver_result expected =
			expected_detection &&
				wait + 1u ==
					V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS ?
				V34_PHASE3_S_RECEIVER_DETECTED :
				V34_PHASE3_S_RECEIVER_OK;

		if (receive_adaptive_marker(
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
	      v34_phase3_s_receiver_init_suffix(&receiver, V34_PHASE3_S_SYMBOLS) ==
		      V34_PHASE3_S_RECEIVER_OK &&
	      receive_marker(&receiver, rotations, 0),
	      "completion fixture did not detect its marker");
	saved = receiver;
	CHECK(v34_phase3_s_receiver_receive(&receiver, 0, 0) ==
		      V34_PHASE3_S_RECEIVER_COMPLETE &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
	      "completed receiver consumed another rotation");
	CHECK(v34_phase3_s_receiver_init_suffix(&receiver, V34_PHASE3_S_SYMBOLS) ==
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
	      v34_phase3_s_receiver_init_suffix(NULL, V34_PHASE3_S_SYMBOLS) ==
		      V34_PHASE3_S_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase3_s_receiver_receive(NULL, 0, 0) ==
		      V34_PHASE3_S_RECEIVER_INVALID_ARGUMENT &&
	      !v34_phase3_s_receiver_get_detection(&saved, NULL),
	      "invalid receiver arguments were accepted");
	return true;
}

static bool hunt_marker(uint8_t offset, size_t skip, bool errors)
{
	uint8_t rotations[V34_PHASE3_S_SEQUENCE_SYMBOLS];
	struct v34_phase3_s_hunt hunt;
	struct v34_phase3_s_detection detection;
	struct v34_analytic_sample tail[
		V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS] = { 0 };
	uint8_t post_symbols = UINT8_MAX;
	size_t first_candidate = 0;
	size_t received = 0;
	bool detected = false;

	CHECK(generate_marker(rotations) &&
	      v34_phase3_s_hunt_init(&hunt) == V34_PHASE3_S_RECEIVER_OK,
	      "online S hunt did not initialize");
	for (size_t symbol = skip;
	     symbol < V34_PHASE3_S_SEQUENCE_SYMBOLS + ARRAY_LENGTH(tail) &&
	     !detected; ++symbol) {
		struct v34_phase3_s_adaptation adaptation = { 0 };
		struct v34_analytic_sample observation;
		uint8_t rotation = symbol < V34_PHASE3_S_SEQUENCE_SYMBOLS ?
			rotations[symbol] : 1u;
		bool control = false;
		enum v34_phase3_s_receiver_result result;

		/* Delay qualification through the complete S error budget, then
		 * require bounded confirmation for one damaged S-bar decision. */
		if (errors &&
		    (symbol < V34_PHASE3_S_HUNT_CANDIDATE_PREFIX_SYMBOLS *
				       V34_PHASE3_S_RECEIVER_MAX_S_ERRORS &&
		      (symbol + 1u) %
			      V34_PHASE3_S_HUNT_CANDIDATE_PREFIX_SYMBOLS == 0u))
			rotation = (uint8_t)((rotation + 1u) & 3u);
		if (errors && symbol == V34_PHASE3_S_SYMBOLS)
			rotation = (uint8_t)((rotation + 1u) & 3u);
		CHECK(v34_training_constellation_fixed_point(
			      V34_CONSTELLATION_4_POINT, 0,
			      (uint8_t)((rotation + offset) & 3u),
			      &observation) == V34_CONSTELLATION_OK,
		      "online S point did not generate");
		for (size_t index = 1; index < ARRAY_LENGTH(tail); ++index)
			tail[index - 1u] = tail[index];
		tail[ARRAY_LENGTH(tail) - 1u] = observation;
		result = v34_phase3_s_hunt_receive(
			&hunt, observation, &adaptation, &control);
		received++;
		CHECK(result == V34_PHASE3_S_RECEIVER_OK ||
		      result == V34_PHASE3_S_RECEIVER_DETECTED,
		      "online S hunt stopped at symbol %zu: %d", symbol, result);
		if (control && first_candidate == 0)
			first_candidate = received;
		CHECK(!adaptation.adapt || control,
		      "unqualified online data became equalizer feedback");
		if (symbol >= (size_t)V34_PHASE3_S_SYMBOLS + (errors ? 1u : 0u))
			CHECK(!adaptation.adapt,
			      "S-bar or its confirmation tail became feedback");
		detected = result == V34_PHASE3_S_RECEIVER_DETECTED;
	}
	CHECK(detected &&
	      first_candidate == V34_PHASE3_S_HUNT_CANDIDATE_PREFIX_SYMBOLS *
		      (errors ? V34_PHASE3_S_RECEIVER_MAX_S_ERRORS + 1u : 1u) &&
	      v34_phase3_s_hunt_get_detection(&hunt, &detection) &&
	      v34_phase3_s_hunt_get_post_marker_symbols(&hunt, &post_symbols) &&
	      (errors ?
		      received + skip >= V34_PHASE3_S_SEQUENCE_SYMBOLS &&
			      received + skip <= V34_PHASE3_S_SEQUENCE_SYMBOLS +
				      ARRAY_LENGTH(tail) :
		      received + skip == V34_PHASE3_S_SEQUENCE_SYMBOLS) &&
	      post_symbols == (errors ?
		      V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS : 0u),
	      "online S boundary changed offset=%u skip=%zu errors=%u "
	      "candidate=%zu received=%zu post=%u",
	      offset, skip, errors, first_candidate, received, post_symbols);
	for (uint8_t index = 0; index < post_symbols; ++index) {
		struct v34_analytic_sample observation = { 0 };

		CHECK(v34_phase3_s_hunt_get_post_marker_observation(
			      &hunt, index, &observation) &&
		      memcmp(&observation,
			     &tail[ARRAY_LENGTH(tail) - post_symbols + index],
			     sizeof(observation)) == 0,
		      "post-marker observation %u was lost or reordered", index);
	}
	{
		struct v34_phase3_s_hunt saved = hunt;
		struct v34_phase3_s_adaptation adaptation = { 0 };
		struct v34_phase3_s_adaptation saved_adaptation = adaptation;
		struct v34_analytic_sample observation = { 1, 2 };
		bool control = false;

		CHECK(v34_phase3_s_hunt_receive(
			      &hunt, observation, &adaptation, &control) ==
			      V34_PHASE3_S_RECEIVER_COMPLETE &&
		      memcmp(&hunt, &saved, sizeof(hunt)) == 0 &&
		      memcmp(&adaptation, &saved_adaptation,
			     sizeof(adaptation)) == 0 && !control &&
		      !v34_phase3_s_hunt_get_post_marker_observation(
			      &hunt, post_symbols, &observation) &&
		      observation.in_phase == 1 && observation.quadrature == 2,
		      "completed hunt consumed a point or changed an output");
	}
	return true;
}

static bool test_online_hunt_boundaries(void)
{
	for (uint8_t offset = 0; offset < 4u; ++offset) {
		CHECK(hunt_marker(offset, 0, false) &&
		      hunt_marker(offset, 1, false) &&
		      hunt_marker(offset, 0, true),
		      "online S hunt failed at rotation %u", offset);
	}
	return true;
}

static bool test_online_hunt_requires_physical_recurrence(void)
{
	/* These points alternate between adjacent quadrants, but their actual
	 * phases do not recur after two symbols. Replacing them with four-point
	 * hard targets would invent an S prefix and train the live equalizer
	 * against ordinary higher-order QAM data. */
	static const struct v34_analytic_sample points[] = {
		{ 4000, 1000 }, { -1000, 4000 },
		{ 1000, 4000 }, { -4000, 1000 },
	};
	struct v34_phase3_s_hunt hunt;
	uint8_t rotations[2] = { 0 };

	CHECK(v34_phase3_s_hunt_init(&hunt) == V34_PHASE3_S_RECEIVER_OK,
	      "physical recurrence hunt did not initialize");
	for (size_t symbol = 0;
	     symbol < V34_PHASE3_S_HUNT_CANDIDATE_PREFIX_SYMBOLS; ++symbol) {
		struct v34_analytic_sample observation =
			points[symbol % ARRAY_LENGTH(points)];
		struct v34_fixed_training_decision hard;
		struct v34_phase3_s_adaptation adaptation = { 0 };
		bool control = false;

		CHECK(v34_training_constellation_fixed_slice(
			      V34_CONSTELLATION_4_POINT, observation, &hard) ==
			      V34_CONSTELLATION_OK,
		      "physical recurrence observation did not slice");
		if (symbol < 2u)
			rotations[symbol] = hard.clockwise_quarter_turns;
		CHECK(hard.clockwise_quarter_turns == rotations[symbol & 1u] &&
		      (symbol == 0u ||
		       ((rotations[1] + 4u - rotations[0]) & 1u) != 0u),
		      "physical recurrence fixture lost alternating hard quadrants");
		CHECK(v34_phase3_s_hunt_receive(
			      &hunt, observation, &adaptation, &control) ==
			      V34_PHASE3_S_RECEIVER_OK &&
		      !control && !adaptation.adapt && !hunt.candidate &&
		      !v34_phase3_s_hunt_is_complete(&hunt),
		      "non-recurrent data armed S adaptation at symbol %zu", symbol);
	}
	return true;
}

static bool test_online_hunt_rejects_non_s_recurrence(void)
{
	/* Relative near-quarter turns can recur within the same quadrant or
	 * across opposite quadrants. Neither pair is an absolute S hypothesis,
	 * but both are ordinary online observations, not invalid API input. */
	static const struct v34_analytic_sample pairs[][2] = {
		{ { 4000, 1000 }, { 1000, 4000 } },
		{ { 4000, 1000 }, { -1000, -4000 } },
	};
	uint8_t rotations[V34_PHASE3_S_SEQUENCE_SYMBOLS];

	CHECK(generate_marker(rotations), "recovery S marker did not generate");
	for (size_t pair = 0; pair < ARRAY_LENGTH(pairs); ++pair) {
		struct v34_phase3_s_hunt hunt;
		struct v34_phase3_s_adaptation adaptation = { 0 };
		bool control = false;

		CHECK(v34_phase3_s_hunt_init(&hunt) == V34_PHASE3_S_RECEIVER_OK,
		      "non-S recurrence hunt did not initialize");
		for (size_t symbol = 0;
		     symbol < 2u * V34_PHASE3_S_HUNT_CANDIDATE_PREFIX_SYMBOLS;
		     ++symbol) {
			CHECK(v34_phase3_s_hunt_receive(
				      &hunt, pairs[pair][symbol & 1u],
				      &adaptation, &control) ==
				      V34_PHASE3_S_RECEIVER_OK &&
			      !control && !adaptation.adapt && !hunt.candidate &&
			      !v34_phase3_s_hunt_is_complete(&hunt),
			      "non-S pair %zu interrupted data at symbol %zu",
			      pair, symbol);
			if (symbol + 2u ==
			    V34_PHASE3_S_HUNT_CANDIDATE_PREFIX_SYMBOLS)
				CHECK(hunt.prefix.alternating_turns ==
					      V34_PHASE3_S_PREFIX_TURNS &&
				      hunt.prefix.recurrence_hits + 1u ==
					      V34_PHASE3_S_HUNT_CANDIDATE_RECURRENCE_HITS,
				      "non-S fixture did not approach qualification");
		}
		/* Rejecting data must not prevent the next genuine retrain marker. */
		for (size_t symbol = 0;
		     symbol < ARRAY_LENGTH(rotations); ++symbol) {
			struct v34_analytic_sample observation;

			CHECK(v34_training_constellation_fixed_point(
				      V34_CONSTELLATION_4_POINT, 0u,
				      rotations[symbol], &observation) ==
				      V34_CONSTELLATION_OK &&
			      v34_phase3_s_hunt_receive(
				      &hunt, observation, &adaptation, &control) ==
				      (symbol + 1u == ARRAY_LENGTH(rotations) ?
					       V34_PHASE3_S_RECEIVER_DETECTED :
					       V34_PHASE3_S_RECEIVER_OK),
			      "S marker after rejected pair %zu failed at symbol %zu",
			      pair, symbol);
		}
	}
	return true;
}

static bool test_online_hunt_reset_and_validation(void)
{
	struct v34_phase3_s_hunt hunt;
	struct v34_phase3_s_hunt saved;
	struct v34_phase3_s_adaptation adaptation = { 0 };
	struct v34_analytic_sample observation = { 0 };
	bool control = false;

	CHECK(v34_phase3_s_hunt_init(&hunt) == V34_PHASE3_S_RECEIVER_OK,
	      "online S reset fixture did not initialize");
	for (size_t symbol = 0;
	     symbol < V34_PHASE3_S_HUNT_CANDIDATE_PREFIX_SYMBOLS; ++symbol) {
		CHECK(v34_training_constellation_fixed_point(
			      V34_CONSTELLATION_4_POINT, 0, (uint8_t)(symbol & 1u),
			      &observation) == V34_CONSTELLATION_OK &&
		      v34_phase3_s_hunt_receive(
			      &hunt, observation, &adaptation, &control) ==
			      V34_PHASE3_S_RECEIVER_OK,
		      "online S candidate did not start");
	}
	CHECK(control && hunt.candidate,
	      "online S prefix did not qualify");
	for (size_t symbol = 0;
	     symbol < V34_PHASE3_S_HUNT_CANDIDATE_SYMBOLS; ++symbol)
		CHECK(v34_phase3_s_hunt_receive(
			      &hunt, observation, &adaptation, &control) ==
			      V34_PHASE3_S_RECEIVER_OK,
		      "uncompleted online S candidate did not expire");
	CHECK(!hunt.candidate && !hunt.complete &&
	      v34_phase3_s_hunt_is_valid(&hunt),
	      "expired online S candidate kept protocol state");
	saved = hunt;
	CHECK(v34_phase3_s_hunt_receive(
		      &hunt, observation, NULL, &control) ==
		      V34_PHASE3_S_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase3_s_hunt_receive(
		      &hunt, observation, &adaptation, &hunt.candidate) ==
		      V34_PHASE3_S_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&hunt, &saved, sizeof(hunt)) == 0 &&
	      !v34_phase3_s_hunt_is_valid(NULL),
	      "invalid online S call changed the hunt");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "prefix qualification", test_prefix_qualification },
		{ "prefix timing ambiguity", test_prefix_timing_ambiguity },
		{ "generated rotations", test_generated_rotations },
		{ "sliding window and tolerance",
		  test_sliding_window_and_tolerance },
		{ "rejection thresholds", test_rejection_thresholds },
		{ "point-zero requirement", test_point_zero_requirement },
		{ "transition anchor tolerance",
		  test_transition_anchor_tolerance },
		{ "fixed smeared transition confirmation",
		  test_fixed_smeared_transition_confirmation },
		{ "transition error confirmation",
		  test_transition_error_confirmation },
		{ "suffix acquisition", test_suffix_acquisition },
		{ "fixed metric fallback", test_fixed_metric_fallback },
		{ "fixed metric exact marker",
		  test_fixed_metric_exact_marker },
		{ "fixed metric API ownership",
		  test_fixed_metric_api_ownership },
		{ "fixed metric margin", test_fixed_metric_margin },
		{ "completion and validation", test_completion_and_validation },
		{ "online S hunt boundaries", test_online_hunt_boundaries },
		{ "online S hunt physical recurrence",
		  test_online_hunt_requires_physical_recurrence },
		{ "online S hunt non-S recurrence",
		  test_online_hunt_rejects_non_s_recurrence },
		{ "online S hunt reset and validation",
		  test_online_hunt_reset_and_validation },
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
