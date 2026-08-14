// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>
#include <v90_phase3_analog_line_rx.h>
#include <v90_phase3_analog_line_tx.h>
#include <v90_phase3_return_line_rx.h>
#include <v90_phase3_return_line_tx.h>

#include "test.h"

#include <math.h>
#include <string.h>

enum {
	TRN_SYMBOLS = V34_PHASE3_TRN_MIN_SYMBOLS + 173,
	RETURN_SILENCE_SYMBOLS = 350,
	RETURN_S_FINISH_SYMBOL = V34_PHASE3_S_SYMBOLS + 17,
	POST_POINT_LIMIT =
		2 * V34_TRAINING_LINE_PULSE_SPAN_SYMBOLS +
		V34_EQUALIZER_DECISION_DELAY,
	SAMPLE_LIMIT = 40000,
	NOMINAL_AMPLITUDE = 6000,
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

static struct v90_phase3_analog_rx_configuration configuration(void)
{
	return (struct v90_phase3_analog_rx_configuration){
		.s_gain_adaptation = 0.15,
		.pp_equalizer_adaptation = 0.2,
		.tracking_equalizer_adaptation = 0.05,
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
	struct v90_phase3_analog_rx_configuration config = configuration();
	struct v90_phase3_analog_line_rx rx;
	bool started_ja = false;

	if (v90_phase3_analog_line_tx_init(
		    tx, &selected, NOMINAL_AMPLITUDE) !=
		    V90_PHASE3_ANALOG_LINE_TX_OK ||
	    v90_phase3_analog_line_rx_init(&rx, &selected, &config) !=
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

static bool run_line(enum g711_law law)
{
	struct v90_phase3_analog_line_tx prefix_tx;
	struct v90_phase3_analog_line_handoff prefix_handoff;
	struct v90_phase3_return_line_tx return_tx;
	struct v90_phase3_return_line_rx return_rx;
	struct v90_phase3_return_line_handoff return_handoff;
	struct v34_training_line line;
	uint32_t finish_ja_at;
	unsigned int s_events = 0;
	unsigned int s_bar_events = 0;
	unsigned int post_points = 0;
	bool finish_ja_requested = false;
	bool started_s = false;
	bool finished_s = false;

	CHECK(establish_ja(law, &prefix_tx, &prefix_handoff) &&
	      v90_phase3_return_line_rx_init(
		      &return_rx, &prefix_handoff, 0.05) ==
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
	for (uint32_t index = 0; index < SAMPLE_LIMIT; ++index) {
		int16_t sample;
		enum v90_phase3_return_line_rx_result result;

		if (!started_s &&
		    return_tx.points.silence_symbols == RETURN_SILENCE_SYMBOLS) {
			CHECK(v90_phase3_return_line_tx_start_s(&return_tx) ==
				      V90_PHASE3_RETURN_LINE_TX_OK,
			      "law %d could not start return S", law);
			started_s = true;
		}
		if (started_s && !finished_s &&
		    return_tx.points.s_symbols == RETURN_S_FINISH_SYMBOL) {
			CHECK(v90_phase3_return_line_tx_finish_s(&return_tx) ==
				      V90_PHASE3_RETURN_LINE_TX_OK,
			      "law %d could not finish return S", law);
			finished_s = true;
		}
		if (v90_phase3_return_line_tx_is_handoff_ready(&return_tx))
			break;
		CHECK(v90_phase3_return_line_tx_sample(&return_tx, &sample) ==
			      V90_PHASE3_RETURN_LINE_TX_OK,
		      "law %d return line stopped", law);
		result = v90_phase3_return_line_rx_sample(
			&return_rx, quantize(law, sample));
		if (result == V90_PHASE3_RETURN_LINE_RX_S_DETECTED)
			s_events++;
		else
			CHECK(result == V90_PHASE3_RETURN_LINE_RX_OK,
			      "law %d return line reported %s early", law,
			      v90_phase3_return_line_rx_result_name(result));
	}
	CHECK(started_s && finished_s && s_events == 1 &&
	      v90_phase3_return_line_tx_handoff(&return_tx, &line) ==
		      V90_PHASE3_RETURN_LINE_TX_OK,
	      "law %d did not preserve the S-bar line boundary", law);
	for (uint32_t sample_index = 0;
	     sample_index < SAMPLE_LIMIT && s_bar_events == 0; ++sample_index) {
		struct qam_point point;
		const struct qam_point *supplied = NULL;
		int16_t sample;
		bool accepting = v34_training_line_needs_point(&line);
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
			      &line, supplied, &sample, &accepted) ==
			      V34_TRAINING_LINE_OK &&
		      accepted == accepting,
		      "law %d could not continue the Phase 4 line", law);
		if (accepted)
			post_points++;
		result = v90_phase3_return_line_rx_sample(
			&return_rx, quantize(law, sample));
		if (result == V90_PHASE3_RETURN_LINE_RX_S_BAR_DETECTED)
			s_bar_events++;
		else
			CHECK(result == V90_PHASE3_RETURN_LINE_RX_OK,
			      "law %d Phase 4 boundary reported %s", law,
			      v90_phase3_return_line_rx_result_name(result));
	}
	CHECK(s_bar_events == 1 &&
	      post_points >= V34_EQUALIZER_DECISION_DELAY &&
	      !v90_phase3_return_line_rx_has_failed(&return_rx) &&
	      v90_phase3_return_line_rx_handoff(
		      &return_rx, &return_handoff) ==
		      V90_PHASE3_RETURN_LINE_RX_OK &&
	      v90_phase3_return_line_handoff_is_valid(&return_handoff),
	      "law %d did not recover the complete return line", law);
	{
		struct v90_phase3_return_line_rx saved = return_rx;

		CHECK(v90_phase3_return_line_rx_sample(&return_rx, 0) ==
			      V90_PHASE3_RETURN_LINE_RX_COMPLETE &&
		      memcmp(&return_rx, &saved, sizeof(return_rx)) == 0,
		      "law %d complete receiver consumed a sample", law);
	}
	return true;
}

static bool test_g711_lines(void)
{
	CHECK(run_line(G711_LAW_PCMU) && run_line(G711_LAW_PCMA),
	      "G.711 return lines failed");
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
	CHECK(v90_phase3_return_line_rx_init(&rx, &source, NAN) ==
		      V90_PHASE3_RETURN_LINE_RX_INVALID_ARGUMENT &&
	      memcmp(&rx, &saved, sizeof(rx)) == 0,
	      "invalid line configuration changed state");
	CHECK(v90_phase3_return_line_rx_init(&rx, &source, 0.05) ==
		      V90_PHASE3_RETURN_LINE_RX_OK,
	      "valid return line receiver did not initialize");
	memset(&handoff, 0x5a, sizeof(handoff));
	saved_handoff = handoff;
	CHECK(v90_phase3_return_line_rx_sample(NULL, 0) ==
		      V90_PHASE3_RETURN_LINE_RX_INVALID_ARGUMENT &&
	      v90_phase3_return_line_rx_handoff(&rx, &handoff) ==
		      V90_PHASE3_RETURN_LINE_RX_INVALID_STATE &&
	      memcmp(&handoff, &saved_handoff, sizeof(handoff)) == 0,
	      "invalid return-line call changed its destination");
	rx.demodulator.tracking_window_symbols++;
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
