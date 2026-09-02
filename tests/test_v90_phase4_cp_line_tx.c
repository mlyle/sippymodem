// SPDX-License-Identifier: BSD-3-Clause

#include <v90_phase4_cp_line_tx.h>
#include <v90_phase4_e_line_tx.h>

#include <v90_phase3.h>

#include "test.h"

#include <string.h>

enum {
	NOMINAL_AMPLITUDE = 6000,
	SAMPLE_LIMIT = 10000,
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

static void add_ucode(
	struct pcm_constellation *constellation, uint8_t ucode)
{
	constellation->ucode_mask[ucode / 8u] |=
		(uint8_t)(1u << (ucode % 8u));
}

static struct v90_cp_parameters parameters(
	enum v90_cp_kind kind, uint8_t variant)
{
	struct v90_cp_parameters selected = {
		.kind = kind,
		.law = variant == 0 ? G711_LAW_PCMU : G711_LAW_PCMA,
		.upstream_rate_mask = (uint16_t)(0x1555u ^ variant),
		.trn1d_ratio_q3_13 = (uint16_t)(0x2000u + variant),
		.shaping_a1_q1_6 = (int8_t)(-16 - variant),
		.shaping_a2_q1_6 = (int8_t)(8 + variant),
		.shaping_b1_q1_6 = (int8_t)(-1 - variant),
		.shaping_b2_q1_6 = (int8_t)(31 - variant),
		.selected_rate_index = (uint8_t)(1u + variant),
		.sign_redundancy = (uint8_t)(variant % 4u),
		.spectral_lookahead = (uint8_t)((variant + 1u) % 4u),
		.constellation_count = 1,
		.request_silence = variant != 0,
		.acknowledges_peer = variant != 0,
	};

	add_ucode(&selected.transmit_constellations[0], variant);
	add_ucode(&selected.transmit_constellations[0], 127u - variant);
	return selected;
}

static struct qam_point warmup_point(size_t point_index)
{
	return (struct qam_point){
		.in_phase = point_index % 2u == 0 ? 0.5 : -0.5,
		.quadrature = point_index % 3u == 0 ? 0.5 : -0.5,
	};
}

static bool advance_to_nontrivial_boundary(
	struct v34_training_line *line)
{
	size_t accepted_points = 0;

	for (size_t sample_index = 0;
	     sample_index < SAMPLE_LIMIT; ++sample_index) {
		struct qam_point point = warmup_point(accepted_points);
		bool accepting = v34_training_line_needs_point(line);
		bool accepted;
		int16_t sample;

		if (accepted_points >= 25u && accepting)
			return true;
		if (v34_training_line_sample(
			    line, accepting ? &point : NULL,
			    &sample, &accepted) != V34_TRAINING_LINE_OK ||
		    accepted != accepting)
			return false;
		if (accepted)
			accepted_points++;
	}
	return false;
}

static bool model_sample(
	struct v34_training_line *line,
	struct v90_phase4_cp_tx *symbols, int16_t *sample)
{
	struct qam_point point;
	const struct qam_point *supplied = NULL;
	bool accepting = v34_training_line_needs_point(line);
	bool accepted;

	if (accepting) {
		uint8_t quarter_point;
		uint8_t clockwise_quarter_turns;
		bool record_complete;

		if (v90_phase4_cp_tx_next(
			    symbols, &point, &quarter_point,
			    &clockwise_quarter_turns, &record_complete) !=
		    V90_PHASE4_CP_TX_OK)
			return false;
		supplied = &point;
	}
	return v34_training_line_sample(
		       line, supplied, sample, &accepted) ==
		       V34_TRAINING_LINE_OK &&
	       accepted == accepting;
}

static bool model_e_sample(
	struct v34_training_line *line,
	struct v34_phase4_e_sequence *sequence, int16_t *sample)
{
	struct qam_point point;
	const struct qam_point *supplied = NULL;
	bool accepting = v34_training_line_needs_point(line);
	bool accepted;

