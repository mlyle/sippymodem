// SPDX-License-Identifier: BSD-3-Clause

#include "test.h"
#include "v34_state_fixture.h"

#include "v90_phase3_analog_line_tx.h"

#include <stdint.h>
#include <string.h>

enum {
	TRN_SYMBOLS = 600,
	JA_SYMBOLS = V90_PHASE3_JA_DESCRIPTOR_SYMBOLS + 9,
	SAMPLE_LIMIT = 20000,
	NOMINAL_AMPLITUDE = 6000,
};

/* Compare successfully initialized owners without training-line padding.
 * Invalid initialization destinations remain fully defined byte sentinels. */
static bool states_equal(
	const struct v90_phase3_analog_line_tx *left,
	const struct v90_phase3_analog_line_tx *right)
{
	return v34_training_lines_equal(&left->line, &right->line) &&
	       memcmp(&left->symbols, &right->symbols, sizeof(left->symbols)) == 0 &&
	       left->leading_silence_samples == right->leading_silence_samples;
}

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

static bool apply_controls(struct v90_phase3_analog_line_tx *tx)
{
	if (tx->symbols.stage == V90_PHASE3_ANALOG_TX_PREFIX &&
	    tx->symbols.sequence.prefix.stage == V34_PHASE3_TRANSMITTER_TRN &&
	    tx->symbols.sequence.prefix.trn_symbols == TRN_SYMBOLS)
		return v90_phase3_analog_line_tx_start_ja(tx) ==
		       V90_PHASE3_ANALOG_LINE_TX_OK;
	if (tx->symbols.stage == V90_PHASE3_ANALOG_TX_JA &&
	    tx->symbols.ja_symbols == JA_SYMBOLS)
		return v90_phase3_analog_line_tx_finish_ja(tx) ==
		       V90_PHASE3_ANALOG_LINE_TX_OK;
	return true;
}

static bool model_sample(
	struct v34_training_line *line,
	struct v90_phase3_analog_tx *symbols, int16_t *sample)
{
	struct qam_point point;
	const struct qam_point *supplied = NULL;
	bool accepting = v34_training_line_needs_point(line);
	bool accepted;

	if (accepting) {
		if (v90_phase3_analog_tx_next(symbols, &point) !=
		    V90_PHASE3_ANALOG_TX_OK)
			return false;
		supplied = &point;
	}
	return v34_training_line_sample(
		       line, supplied, sample, &accepted) ==
		       V34_TRAINING_LINE_OK &&
	       accepted == accepting;
}

