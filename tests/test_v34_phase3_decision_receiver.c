// SPDX-License-Identifier: BSD-3-Clause

#include <v34_constellation.h>
#include <v34_phase3_decision_receiver.h>
#include <v34_phase3_j_sequence.h>
#include <v34_phase3_s_sequence.h>
#include <v34_phase3_trn.h>

#include "test.h"

#include <string.h>

enum {
	TEST_TRN_SYMBOLS = 640,
	TEST_J_SYMBOLS = V34_PHASE3_J_RECEIVER_WINDOW_SYMBOLS,
	TEST_MD_SYMBOLS = 12,
	TEST_DECODER_POWER_Q15 = 0x7000,
};

struct observation_clock {
	uint64_t epoch;
	uint64_t lineage;
};

static struct v34_adaptive_equalizer_observation observation(
	struct observation_clock *clock, struct v34_analytic_sample point)
{
	clock->epoch++;
	clock->lineage = (clock->lineage ^
		(uint16_t)point.in_phase ^
		((uint64_t)(uint16_t)point.quadrature << 16u)) *
		UINT64_C(1099511628211);
	if (clock->lineage == 0u)
		clock->lineage = UINT64_C(14695981039346656037);
	return (struct v34_adaptive_equalizer_observation){
		.decision = point,
		.epoch = clock->epoch,
		.lineage = clock->lineage,
	};
}

static bool observations_are_equal(
	const struct v34_adaptive_equalizer_observation *left,
	const struct v34_adaptive_equalizer_observation *right)
{
	return left->decision.in_phase == right->decision.in_phase &&
	       left->decision.quadrature == right->decision.quadrature &&
	       left->epoch == right->epoch && left->lineage == right->lineage;
}

static bool decision_is_zero(
	const struct v34_adaptive_equalizer_decision *decision)
{
	return decision->observation.decision.in_phase == 0 &&
	       decision->observation.decision.quadrature == 0 &&
	       decision->observation.epoch == 0u &&
	       decision->observation.lineage == 0u &&
	       decision->target.in_phase == 0 &&
	       decision->target.quadrature == 0 &&
	       decision->decoder_power_q15 == 0u &&
	       decision->adaptation == V34_ADAPTIVE_EQUALIZER_SKIP;
}

static bool expect_action(
	const struct v34_phase3_decision_output *output,
	enum v34_phase3_decision_action action)
{
	return output->action == action &&
	       (action == V34_PHASE3_DECISION_COMMIT ||
		decision_is_zero(&output->equalizer));
}

static bool expect_commit(
	const struct v34_phase3_decision_output *output,
	const struct v34_adaptive_equalizer_observation *input,
	struct v34_analytic_sample target,
	enum v34_adaptive_equalizer_adaptation adaptation)
{
	return output->action == V34_PHASE3_DECISION_COMMIT &&
	       observations_are_equal(&output->equalizer.observation, input) &&
	       output->equalizer.target.in_phase == target.in_phase &&
	       output->equalizer.target.quadrature == target.quadrature &&
	       output->equalizer.decoder_power_q15 ==
		       TEST_DECODER_POWER_Q15 &&
	       output->equalizer.adaptation == adaptation;
}

static bool receive(
	struct v34_phase3_decision_receiver *receiver,
	struct observation_clock *clock, struct v34_analytic_sample point,
	enum v34_phase3_decision_receiver_result expected_result,
	struct v34_phase3_decision_output *output,
	struct v34_adaptive_equalizer_observation *input)
{
	*input = observation(clock, point);
	return v34_phase3_decision_receiver_observe(
		receiver, input, TEST_DECODER_POWER_Q15, output) ==
	       expected_result;
}

static uint8_t s_clockwise_rotation(unsigned int symbol)
{
	unsigned int counterclockwise = symbol < V34_PHASE3_S_SYMBOLS ?
		(symbol & 1u) :
		(2u + ((symbol - V34_PHASE3_S_SYMBOLS) & 1u));

	return (uint8_t)((4u - counterclockwise) & 3u);
}

