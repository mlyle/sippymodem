// SPDX-License-Identifier: BSD-3-Clause

#include <v34_caller_training.h>

#include "test.h"

#include <string.h>

struct model_training {
	struct v34_training_line line;
	struct v34_phase3_caller phase3;
	union v34_caller_phase4_state phase4;
	uint32_t phase4_trn_symbols;
	enum v34_caller_training_stage stage;
};

static struct v34_phase3_profile profile_fixture(void)
{
	return (struct v34_phase3_profile){
		.symbol_rate_numerator = 3000,
		.symbol_rate_denominator = 1,
		.carrier_frequency_numerator = 2000,
		.carrier_frequency_denominator = 1,
		.symbol_rate = V34_SYMBOL_RATE_3000,
		.local_role = V34_CALLING_STATION,
		.preemphasis_filter_index = 8,
		.projected_data_rate_2400_bps_units = 1,
		.high_carrier = true,
	};
}

static bool model_init(
	struct model_training *model,
	const struct v34_phase3_profile *profile,
	enum v34_constellation_size constellation)
{
	struct model_training initialized = { 0 };

	if (v34_training_line_init(
		    &initialized.line, profile, 8000, 5000) !=
		    V34_TRAINING_LINE_OK ||
	    v34_phase3_caller_init(
		    &initialized.phase3, constellation) !=
		    V34_PHASE3_CALLER_OK)
		return false;
	initialized.stage = V34_CALLER_TRAINING_PHASE3;
	*model = initialized;
	return true;
}

static bool model_finish_j(struct model_training *model)
{
	if (v34_phase3_caller_finish_j(&model->phase3) !=
		    V34_PHASE3_CALLER_OK ||
	    v34_phase3_caller_start_j_prime(
		    &model->phase3, &model->phase4.j_prime) !=
		    V34_PHASE3_CALLER_OK)
		return false;
	model->stage = V34_CALLER_TRAINING_J_PRIME;
	return true;
}

static bool model_next_point(
	struct model_training *model, struct qam_point *point)
{
	switch (model->stage) {
	case V34_CALLER_TRAINING_PHASE3:
		return v34_phase3_caller_next(&model->phase3, point) ==
		       V34_PHASE3_CALLER_OK;
	case V34_CALLER_TRAINING_J_PRIME: {
		struct v34_phase4_trn trn;
		uint8_t rotation;

		if (v34_phase4_j_prime_sequence_next(
			    &model->phase4.j_prime, point, &rotation) !=
		    V34_PHASE4_J_PRIME_SEQUENCE_OK)
			return false;
		if (!v34_phase4_j_prime_sequence_is_complete(
			    &model->phase4.j_prime))
			return true;
		if (v34_phase4_trn_init(
			    &trn, V34_CALLING_STATION,
			    model->phase3.remote_constellation) !=
		    V34_PHASE4_TRN_OK)
			return false;
		model->phase4 = (union v34_caller_phase4_state){ 0 };
		model->phase4.trn = trn;
		model->stage = V34_CALLER_TRAINING_PHASE4_TRN;
		return true;
	}
	case V34_CALLER_TRAINING_PHASE4_TRN: {
		uint8_t quarter_point;
		uint8_t rotation;

		if (v34_phase4_trn_next(
			    &model->phase4.trn, point, &quarter_point,
			    &rotation) != V34_PHASE4_TRN_OK)
			return false;
		model->phase4_trn_symbols++;
		return true;
	}
	}
	return false;
}

static bool protocol_state_matches(
	const struct v34_caller_training *actual,
	const struct model_training *model)
{
	if (actual->stage != model->stage ||
	    actual->phase4_trn_symbols != model->phase4_trn_symbols ||
	    actual->phase3.stage != model->phase3.stage ||
	    actual->phase3.trn_symbols != model->phase3.trn_symbols ||
	    actual->phase3.j_symbols != model->phase3.j_symbols)
		return false;
	switch (model->stage) {
	case V34_CALLER_TRAINING_PHASE3:
		return true;
	case V34_CALLER_TRAINING_J_PRIME:
		return actual->phase4.j_prime.emitted_symbols ==
		       model->phase4.j_prime.emitted_symbols &&
		       actual->phase4.j_prime.scrambler.history ==
		       model->phase4.j_prime.scrambler.history;
	case V34_CALLER_TRAINING_PHASE4_TRN:
		return actual->phase4.trn.scrambler.history ==
		       model->phase4.trn.scrambler.history &&
		       actual->phase4.trn.has_last_symbol ==
		       model->phase4.trn.has_last_symbol &&
		       actual->phase4.trn.last_clockwise_quarter_turns ==
		       model->phase4.trn.last_clockwise_quarter_turns;
	}
	return false;
}

static bool run_composed_sequence(
	enum v34_constellation_size constellation)
{
	struct v34_phase3_profile profile = profile_fixture();
	struct v34_caller_training actual;
	struct model_training model;
	uint32_t samples = 0;
	unsigned int stages_seen = 0;