static bool test_exact_line_timeline(void)
{
	struct v34_phase3_profile selected = profile();
	struct v90_phase3_analog_line_tx tx;
	struct v34_training_line model_line;
	struct v90_phase3_analog_tx model_symbols;
	struct v34_training_line handoff;
	struct v34_training_line unchanged;
	uint32_t waveform_samples = 0;
	bool complete = false;

	CHECK(v90_phase3_analog_line_tx_init(
		      &tx, &selected, NOMINAL_AMPLITUDE) ==
		      V90_PHASE3_ANALOG_LINE_TX_OK &&
	      v34_training_line_init(
		      &model_line, &selected, V90_PHASE3_SAMPLE_RATE,
		      NOMINAL_AMPLITUDE) == V34_TRAINING_LINE_OK &&
	      v90_phase3_analog_tx_init(&model_symbols) ==
		      V90_PHASE3_ANALOG_TX_OK &&
	      v90_phase3_analog_line_tx_is_valid(&tx),
	      "analogue Phase 3 line did not initialize");
	for (uint16_t index = 0;
	     index < V90_PHASE3_ANALOG_LEADING_SILENCE_SAMPLES; ++index) {
		int16_t sample = INT16_MAX;

		CHECK(v90_phase3_analog_line_tx_sample(&tx, &sample) ==
			      V90_PHASE3_ANALOG_LINE_TX_OK &&
		      sample == 0 && tx.line.generated_samples == 0 &&
		      tx.symbols.stage == V90_PHASE3_ANALOG_TX_PREFIX &&
		      tx.symbols.sequence.prefix.stage ==
			      V34_PHASE3_TRANSMITTER_S,
		      "leading silence changed at sample %u", index);
	}
	CHECK(tx.leading_silence_samples == 0,
	      "leading silence did not end at exactly 70 ms");
	for (uint32_t index = 0; index < SAMPLE_LIMIT; ++index) {
		int16_t actual;
		int16_t expected;

		CHECK(apply_controls(&tx),
		      "analogue line control failed at sample %u", index);
		/* Apply the same point-boundary controls without aliasing the model
		 * line into the production owner. */
		if (model_symbols.stage == V90_PHASE3_ANALOG_TX_PREFIX &&
		    model_symbols.sequence.prefix.stage ==
			    V34_PHASE3_TRANSMITTER_TRN &&
		    model_symbols.sequence.prefix.trn_symbols == TRN_SYMBOLS)
			CHECK(v90_phase3_analog_tx_start_ja(&model_symbols) ==
				      V90_PHASE3_ANALOG_TX_OK,
			      "model did not start Ja");
		if (model_symbols.stage == V90_PHASE3_ANALOG_TX_JA &&
		    model_symbols.ja_symbols == JA_SYMBOLS)
			CHECK(v90_phase3_analog_tx_finish_ja(&model_symbols) ==
				      V90_PHASE3_ANALOG_TX_OK,
			      "model did not finish Ja");
		if (v90_phase3_analog_line_tx_is_handoff_ready(&tx)) {
			complete = true;
			break;
		}
		CHECK(model_sample(&model_line, &model_symbols, &expected),
		      "analogue line model stopped at sample %u", index);
		CHECK(v90_phase3_analog_line_tx_sample(&tx, &actual) ==
			      V90_PHASE3_ANALOG_LINE_TX_OK,
		      "analogue line stopped at sample %u", index);
		CHECK(actual == expected,
		      "analogue waveform differed at sample %u: %d != %d",
		      index, actual, expected);
		CHECK(v34_training_line_is_valid(&tx.line) &&
		      tx.line.generated_samples == model_line.generated_samples,
		      "analogue line state diverged at sample %u", index);
		CHECK(memcmp(&tx.symbols, &model_symbols,
			     sizeof(model_symbols)) == 0,
		      "analogue symbol state diverged at sample %u", index);
		waveform_samples++;
	}
	CHECK(complete && waveform_samples != 0 &&
	      tx.symbols.ja_symbols == JA_SYMBOLS &&
	      tx.symbols.sequence.ja.bit_index == 18 &&
	      v90_phase3_analog_line_tx_handoff(&tx, &handoff) ==
		      V90_PHASE3_ANALOG_LINE_TX_OK &&
	      v34_training_line_is_valid(&handoff) &&
	      handoff.generated_samples == model_line.generated_samples,
	      "analogue line did not expose its exact completion boundary");
	{
		struct v90_phase3_analog_line_tx saved = tx;
		int16_t sample = 1234;

		CHECK(v90_phase3_analog_line_tx_sample(&tx, &sample) ==
			      V90_PHASE3_ANALOG_LINE_TX_COMPLETE &&
		      states_equal(&tx, &saved) && sample == 1234,
		      "complete analogue line consumed another sample");
	}
	memset(&handoff, 0xa5, sizeof(handoff));
	unchanged = handoff;
	CHECK(v90_phase3_analog_line_tx_handoff(NULL, &handoff) ==
		      V90_PHASE3_ANALOG_LINE_TX_INVALID_ARGUMENT &&
	      memcmp(&handoff, &unchanged, sizeof(handoff)) == 0,
	      "invalid analogue line handoff changed its destination");
	return true;
}