static bool run_clean_timeline(enum v34_station_role role)
{
	const uint8_t suffix = V34_PHASE3_S_RECEIVER_MIN_S_SUFFIX_SYMBOLS;
	struct v34_phase3_decision_receiver receiver;
	struct v34_phase3_s_sequence s;
	struct v34_phase3_pp_sequence pp;
	struct v34_phase3_trn trn;
	struct v34_phase3_j_sequence j;
	struct v34_phase3_decision_handoff handoff;
	struct observation_clock clock = {
		.lineage = UINT64_C(14695981039346656037),
	};
	struct v34_analytic_sample point;
	enum v34_phase3_s_section section;
	uint8_t rotation;

	CHECK(v34_phase3_decision_receiver_init_suffix(
		      &receiver, role, 0u, suffix) ==
		      V34_PHASE3_DECISION_RECEIVER_OK &&
	      v34_phase3_s_sequence_init(&s) ==
		      V34_PHASE3_S_SEQUENCE_OK &&
	      v34_phase3_pp_sequence_init(&pp) ==
		      V34_PHASE3_PP_SEQUENCE_OK &&
	      v34_phase3_trn_init(&trn, role) == V34_PHASE3_TRN_OK,
	      "role %d fixed Phase 3 timeline did not initialize", role);
	for (unsigned int symbol = 0;
	     symbol < V34_PHASE3_S_SYMBOLS - suffix; ++symbol)
		CHECK(v34_phase3_s_sequence_next_fixed(
			      &s, &point, &section) ==
			      V34_PHASE3_S_SEQUENCE_OK,
		      "role %d S prefix stopped at %u", role, symbol);
	for (unsigned int local = 0;
	     local < suffix + V34_PHASE3_S_BAR_SYMBOLS; ++local) {
		struct v34_phase3_decision_output output = { 0 };
		struct v34_adaptive_equalizer_observation input;
		struct v34_analytic_sample target;
		enum v34_phase3_decision_receiver_result expected =
			local + 1u == suffix + V34_PHASE3_S_BAR_SYMBOLS ?
				V34_PHASE3_DECISION_RECEIVER_S_DETECTED :
				V34_PHASE3_DECISION_RECEIVER_OK;

		CHECK(v34_phase3_s_sequence_next_fixed(
			      &s, &point, &section) ==
			      V34_PHASE3_S_SEQUENCE_OK &&
		      receive(
			      &receiver, &clock, point, expected,
			      &output, &input),
		      "role %d S/S-bar observation %u failed", role, local);
		if (expected == V34_PHASE3_DECISION_RECEIVER_S_DETECTED) {
			CHECK(expect_action(
				      &output,
				      V34_PHASE3_DECISION_BEGIN_TRACKING),
			      "role %d S/S-bar boundary was not explicit", role);
			continue;
		}
		if (local >= suffix) {
			CHECK(expect_action(
				      &output, V34_PHASE3_DECISION_DISCARD),
			      "role %d S-bar evidence %u was adapted",
			      role, local);
			continue;
		}
		/* Startup tracking follows only the qualified S alternation. */
		CHECK(v34_training_constellation_fixed_point(
			      V34_CONSTELLATION_4_POINT, 0,
			      (uint8_t)((local & 1u) != 0u ? 3u : 0u),
			      &target) == V34_CONSTELLATION_OK,
		      "role %d startup S target %u failed", role, local);
		CHECK(expect_commit(
			      &output, &input, target,
			      V34_ADAPTIVE_EQUALIZER_CENTER),
		      "role %d S target %u changed", role, local);
	}
	CHECK(receiver.stage == V34_PHASE3_DECISION_RECEIVER_PP,
	      "role %d did not enter PP", role);
	for (unsigned int symbol = 0;
	     symbol < V34_PHASE3_PP_SEQUENCE_SYMBOLS; ++symbol) {
		struct v34_phase3_decision_output output = { 0 };
		struct v34_adaptive_equalizer_observation input;
		enum v34_phase3_decision_receiver_result expected =
			symbol + 1u == V34_PHASE3_PP_SEQUENCE_SYMBOLS ?
				V34_PHASE3_DECISION_RECEIVER_PP_COMPLETE :
				V34_PHASE3_DECISION_RECEIVER_OK;

		CHECK(v34_phase3_pp_sequence_next_fixed(&pp, &point) ==
			      V34_PHASE3_PP_SEQUENCE_OK &&
		      receive(
			      &receiver, &clock, point, expected,
			      &output, &input) &&
		      expect_commit(
			      &output, &input, point,
			      V34_ADAPTIVE_EQUALIZER_FULL),
		      "role %d PP target %u changed", role, symbol);
	}
	for (unsigned int symbol = 0; symbol < TEST_TRN_SYMBOLS; ++symbol) {
		struct v34_phase3_decision_output output = { 0 };
		struct v34_adaptive_equalizer_observation input;

		CHECK(v34_phase3_trn_next_fixed(
			      &trn, &point, &rotation) == V34_PHASE3_TRN_OK &&
		      receive(
			      &receiver, &clock, point,
			      V34_PHASE3_DECISION_RECEIVER_OK,
			      &output, &input) &&
		      expect_commit(
			      &output, &input, point,
			      V34_ADAPTIVE_EQUALIZER_FULL),
		      "role %d TRN target %u changed", role, symbol);
	}
	CHECK(v34_phase3_j_sequence_init_from_trn(
		      &j, &trn, V34_CONSTELLATION_16_POINT) ==
		      V34_PHASE3_J_SEQUENCE_OK,
	      "role %d J source did not initialize", role);
	for (unsigned int symbol = 0; symbol < TEST_J_SYMBOLS; ++symbol) {
		struct v34_phase3_decision_output output = { 0 };
		struct v34_adaptive_equalizer_observation input;
		enum v34_phase3_decision_receiver_result expected =
			symbol + 1u == TEST_J_SYMBOLS ?
				V34_PHASE3_DECISION_RECEIVER_J_DETECTED :
				V34_PHASE3_DECISION_RECEIVER_OK;

		CHECK(v34_phase3_j_sequence_next_fixed(
			      &j, &point, &rotation) ==
			      V34_PHASE3_J_SEQUENCE_OK &&
		      receive(
			      &receiver, &clock, point, expected,
			      &output, &input) &&
		      expect_commit(
			      &output, &input, point,
			      V34_ADAPTIVE_EQUALIZER_FULL),
		      "role %d J target %u changed", role, symbol);
	}
	CHECK(v34_phase3_decision_receiver_is_complete(&receiver) &&
	      v34_phase3_decision_receiver_handoff(&receiver, &handoff) ==
		      V34_PHASE3_DECISION_RECEIVER_OK &&
	      v34_phase3_decision_handoff_is_valid(&handoff) &&
	      handoff.j.preceding_symbols == TEST_TRN_SYMBOLS &&
	      handoff.j.sequence.remote_constellation ==
		      V34_CONSTELLATION_16_POINT,
	      "role %d fixed Phase 3 handoff changed", role);
	return true;
}