	if (accepting) {
		uint8_t quarter_point;
		uint8_t clockwise_quarter_turns;

		if (v34_phase4_e_sequence_next(
			    sequence, &point, &quarter_point,
			    &clockwise_quarter_turns) !=
		    V34_PHASE4_E_SEQUENCE_OK)
			return false;
		supplied = &point;
	}
	return v34_training_line_sample(
		       line, supplied, sample, &accepted) ==
		       V34_TRAINING_LINE_OK &&
	       accepted == accepting;
}

static bool finish_cp_record(struct v90_phase4_cp_tx *symbols)
{
	while (!v90_phase4_cp_tx_record_is_complete(symbols)) {
		struct qam_point point;
		uint8_t quarter_point;
		uint8_t clockwise_quarter_turns;
		bool complete;

		if (v90_phase4_cp_tx_next(
			    symbols, &point, &quarter_point,
			    &clockwise_quarter_turns, &complete) !=
		    V90_PHASE4_CP_TX_OK)
			return false;
	}
	return true;
}

static bool verify_online_resume(
	enum v34_constellation_size constellation, uint8_t variant)
{
	struct v34_phase3_profile selected = profile();
	struct v90_cp_parameters training =
		parameters(V90_CP_TRAINING, variant);
	struct v90_cp_parameters preceding =
		parameters(V90_CP_DATA, variant);
	struct v90_cp_parameters next_parameters = preceding;
	struct v34_training_line online_line;
	struct v34_training_line model_line;
	struct v90_phase4_cp_tx control_symbols;
	struct v90_phase4_cp_tx model_symbols;
	struct v90_phase4_cp_line_tx resumed;
	bool complete = false;

	next_parameters.upstream_rate_mask ^= 0x0200u;
	next_parameters.acknowledges_peer = true;
	CHECK(v34_training_line_init(
		      &online_line, &selected, V90_PHASE3_SAMPLE_RATE,
		      NOMINAL_AMPLITUDE) == V34_TRAINING_LINE_OK &&
	      advance_to_nontrivial_boundary(&online_line) &&
	      v90_phase4_cp_tx_init(
		      &control_symbols, V34_ANSWERING_STATION,
		      constellation, &training) == V90_PHASE4_CP_TX_OK &&
	      finish_cp_record(&control_symbols) &&
	      v90_phase4_cp_tx_start_next_record(
		      &control_symbols, &preceding) == V90_PHASE4_CP_TX_OK &&
	      finish_cp_record(&control_symbols),
	      "online CP resume fixture did not reach its boundary");
	model_line = online_line;
	model_symbols = control_symbols;
	CHECK(v90_phase4_cp_tx_start_next_record(
		      &model_symbols, &next_parameters) == V90_PHASE4_CP_TX_OK &&
	      v90_phase4_cp_line_tx_resume_online(
		      &resumed, &online_line, &control_symbols,
		      &next_parameters) == V90_PHASE4_CP_LINE_TX_OK &&
	      memcmp(&resumed.line, &online_line, sizeof(online_line)) == 0 &&
	      memcmp(&resumed.symbols, &model_symbols,
		     sizeof(model_symbols)) == 0,
	      "online CP resume did not preserve line and control histories");
	for (size_t sample_index = 0;
	     sample_index < SAMPLE_LIMIT; ++sample_index) {
		int16_t actual;
		int16_t expected;

		if (v90_phase4_cp_line_tx_is_handoff_ready(&resumed)) {
			complete = true;
			break;
		}
		CHECK(model_sample(&model_line, &model_symbols, &expected) &&
		      v90_phase4_cp_line_tx_sample(&resumed, &actual) ==
			      V90_PHASE4_CP_LINE_TX_OK &&
		      actual == expected &&
		      memcmp(&resumed.line, &model_line,
			     sizeof(model_line)) == 0 &&
		      memcmp(&resumed.symbols, &model_symbols,
			     sizeof(model_symbols)) == 0,
		      "online CP continuation diverged at sample %zu",
		      sample_index);
	}
	CHECK(complete &&
	      v90_phase4_cp_line_tx_record_is_complete(&resumed),
	      "online CP continuation did not complete");
	return true;
}

static bool test_online_resume(void)
{
	for (uint8_t variant = 0; variant < 2u; ++variant) {
		CHECK(verify_online_resume(
			      V34_CONSTELLATION_4_POINT, variant),
		      "four-point online CP resume failed for variant %u",
		      variant);
		CHECK(verify_online_resume(
			      V34_CONSTELLATION_16_POINT, variant),
		      "sixteen-point online CP resume failed for variant %u",
		      variant);
	}
	return true;
}

static bool verify_continuation(
	enum v34_constellation_size constellation)
{
	struct v34_phase3_profile selected = profile();
	struct v90_cp_parameters training = parameters(V90_CP_TRAINING, 0);
	struct v90_cp_parameters data = parameters(V90_CP_DATA, 1);
	struct v34_training_line source;
	struct v34_training_line model_line;
	struct v34_training_line handoff;
	struct v34_training_line e_handoff;
	struct v90_phase4_cp_tx model_symbols;
	struct v34_phase4_e_sequence model_e;
	struct v90_phase4_cp_line_tx tx;
	struct v90_phase4_e_line_tx e;
	bool started_data = false;
	bool complete = false;