static bool test_validation(void)
{
	struct v34_phase3_profile selected = profile();
	struct v90_phase3_analog_line_tx tx;
	struct v90_phase3_analog_line_tx saved;
	struct v34_training_line line;
	struct v34_training_line saved_line;
	int16_t sample = 777;

	memset(&tx, 0xa5, sizeof(tx));
	saved = tx;
	CHECK(v90_phase3_analog_line_tx_init(NULL, &selected, 6000) ==
		      V90_PHASE3_ANALOG_LINE_TX_INVALID_ARGUMENT &&
	      v90_phase3_analog_line_tx_init(&tx, NULL, 6000) ==
		      V90_PHASE3_ANALOG_LINE_TX_INVALID_ARGUMENT,
	      "analogue line accepted null initialization");
	selected.local_role = V34_CALLING_STATION;
	CHECK(v90_phase3_analog_line_tx_init(&tx, &selected, 6000) ==
		      V90_PHASE3_ANALOG_LINE_TX_INVALID_ARGUMENT &&
	      memcmp(&tx, &saved, sizeof(tx)) == 0,
	      "analogue line accepted the digital station role");
	selected = profile();
	selected.md_length_35_ms_units = 1;
	selected.md_symbol_count = 105;
	CHECK(v90_phase3_analog_line_tx_init(&tx, &selected, 6000) ==
		      V90_PHASE3_ANALOG_LINE_TX_INVALID_ARGUMENT,
	      "analogue line accepted nonzero MD");
	selected = profile();
	CHECK(v90_phase3_analog_line_tx_init(&tx, &selected, 6000) ==
		      V90_PHASE3_ANALOG_LINE_TX_OK,
	      "analogue validation fixture did not initialize");
	saved = tx;
	memset(&line, 0xa5, sizeof(line));
	saved_line = line;
	CHECK(v90_phase3_analog_line_tx_sample(NULL, &sample) ==
		      V90_PHASE3_ANALOG_LINE_TX_INVALID_ARGUMENT &&
	      v90_phase3_analog_line_tx_sample(&tx, NULL) ==
		      V90_PHASE3_ANALOG_LINE_TX_INVALID_ARGUMENT &&
	      v90_phase3_analog_line_tx_start_ja(&tx) ==
		      V90_PHASE3_ANALOG_LINE_TX_INVALID_STATE &&
	      v90_phase3_analog_line_tx_finish_ja(&tx) ==
		      V90_PHASE3_ANALOG_LINE_TX_INVALID_STATE &&
	      v90_phase3_analog_line_tx_handoff(&tx, &line) ==
		      V90_PHASE3_ANALOG_LINE_TX_INVALID_STATE &&
	      states_equal(&tx, &saved) && sample == 777 &&
	      memcmp(&line, &saved_line, sizeof(line)) == 0,
	      "analogue line invalid call was not transactional");
	tx.leading_silence_samples =
		V90_PHASE3_ANALOG_LEADING_SILENCE_SAMPLES + 1u;
	CHECK(!v90_phase3_analog_line_tx_is_valid(&tx) &&
	      !v90_phase3_analog_line_tx_is_handoff_ready(&tx) &&
	      strcmp(v90_phase3_analog_line_tx_result_name(
		      V90_PHASE3_ANALOG_LINE_TX_CLIPPING_RISK),
		      "clipping risk") == 0 &&
	      strcmp(v90_phase3_analog_line_tx_result_name(
		      (enum v90_phase3_analog_line_tx_result)99),
		      "unknown") == 0,
	      "analogue line validation or names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "exact line timeline", test_exact_line_timeline },
		{ "validation", test_validation },
	};
	unsigned int failures = 0;

	for (size_t index = 0; index < ARRAY_LENGTH(tests); ++index) {
		fprintf(stderr, "[ RUN      ] %s\n", tests[index].name);
		if (tests[index].run())
			fprintf(stderr, "[       OK ] %s\n", tests[index].name);
		else {
			fprintf(stderr, "[  FAILED  ] %s\n", tests[index].name);
			failures++;
		}
	}
	return failures == 0 ? 0 : 1;
}
