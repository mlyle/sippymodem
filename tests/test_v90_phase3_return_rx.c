// SPDX-License-Identifier: BSD-3-Clause

#include <v34_constellation.h>
#include <v90_phase3_analog_tx.h>
#include <v90_phase3_return_rx.h>
#include <v90_phase3_return_tx.h>

#include "test.h"

#include <math.h>
#include <string.h>

enum {
	JA_REPETITIONS = 4,
	TRN_SYMBOLS = V34_PHASE3_TRN_MIN_SYMBOLS + 97,
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

static bool run_timeline(unsigned int quarter_turns, uint32_t finish_after)
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

	CHECK(seed_handoff(quarter_turns, &initial) && start_ja(&ja) &&
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
		    (s_symbol == 5u || s_symbol == 19u))
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
	      s_at_detection == V90_PHASE3_RETURN_RX_ONSET_SYMBOLS +
					V34_EQUALIZER_DECISION_DELAY &&
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
	CHECK(run_timeline(0, V34_PHASE3_S_SYMBOLS) &&
	      run_timeline(1, V34_PHASE3_S_SYMBOLS + 53u) &&
	      run_timeline(3, V34_PHASE3_S_SYMBOLS + 18u),
	      "generated return timelines failed");
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