static bool test_clean_timelines(void)
{
	return run_clean_timeline(V34_CALLING_STATION) &&
	       run_clean_timeline(V34_ANSWERING_STATION);
}

static bool test_md_and_delayed_marker(void)
{
	struct v34_phase3_decision_receiver receiver;
	struct v34_phase3_s_sequence s;
	struct observation_clock clock = {
		.lineage = UINT64_C(14695981039346656037),
	};
	struct v34_analytic_sample point;
	enum v34_phase3_s_section section;

	CHECK(v34_phase3_decision_receiver_init(
		      &receiver, V34_CALLING_STATION,
		      TEST_MD_SYMBOLS) == V34_PHASE3_DECISION_RECEIVER_OK &&
	      v34_phase3_s_sequence_init(&s) ==
		      V34_PHASE3_S_SEQUENCE_OK,
	      "MD decision receiver did not initialize");
	for (unsigned int symbol = 0;
	     symbol < V34_PHASE3_S_SEQUENCE_SYMBOLS; ++symbol) {
		struct v34_phase3_decision_output output = { 0 };
		struct v34_adaptive_equalizer_observation input;

		CHECK(v34_phase3_s_sequence_next_fixed(
			      &s, &point, &section) ==
			      V34_PHASE3_S_SEQUENCE_OK,
		      "MD S source stopped at %u", symbol);
		if (symbol >= V34_PHASE3_S_SYMBOLS &&
		    symbol < V34_PHASE3_S_SYMBOLS +
			     V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS) {
			uint8_t rotation = (uint8_t)(
				(s_clockwise_rotation(symbol) + 1u) & 3u);

			CHECK(v34_training_constellation_fixed_point(
				      V34_CONSTELLATION_4_POINT, 0, rotation,
				      &point) == V34_CONSTELLATION_OK,
			      "MD corrupt marker point %u failed", symbol);
		}
		CHECK(receive(
			      &receiver, &clock, point,
			      V34_PHASE3_DECISION_RECEIVER_OK,
			      &output, &input),
		      "MD S observation %u failed", symbol);
	}
	CHECK(receiver.s.detection_pending,
	      "MD transition error did not retain a candidate");
	for (unsigned int confirmation = 0;
	     confirmation < V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS;
	     ++confirmation) {
		struct v34_phase3_decision_output output = { 0 };
		struct v34_adaptive_equalizer_observation input;
		enum v34_phase3_decision_receiver_result expected =
			confirmation + 1u ==
				V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS ?
				V34_PHASE3_DECISION_RECEIVER_S_DETECTED :
				V34_PHASE3_DECISION_RECEIVER_OK;

		point = (struct v34_analytic_sample){ -32000, -32000 };
		CHECK(receive(
			      &receiver, &clock, point, expected,
			      &output, &input) &&
		      expect_action(
			      &output, V34_PHASE3_DECISION_DISCARD),
		      "MD marker confirmation %u changed", confirmation);
	}
	CHECK(receiver.stage == V34_PHASE3_DECISION_RECEIVER_MD &&
	      receiver.md_symbols_remaining ==
		      TEST_MD_SYMBOLS -
			      V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS,
	      "post-marker MD symbols were not excluded");
	for (unsigned int symbol = 0;
	     symbol < TEST_MD_SYMBOLS -
		      V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS;
	     ++symbol) {
		struct v34_phase3_decision_output output = { 0 };
		struct v34_adaptive_equalizer_observation input;
		enum v34_phase3_decision_receiver_result expected =
			symbol + 1u == TEST_MD_SYMBOLS -
					       V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS ?
				V34_PHASE3_DECISION_RECEIVER_MD_COMPLETE :
				V34_PHASE3_DECISION_RECEIVER_OK;
		enum v34_phase3_decision_action action =
			expected == V34_PHASE3_DECISION_RECEIVER_MD_COMPLETE ?
				V34_PHASE3_DECISION_RESTART_RECEIVE_PATH :
				V34_PHASE3_DECISION_DISCARD;

		point = (struct v34_analytic_sample){
			.in_phase = (int16_t)(symbol * 211u),
			.quadrature = (int16_t)(-((int)symbol * 173)),
		};
		CHECK(receive(
			      &receiver, &clock, point, expected,
			      &output, &input) && expect_action(&output, action),
		      "MD observation %u changed", symbol);
	}
	CHECK(receiver.stage ==
		      V34_PHASE3_DECISION_RECEIVER_REPEATED_S &&
	      receiver.md_symbols_remaining == 0u &&
	      receiver.s.s_symbols ==
		      V34_PHASE3_S_RECEIVER_MIN_S_SUFFIX_SYMBOLS,
	      "MD completion did not begin repeated S");
	CHECK(v34_phase3_s_sequence_init(&s) ==
		      V34_PHASE3_S_SEQUENCE_OK,
	      "repeated S source did not initialize");
	for (unsigned int symbol = 0;
	     symbol < V34_PHASE3_S_SYMBOLS -
		      V34_PHASE3_S_RECEIVER_MIN_S_SUFFIX_SYMBOLS;
	     ++symbol)
		CHECK(v34_phase3_s_sequence_next_fixed(
			      &s, &point, &section) ==
			      V34_PHASE3_S_SEQUENCE_OK,
		      "repeated S discarded prefix %u stopped", symbol);
	for (unsigned int symbol = 0;
	     symbol < V34_PHASE3_S_RECEIVER_MIN_S_SUFFIX_SYMBOLS +
		      V34_PHASE3_S_BAR_SYMBOLS;
	     ++symbol) {
		struct v34_phase3_decision_output output = { 0 };
		struct v34_adaptive_equalizer_observation input;
		enum v34_phase3_decision_receiver_result expected =
			symbol + 1u ==
				V34_PHASE3_S_RECEIVER_MIN_S_SUFFIX_SYMBOLS +
					V34_PHASE3_S_BAR_SYMBOLS ?
				V34_PHASE3_DECISION_RECEIVER_REPEATED_S_DETECTED :
				V34_PHASE3_DECISION_RECEIVER_OK;
		enum v34_phase3_decision_action action =
			expected ==
					V34_PHASE3_DECISION_RECEIVER_REPEATED_S_DETECTED ?
				V34_PHASE3_DECISION_BEGIN_TRACKING :
				symbol >=
					V34_PHASE3_S_RECEIVER_MIN_S_SUFFIX_SYMBOLS ?
					V34_PHASE3_DECISION_DISCARD :
					V34_PHASE3_DECISION_COMMIT;

		CHECK(v34_phase3_s_sequence_next_fixed(
			      &s, &point, &section) ==
			      V34_PHASE3_S_SEQUENCE_OK &&
		      receive(
			      &receiver, &clock, point, expected,
			      &output, &input) &&
		      expect_action(&output, action),
		      "repeated S observation %u changed", symbol);
	}
	CHECK(receiver.stage == V34_PHASE3_DECISION_RECEIVER_PP,
	      "repeated S did not begin PP");
	return true;
}