	data.request_silence = false;

	CHECK(v34_training_line_init(
		      &source, &selected, V90_PHASE3_SAMPLE_RATE,
		      NOMINAL_AMPLITUDE) == V34_TRAINING_LINE_OK &&
	      advance_to_nontrivial_boundary(&source),
	      "CP line source did not reach a continued point boundary");
	model_line = source;
	CHECK(v90_phase4_cp_tx_init(
		      &model_symbols, V34_ANSWERING_STATION,
		      constellation, &training) == V90_PHASE4_CP_TX_OK &&
	      v90_phase4_cp_line_tx_init(
		      &tx, &source, constellation, &training) ==
		      V90_PHASE4_CP_LINE_TX_OK &&
	      memcmp(&tx.line, &source, sizeof(source)) == 0 &&
	      memcmp(&tx.symbols, &model_symbols,
		     sizeof(model_symbols)) == 0,
	      "CP line did not preserve its incoming state");

	for (size_t sample_index = 0;
	     sample_index < SAMPLE_LIMIT; ++sample_index) {
		int16_t actual;
		int16_t expected;

		if (v90_phase4_cp_line_tx_is_handoff_ready(&tx)) {
			complete = true;
			break;
		}
		CHECK(model_sample(&model_line, &model_symbols, &expected) &&
		      v90_phase4_cp_line_tx_sample(&tx, &actual) ==
			      V90_PHASE4_CP_LINE_TX_OK &&
		      actual == expected &&
		      memcmp(&tx.line, &model_line,
			     sizeof(model_line)) == 0 &&
		      memcmp(&tx.symbols, &model_symbols,
			     sizeof(model_symbols)) == 0,
		      "CP waveform diverged at sample %zu", sample_index);
		if (!started_data &&
		    v90_phase4_cp_tx_record_is_complete(&model_symbols)) {
			uint32_t history = model_symbols.scrambler.history;
			uint8_t rotation =
				model_symbols.previous_clockwise_quarter_turns;

			CHECK(!v34_training_line_needs_point(&model_line) &&
			      v90_phase4_cp_line_tx_start_next_record(
				      &tx, &data) ==
				      V90_PHASE4_CP_LINE_TX_OK &&
			      v90_phase4_cp_tx_start_next_record(
				      &model_symbols, &data) ==
				      V90_PHASE4_CP_TX_OK &&
			      tx.symbols.scrambler.history == history &&
			      tx.symbols.previous_clockwise_quarter_turns ==
				      rotation,
			      "CP line did not continue at the record boundary");
			started_data = true;
		}
	}
	CHECK(started_data && complete &&
	      v90_phase4_cp_line_tx_record_is_complete(&tx) &&
	      v90_phase4_cp_line_tx_handoff(&tx, &handoff) ==
		      V90_PHASE4_CP_LINE_TX_OK &&
	      memcmp(&handoff, &model_line, sizeof(handoff)) == 0,
	      "CP line did not preserve its final handoff");
	CHECK(v34_phase4_e_sequence_init(
		      &model_e, &model_symbols.scrambler,
		      model_symbols.role, model_symbols.constellation,
		      model_symbols.previous_clockwise_quarter_turns) ==
		      V34_PHASE4_E_SEQUENCE_OK &&
	      v90_phase4_e_line_tx_init(&e, &tx) ==
		      V90_PHASE4_E_LINE_TX_OK &&
	      memcmp(&e.line, &model_line, sizeof(model_line)) == 0 &&
	      memcmp(&e.sequence, &model_e, sizeof(model_e)) == 0 &&
	      e.cp.kind == V90_CP_DATA && e.cp.acknowledges_peer &&
	      !e.cp.request_silence,
	      "E did not preserve the completed CP histories");
	for (size_t sample_index = 0;
	     sample_index < SAMPLE_LIMIT &&
	     !v90_phase4_e_line_tx_is_complete(&e); ++sample_index) {
		int16_t actual;
		int16_t expected;

		CHECK(model_e_sample(&model_line, &model_e, &expected) &&
		      v90_phase4_e_line_tx_sample(&e, &actual) ==
			      V90_PHASE4_E_LINE_TX_OK &&
		      actual == expected &&
		      memcmp(&e.line, &model_line, sizeof(model_line)) == 0 &&
		      memcmp(&e.sequence, &model_e, sizeof(model_e)) == 0,
		      "E waveform diverged at sample %zu", sample_index);
	}
	CHECK(v90_phase4_e_line_tx_is_complete(&e) &&
	      v34_phase4_e_sequence_is_complete(&model_e) &&
	      v90_phase4_e_line_tx_handoff(&e, &e_handoff) ==
		      V90_PHASE4_E_LINE_TX_OK &&
	      memcmp(&e_handoff, &model_line, sizeof(model_line)) == 0,
	      "E did not preserve its final line handoff");
	{
		struct v90_phase4_cp_line_tx saved = tx;
		struct v90_phase4_e_line_tx saved_e = e;
		int16_t sample = 1234;

		CHECK(v90_phase4_cp_line_tx_sample(&tx, &sample) ==
			      V90_PHASE4_CP_LINE_TX_COMPLETE &&
		      memcmp(&tx, &saved, sizeof(tx)) == 0 &&
		      sample == 1234,
		      "completed CP line emitted an extra sample");
		CHECK(v90_phase4_e_line_tx_sample(&e, &sample) ==
			      V90_PHASE4_E_LINE_TX_COMPLETE &&
		      memcmp(&e, &saved_e, sizeof(e)) == 0 &&
		      sample == 1234,
		      "completed E line emitted an extra sample");
	}
	return true;
}

static bool test_continuous_waveforms(void)
{
	CHECK(verify_continuation(V34_CONSTELLATION_4_POINT),
	      "four-point CP waveform failed");
	CHECK(verify_continuation(V34_CONSTELLATION_16_POINT),
	      "sixteen-point CP waveform failed");
	return true;
}

static bool test_transactional_contracts(void)
{
	struct v34_phase3_profile selected = profile();
	struct v90_cp_parameters training = parameters(V90_CP_TRAINING, 0);
	struct v90_cp_parameters data = parameters(V90_CP_DATA, 1);
	struct v34_training_line line;
	struct v34_training_line saved_line;
	struct v90_phase4_cp_line_tx tx;
	struct v90_phase4_cp_line_tx saved;
	struct v90_phase4_cp_line_tx untouched;
	struct v90_phase4_cp_line_tx untouched_saved;
	struct v90_phase4_e_line_tx e_untouched;
	struct v90_phase4_e_line_tx e_untouched_saved;
	struct qam_point point = warmup_point(0);
	bool accepted;
	int16_t sample = 777;

	CHECK(v34_training_line_init(
		      &line, &selected, V90_PHASE3_SAMPLE_RATE,
		      NOMINAL_AMPLITUDE) == V34_TRAINING_LINE_OK,
	      "CP line validation fixture did not initialize");
	memset(&untouched, 0xa5, sizeof(untouched));
	untouched_saved = untouched;
	memset(&e_untouched, 0x5a, sizeof(e_untouched));
	e_untouched_saved = e_untouched;
	saved_line = line;
	saved_line.profile.local_role = V34_CALLING_STATION;
	CHECK(v90_phase4_cp_line_tx_init(
		      NULL, &line, V34_CONSTELLATION_4_POINT,
		      &training) == V90_PHASE4_CP_LINE_TX_INVALID_ARGUMENT &&
	      v90_phase4_cp_line_tx_init(
		      &untouched, &saved_line,
		      V34_CONSTELLATION_4_POINT, &training) ==
		      V90_PHASE4_CP_LINE_TX_INVALID_ARGUMENT &&
	      v90_phase4_cp_line_tx_init(
		      &untouched, &line,
		      (enum v34_constellation_size)8, &training) ==
		      V90_PHASE4_CP_LINE_TX_INVALID_ARGUMENT &&
	      v90_phase4_cp_line_tx_init(
		      &untouched, &line,
		      V34_CONSTELLATION_4_POINT, &data) ==
		      V90_PHASE4_CP_LINE_TX_INVALID_PARAMETERS &&
	      memcmp(&untouched, &untouched_saved,
		     sizeof(untouched)) == 0,
	      "rejected CP line initialization mutated state");
	CHECK(v90_phase4_cp_line_tx_init(
		      &tx, &line, V34_CONSTELLATION_4_POINT,
		      &training) == V90_PHASE4_CP_LINE_TX_OK,
	      "valid CP line did not initialize");
	{
		struct v90_phase4_cp_tx control = tx.symbols;
		struct v90_phase4_cp_tx incomplete;
		struct v90_phase4_cp_line_tx resumed;
		struct v90_phase4_cp_line_tx resumed_saved;
		struct v34_training_line mid_point = line;

		CHECK(finish_cp_record(&control),
		      "resume contract cursor did not complete");
		incomplete = control;
		incomplete.bit_index = 0;
		memset(&resumed, 0xa5, sizeof(resumed));
		resumed_saved = resumed;
		CHECK(v90_phase4_cp_line_tx_resume_online(
			      &resumed, &line, &control, &data) ==
			      V90_PHASE4_CP_LINE_TX_OK,
		      "valid online resume was rejected");
		saved = resumed;
		CHECK(v90_phase4_cp_line_tx_resume_online(
			      &resumed, &resumed.line, &control, &data) ==
			      V90_PHASE4_CP_LINE_TX_INVALID_ARGUMENT &&
		      v90_phase4_cp_line_tx_resume_online(
			      &resumed, &line, &resumed.symbols, &data) ==
			      V90_PHASE4_CP_LINE_TX_INVALID_ARGUMENT &&
		      memcmp(&resumed, &saved, sizeof(resumed)) == 0,
		      "overlapping online resume changed its destination");
		CHECK(v34_training_line_sample(
			      &mid_point, &point, &sample, &accepted) ==
			      V34_TRAINING_LINE_OK && accepted &&
		      v90_phase4_cp_line_tx_resume_online(
			      &resumed_saved, &mid_point, &control, &data) ==
			      V90_PHASE4_CP_LINE_TX_INVALID_STATE &&
		      v90_phase4_cp_line_tx_resume_online(
			      &resumed_saved, &line, &incomplete, &data) ==
			      V90_PHASE4_CP_LINE_TX_INVALID_STATE &&
		      v90_phase4_cp_line_tx_resume_online(
			      &resumed_saved, &line, &control, &training) ==
			      V90_PHASE4_CP_LINE_TX_INVALID_PARAMETERS &&
		      v90_phase4_cp_line_tx_resume_online(
			      NULL, &line, &control, &data) ==
			      V90_PHASE4_CP_LINE_TX_INVALID_ARGUMENT &&
		      memcmp(&resumed_saved, &untouched_saved,
			     sizeof(resumed_saved)) == 0,
		      "rejected online resume changed its destination");
	}
	sample = 777;
	CHECK(v90_phase4_e_line_tx_init(NULL, &tx) ==
		      V90_PHASE4_E_LINE_TX_INVALID_ARGUMENT &&
	      v90_phase4_e_line_tx_init(&e_untouched, NULL) ==
		      V90_PHASE4_E_LINE_TX_INVALID_ARGUMENT &&
	      v90_phase4_e_line_tx_init(&e_untouched, &tx) ==
		      V90_PHASE4_E_LINE_TX_INVALID_STATE &&
	      memcmp(&e_untouched, &e_untouched_saved,
		     sizeof(e_untouched)) == 0,
	      "rejected E initialization changed state");
	saved = tx;
	saved_line = line;
	CHECK(v90_phase4_cp_line_tx_start_next_record(&tx, &data) ==
		      V90_PHASE4_CP_LINE_TX_RECORD_IN_PROGRESS &&
	      v90_phase4_cp_line_tx_start_next_record(&tx, NULL) ==
		      V90_PHASE4_CP_LINE_TX_INVALID_ARGUMENT &&
	      v90_phase4_cp_line_tx_sample(NULL, &sample) ==
		      V90_PHASE4_CP_LINE_TX_INVALID_ARGUMENT &&
	      v90_phase4_cp_line_tx_sample(&tx, NULL) ==
		      V90_PHASE4_CP_LINE_TX_INVALID_ARGUMENT &&
	      v90_phase4_cp_line_tx_handoff(&tx, &saved_line) ==
		      V90_PHASE4_CP_LINE_TX_INVALID_STATE &&
	      memcmp(&tx, &saved, sizeof(tx)) == 0 &&
	      memcmp(&saved_line, &line, sizeof(line)) == 0 &&
	      sample == 777,
	      "rejected CP line operation changed state or output");
	CHECK(v34_training_line_sample(
		      &line, &point, &sample, &accepted) ==
		      V34_TRAINING_LINE_OK && accepted &&
	      !v34_training_line_needs_point(&line) &&
	      v90_phase4_cp_line_tx_init(
		      &untouched, &line,
		      V34_CONSTELLATION_4_POINT, &training) ==
		      V90_PHASE4_CP_LINE_TX_INVALID_ARGUMENT &&
	      memcmp(&untouched, &untouched_saved,
		     sizeof(untouched)) == 0,
	      "CP line accepted an inexact sample boundary");
	tx.symbols.previous_clockwise_quarter_turns = 4;
	CHECK(!v90_phase4_cp_line_tx_is_valid(&tx) &&
	      !v90_phase4_cp_line_tx_record_is_complete(&tx) &&
	      !v90_phase4_cp_line_tx_is_handoff_ready(&tx) &&
	      strcmp(v90_phase4_cp_line_tx_result_name(
		     V90_PHASE4_CP_LINE_TX_CLIPPING_RISK),
		     "clipping risk") == 0 &&
	      strcmp(v90_phase4_cp_line_tx_result_name(
		     V90_PHASE4_CP_LINE_TX_INTERNAL_ERROR),
		     "internal error") == 0 &&
	      strcmp(v90_phase4_cp_line_tx_result_name(
		     (enum v90_phase4_cp_line_tx_result)99),
		     "unknown") == 0,
	      "CP line validation or result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "continuous waveforms", test_continuous_waveforms },
		{ "online resume", test_online_resume },
		{ "transactional contracts", test_transactional_contracts },
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