	CHECK(v34_caller_training_init(
		      &actual, &profile, constellation, 8000, 5000) ==
		      V34_CALLER_TRAINING_OK &&
	      model_init(&model, &profile, constellation),
	      "selection %u did not initialize", constellation);
	while (model.phase4_trn_symbols < 600) {
		struct qam_point point;
		const struct qam_point *point_pointer = NULL;
		int16_t expected;
		int16_t sample;
		bool accepted;

		CHECK(samples < 6000,
		      "selection %u exceeded its line-sample bound",
		      constellation);
		if (model.stage == V34_CALLER_TRAINING_PHASE3) {
			stages_seen |= 1u << model.phase3.stage;
			if (model.phase3.stage == V34_PHASE3_CALLER_TRN &&
			    model.phase3.trn_symbols ==
				    V34_PHASE3_TRN_MIN_SYMBOLS) {
				CHECK(v34_caller_training_start_j(&actual) ==
					      V34_CALLER_TRAINING_OK &&
				      v34_phase3_caller_start_j(&model.phase3) ==
					      V34_PHASE3_CALLER_OK,
				      "selection %u did not enter J", constellation);
			}
			if (model.phase3.stage == V34_PHASE3_CALLER_J &&
			    model.phase3.j_symbols ==
				    V34_PHASE3_J_PATTERN_SYMBOLS) {
				CHECK(v34_caller_training_finish_j(&actual) ==
					      V34_CALLER_TRAINING_OK &&
				      model_finish_j(&model),
				      "selection %u did not enter J-prime",
				      constellation);
				stages_seen |= 1u << V34_PHASE3_CALLER_COMPLETE;
			}
		}
		stages_seen |= 1u << (8u + model.stage);
		if (v34_training_line_needs_point(&model.line)) {
			point_pointer = &point;
			CHECK(model_next_point(&model, &point),
			      "selection %u model stopped at sample %u",
			      constellation, samples);
		}
		CHECK(v34_training_line_sample(
			      &model.line, point_pointer, &expected, &accepted) ==
			      V34_TRAINING_LINE_OK &&
		      accepted == (point_pointer != NULL) &&
		      v34_caller_training_sample(&actual, &sample) ==
			      V34_CALLER_TRAINING_OK &&
		      sample == expected && protocol_state_matches(&actual, &model) &&
		      actual.line.generated_samples ==
			      model.line.generated_samples &&
		      actual.line.carrier.carrier_phase ==
			      model.line.carrier.carrier_phase &&
		      actual.line.carrier.symbol_phase_units ==
			      model.line.carrier.symbol_phase_units,
		      "selection %u diverged at line sample %u",
		      constellation, samples);
		samples++;
	}
	CHECK(stages_seen ==
		      (((1u << (V34_PHASE3_CALLER_COMPLETE + 1u)) - 1u) |
		       (1u << (8u + V34_CALLER_TRAINING_PHASE3)) |
		       (1u << (8u + V34_CALLER_TRAINING_J_PRIME)) |
		       (1u << (8u + V34_CALLER_TRAINING_PHASE4_TRN))) &&
	      actual.line.generated_samples == samples &&
	      v34_caller_training_phase4_trn_ready(&actual),
	      "selection %u missed a stage or Phase 4 minimum", constellation);
	return true;
}

static bool test_composed_line(void)
{
	return run_composed_sequence(V34_CONSTELLATION_4_POINT) &&
	       run_composed_sequence(V34_CONSTELLATION_16_POINT);
}

static bool advance_to_phase4(struct v34_caller_training *training)
{
	for (uint32_t sample_count = 0; sample_count < 5000; ++sample_count) {
		int16_t ignored;

		if (training->stage == V34_CALLER_TRAINING_PHASE3 &&
		    training->phase3.stage == V34_PHASE3_CALLER_TRN &&
		    training->phase3.trn_symbols == V34_PHASE3_TRN_MIN_SYMBOLS &&
		    v34_caller_training_start_j(training) !=
			    V34_CALLER_TRAINING_OK)
			return false;
		if (training->stage == V34_CALLER_TRAINING_PHASE3 &&
		    training->phase3.stage == V34_PHASE3_CALLER_J &&
		    training->phase3.j_symbols == V34_PHASE3_J_PATTERN_SYMBOLS &&
		    v34_caller_training_finish_j(training) !=
			    V34_CALLER_TRAINING_OK)
			return false;
		if (training->stage == V34_CALLER_TRAINING_PHASE4_TRN)
			return true;
		if (v34_caller_training_sample(training, &ignored) !=
		    V34_CALLER_TRAINING_OK)
			return false;
	}
	return false;
}