static bool test_delayed_marker_pp_alignment(void)
{
	struct v34_phase3_decision_receiver receiver;
	struct v34_phase3_s_sequence s;
	struct v34_phase3_pp_sequence pp;
	struct observation_clock clock = {
		.lineage = UINT64_C(14695981039346656037),
	};
	struct v34_analytic_sample point;
	enum v34_phase3_s_section section;
	unsigned int skipped = 0;
	bool detected = false;

	CHECK(v34_phase3_decision_receiver_init(
		      &receiver, V34_ANSWERING_STATION, 0u) ==
		      V34_PHASE3_DECISION_RECEIVER_OK &&
	      v34_phase3_s_sequence_init(&s) ==
		      V34_PHASE3_S_SEQUENCE_OK &&
	      v34_phase3_pp_sequence_init(&pp) ==
		      V34_PHASE3_PP_SEQUENCE_OK,
	      "delayed-marker fixture did not initialize");
	for (unsigned int symbol = 0;
	     symbol < V34_PHASE3_S_SEQUENCE_SYMBOLS; ++symbol) {
		struct v34_phase3_decision_output output = { 0 };
		struct v34_adaptive_equalizer_observation input;

		CHECK(v34_phase3_s_sequence_next_fixed(
			      &s, &point, &section) ==
			      V34_PHASE3_S_SEQUENCE_OK,
		      "delayed-marker S source stopped at %u", symbol);
		if (symbol >= V34_PHASE3_S_SYMBOLS &&
		    symbol < V34_PHASE3_S_SYMBOLS +
				     V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS) {
			uint8_t rotation = (uint8_t)(
				(s_clockwise_rotation(symbol) + 1u) & 3u);

			CHECK(v34_training_constellation_fixed_point(
				      V34_CONSTELLATION_4_POINT, 0, rotation,
				      &point) == V34_CONSTELLATION_OK,
			      "delayed-marker error %u failed", symbol);
		}
		CHECK(receive(
			      &receiver, &clock, point,
			      V34_PHASE3_DECISION_RECEIVER_OK,
			      &output, &input),
		      "delayed-marker S observation %u failed", symbol);
	}
	CHECK(receiver.s.detection_pending,
	      "delayed-marker candidate was not retained");
	while (!detected &&
	       skipped < V34_PHASE3_S_RECEIVER_MAX_S_BAR_ERRORS) {
		struct v34_phase3_decision_output output = { 0 };
		struct v34_adaptive_equalizer_observation input;
		enum v34_phase3_decision_receiver_result result;

		CHECK(v34_phase3_pp_sequence_next_fixed(&pp, &point) ==
			      V34_PHASE3_PP_SEQUENCE_OK,
		      "delayed-marker PP prefix stopped at %u", skipped);
		skipped++;
		input = observation(&clock, point);
		result = v34_phase3_decision_receiver_observe(
			&receiver, &input, TEST_DECODER_POWER_Q15, &output);
		if (result == V34_PHASE3_DECISION_RECEIVER_S_DETECTED) {
			detected = true;
		} else {
			CHECK(result == V34_PHASE3_DECISION_RECEIVER_OK,
			      "delayed-marker PP prefix %u failed", skipped);
		}
		CHECK(expect_action(
			      &output,
			      detected ? V34_PHASE3_DECISION_BEGIN_TRACKING :
					 V34_PHASE3_DECISION_DISCARD),
		      "delayed-marker PP prefix %u was adapted", skipped);
	}
	CHECK(detected && receiver.stage == V34_PHASE3_DECISION_RECEIVER_PP &&
	      receiver.pp.emitted_symbols == skipped,
	      "delayed marker did not preserve the PP cursor");
	{
		struct v34_phase3_decision_output output = { 0 };
		struct v34_adaptive_equalizer_observation input;

		CHECK(v34_phase3_pp_sequence_next_fixed(&pp, &point) ==
			      V34_PHASE3_PP_SEQUENCE_OK &&
		      receive(
			      &receiver, &clock, point,
			      V34_PHASE3_DECISION_RECEIVER_OK,
			      &output, &input) &&
		      expect_commit(
			      &output, &input, point,
			      V34_ADAPTIVE_EQUALIZER_FULL),
		      "first post-marker PP target was misaligned");
	}
	return true;
}