static bool test_copy_controls_and_overflow(void)
{
	struct v34_phase3_profile profile = profile_fixture();
	struct v34_caller_training first;
	struct v34_caller_training second;
	struct v34_caller_training before;
	int16_t sample = 1234;

	CHECK(v34_caller_training_init(
		      &first, &profile, V34_CONSTELLATION_16_POINT,
		      8000, 5000) == V34_CALLER_TRAINING_OK,
	      "control fixture did not initialize");
	before = first;
	CHECK(v34_caller_training_start_j(&first) ==
		      V34_CALLER_TRAINING_INVALID_STATE &&
	      v34_caller_training_finish_j(&first) ==
		      V34_CALLER_TRAINING_INVALID_STATE &&
	      v34_caller_training_sample(&first, NULL) ==
		      V34_CALLER_TRAINING_INVALID_ARGUMENT &&
	      memcmp(&first, &before, sizeof(first)) == 0,
	      "early controls or null output mutated state");
	CHECK(advance_to_phase4(&first),
	      "control fixture did not reach Phase 4");
	second = first;
	for (unsigned int index = 0; index < 257; ++index) {
		int16_t first_sample;
		int16_t second_sample;

		CHECK(v34_caller_training_sample(&first, &first_sample) ==
			      V34_CALLER_TRAINING_OK &&
		      v34_caller_training_sample(&second, &second_sample) ==
			      V34_CALLER_TRAINING_OK &&
		      first_sample == second_sample &&
		      memcmp(&first, &second, sizeof(first)) == 0,
		      "copied caller training diverged at sample %u", index);
	}
	before = first;
	CHECK(v34_caller_training_start_j(&first) ==
		      V34_CALLER_TRAINING_INVALID_STATE &&
	      v34_caller_training_finish_j(&first) ==
		      V34_CALLER_TRAINING_INVALID_STATE &&
	      memcmp(&first, &before, sizeof(first)) == 0,
	      "completed Phase 3 controls changed Phase 4 state");
	while (!v34_training_line_needs_point(&first.line))
		CHECK(v34_caller_training_sample(&first, &sample) ==
			      V34_CALLER_TRAINING_OK,
		      "overflow fixture did not reach a point boundary");
	first.phase4_trn_symbols = UINT32_MAX;
	CHECK(v34_caller_training_is_valid(&first),
	      "overflow fixture was not valid");
	before = first;
	sample = 1234;
	CHECK(v34_caller_training_sample(&first, &sample) ==
		      V34_CALLER_TRAINING_OUT_OF_RANGE &&
	      memcmp(&first, &before, sizeof(first)) == 0 && sample == 1234,
	      "Phase 4 symbol overflow mutated state or output");
	return true;
}

static bool test_initialization(void)
{
	struct v34_phase3_profile profile = profile_fixture();
	struct v34_phase3_profile answering = profile;
	struct v34_caller_training training;
	struct v34_caller_training before;

	answering.local_role = V34_ANSWERING_STATION;
	memset(&training, 0xa5, sizeof(training));
	before = training;
	CHECK(v34_caller_training_init(
		      NULL, &profile, V34_CONSTELLATION_4_POINT, 8000, 5000) ==
		      V34_CALLER_TRAINING_INVALID_ARGUMENT &&
	      v34_caller_training_init(
		      &training, NULL, V34_CONSTELLATION_4_POINT, 8000, 5000) ==
		      V34_CALLER_TRAINING_INVALID_ARGUMENT &&
	      v34_caller_training_init(
		      &training, &answering, V34_CONSTELLATION_4_POINT,
		      8000, 5000) == V34_CALLER_TRAINING_INVALID_ARGUMENT &&
	      v34_caller_training_init(
		      &training, &profile, (enum v34_constellation_size)8,
		      8000, 5000) == V34_CALLER_TRAINING_INVALID_ARGUMENT &&
	      v34_caller_training_init(
		      &training, &profile, V34_CONSTELLATION_4_POINT,
		      7000, 5000) == V34_CALLER_TRAINING_OUT_OF_RANGE &&
	      memcmp(&training, &before, sizeof(training)) == 0,
	      "invalid caller training initialization mutated its destination");
	CHECK(v34_caller_training_init(
		      &training, &profile, V34_CONSTELLATION_4_POINT,
		      8000, INT16_MAX) == V34_CALLER_TRAINING_CLIPPING_RISK &&
	      memcmp(&training, &before, sizeof(training)) == 0,
	      "clipping caller training mutated its destination");
	CHECK(!v34_caller_training_is_valid(NULL) &&
	      !v34_caller_training_phase4_trn_ready(NULL) &&
	      strcmp(v34_caller_training_result_name(
		     V34_CALLER_TRAINING_CLIPPING_RISK),
		     "clipping risk") == 0 &&
	      strcmp(v34_caller_training_result_name(
		     (enum v34_caller_training_result)99),
		     "unknown") == 0,
	      "caller training validation or result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "composed line", test_composed_line },
		{ "copy, controls, and overflow", test_copy_controls_and_overflow },
		{ "initialization", test_initialization },
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