static bool test_validation_and_transactionality(void)
{
	struct v34_phase3_decision_receiver receiver;
	struct v34_phase3_decision_receiver scaled;
	struct v34_phase3_decision_receiver saved;
	struct v34_phase3_decision_receiver corrupt;
	struct v34_phase3_decision_output output = {
		.action = V34_PHASE3_DECISION_RESTART_RECEIVE_PATH,
	};
	struct v34_phase3_decision_output saved_output = output;
	struct v34_adaptive_equalizer_observation input = {
		.decision = { 2289, 2289 },
		.epoch = 1,
		.lineage = 2,
	};
	struct v34_phase3_decision_handoff handoff = {
		.s = { 3, 7, 4 },
	};
	struct v34_phase3_decision_handoff saved_handoff = handoff;

	CHECK(v34_phase3_decision_receiver_init(
		      &receiver, V34_CALLING_STATION, 0u) ==
		      V34_PHASE3_DECISION_RECEIVER_OK,
	      "transaction fixture did not initialize");
	scaled = receiver;
	saved = receiver;
	{
		struct v34_phase3_decision_output scaled_output = { 0 };

		CHECK(v34_phase3_decision_receiver_observe(
			      &scaled, &input, 0x4000u, &scaled_output) ==
			      V34_PHASE3_DECISION_RECEIVER_OK &&
		      scaled_output.action == V34_PHASE3_DECISION_COMMIT &&
		      scaled_output.equalizer.decoder_power_q15 == 0x4000u,
		      "aggregate training power was not retained");
	}
	CHECK(v34_phase3_decision_receiver_init(
		      NULL, V34_CALLING_STATION, 0u) ==
		      V34_PHASE3_DECISION_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase3_decision_receiver_init(
		      &receiver, (enum v34_station_role)99, 0u) ==
		      V34_PHASE3_DECISION_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase3_decision_receiver_init(
		      &receiver, V34_CALLING_STATION, 1u) ==
		      V34_PHASE3_DECISION_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase3_decision_receiver_init(
		      &receiver, V34_CALLING_STATION,
		      V34_PHASE3_DECISION_RECEIVER_MAX_MD_SYMBOLS + 1u) ==
		      V34_PHASE3_DECISION_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
	      "invalid initialization changed the receiver");
	CHECK(v34_phase3_decision_receiver_observe(
		      NULL, &input, TEST_DECODER_POWER_Q15, &output) ==
		      V34_PHASE3_DECISION_RECEIVER_INVALID_STATE &&
	      v34_phase3_decision_receiver_observe(
		      &receiver, NULL, TEST_DECODER_POWER_Q15, &output) ==
		      V34_PHASE3_DECISION_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase3_decision_receiver_observe(
		      &receiver, &input, TEST_DECODER_POWER_Q15, NULL) ==
		      V34_PHASE3_DECISION_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase3_decision_receiver_observe(
		      &receiver,
		      (const struct v34_adaptive_equalizer_observation *)&receiver,
		      TEST_DECODER_POWER_Q15, &output) ==
		      V34_PHASE3_DECISION_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase3_decision_receiver_observe(
		      &receiver, &input, TEST_DECODER_POWER_Q15,
		      (struct v34_phase3_decision_output *)&receiver) ==
		      V34_PHASE3_DECISION_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase3_decision_receiver_observe(
		      &receiver, &input, TEST_DECODER_POWER_Q15,
		      (struct v34_phase3_decision_output *)&input) ==
		      V34_PHASE3_DECISION_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase3_decision_receiver_observe(
		      &receiver, &input, 0u, &output) ==
		      V34_PHASE3_DECISION_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase3_decision_receiver_observe(
		      &receiver, &input, 0x8000u, &output) ==
		      V34_PHASE3_DECISION_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
	      memcmp(&output, &saved_output, sizeof(output)) == 0,
	      "invalid observation changed owner or output");
	input.epoch = 0u;
	CHECK(v34_phase3_decision_receiver_observe(
		      &receiver, &input, TEST_DECODER_POWER_Q15, &output) ==
		      V34_PHASE3_DECISION_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
	      memcmp(&output, &saved_output, sizeof(output)) == 0,
	      "zero observation lineage changed owner or output");
	corrupt = receiver;
	corrupt.stage = (enum v34_phase3_decision_receiver_stage)99;
	CHECK(!v34_phase3_decision_receiver_is_valid(NULL) &&
	      !v34_phase3_decision_receiver_is_valid(&corrupt) &&
	      v34_phase3_decision_receiver_observe(
		      &corrupt, &input, TEST_DECODER_POWER_Q15, &output) ==
		      V34_PHASE3_DECISION_RECEIVER_INVALID_STATE &&
	      v34_phase3_decision_receiver_handoff(
		      &receiver, &handoff) ==
		      V34_PHASE3_DECISION_RECEIVER_INVALID_STATE &&
	      memcmp(&handoff, &saved_handoff, sizeof(handoff)) == 0,
	      "invalid owner or early handoff changed state");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "clean fixed timelines", test_clean_timelines },
		{ "MD and delayed marker", test_md_and_delayed_marker },
		{ "delayed marker PP alignment",
		  test_delayed_marker_pp_alignment },
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
