// SPDX-License-Identifier: BSD-3-Clause

#include <v34_training_transmitter.h>

#include "test.h"

#include <string.h>

struct model_training {
	struct v34_training_line line;
	struct v34_phase3_transmitter phase3;
	union v34_training_transmitter_phase4_state phase4;
	uint32_t phase4_trn_symbols;
	enum v34_training_transmitter_stage stage;
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

static struct v34_mp_parameters mp_parameters(bool acknowledges_peer)
{
	return (struct v34_mp_parameters){
		.maximum_call_to_answer_rate_2400_bps_units = 1,
		.maximum_answer_to_call_rate_2400_bps_units = 1,
		.remote_transmitter_trellis = V34_TRELLIS_16_STATE,
		.acknowledges_peer_mp = acknowledges_peer,
		.data_rate_capability_mask = 1,
	};
}

static struct v34_mp_type1_parameters
mp_type1_parameters(bool acknowledges_peer)
{
	return (struct v34_mp_type1_parameters){
		.common = mp_parameters(acknowledges_peer),
		.precoder_coefficients = {
			{ .real = 4096, .imaginary = -2048 },
			{ .real = 1024, .imaginary = 512 },
			{ .real = -256, .imaginary = 128 },
		},
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
	    v34_phase3_transmitter_init(
		    &initialized.phase3, V34_CALLING_STATION,
		    constellation) !=
		    V34_PHASE3_TRANSMITTER_OK)
		return false;
	initialized.stage = V34_TRAINING_TRANSMITTER_PHASE3;
	*model = initialized;
	return true;
}

static bool model_finish_j(struct model_training *model)
{
	if (v34_phase3_transmitter_finish_j(&model->phase3) !=
		    V34_PHASE3_TRANSMITTER_OK ||
	    v34_phase3_transmitter_start_j_prime(
		    &model->phase3, &model->phase4.j_prime) !=
		    V34_PHASE3_TRANSMITTER_OK)
		return false;
	model->stage = V34_TRAINING_TRANSMITTER_J_PRIME;
	return true;
}

static bool model_start_mp_type0(
	struct model_training *model,
	const struct v34_mp_parameters *parameters)
{
	if (model->stage == V34_TRAINING_TRANSMITTER_PHASE4_TRN) {
		struct v34_phase4_mp mp;

		if (v34_phase4_mp_init_type0_from_trn(
			    &mp, &model->phase4.trn, parameters) !=
		    V34_PHASE4_MP_OK)
			return false;
		model->phase4 = (union v34_training_transmitter_phase4_state){ 0 };
		model->phase4.mp = mp;
		model->stage = V34_TRAINING_TRANSMITTER_PHASE4_MP;
		return true;
	}
	return model->stage == V34_TRAINING_TRANSMITTER_PHASE4_MP &&
	       v34_phase4_mp_start_next_type0_record(
		       &model->phase4.mp, parameters) == V34_PHASE4_MP_OK;
}

static bool model_start_mp_type1(
	struct model_training *model,
	const struct v34_mp_type1_parameters *parameters)
{
	if (model->stage == V34_TRAINING_TRANSMITTER_PHASE4_TRN) {
		struct v34_phase4_mp mp;

		if (v34_phase4_mp_init_type1_from_trn(
			    &mp, &model->phase4.trn, parameters) !=
		    V34_PHASE4_MP_OK)
			return false;
		model->phase4 = (union v34_training_transmitter_phase4_state){ 0 };
		model->phase4.mp = mp;
		model->stage = V34_TRAINING_TRANSMITTER_PHASE4_MP;
		return true;
	}
	return model->stage == V34_TRAINING_TRANSMITTER_PHASE4_MP &&
	       v34_phase4_mp_start_next_type1_record(
		       &model->phase4.mp, parameters) == V34_PHASE4_MP_OK;
}

static bool model_start_e(struct model_training *model)
{
	struct v34_phase4_e_sequence e;

	if (model->stage != V34_TRAINING_TRANSMITTER_PHASE4_MP ||
	    v34_phase4_e_sequence_init_from_mp(
		    &e, &model->phase4.mp) != V34_PHASE4_E_SEQUENCE_OK)
		return false;
	model->phase4 = (union v34_training_transmitter_phase4_state){ 0 };
	model->phase4.e = e;
	model->stage = V34_TRAINING_TRANSMITTER_PHASE4_E;
	return true;
}

static bool model_next_point(
	struct model_training *model, struct qam_point *point)
{
	switch (model->stage) {
	case V34_TRAINING_TRANSMITTER_PHASE3:
		return v34_phase3_transmitter_next(&model->phase3, point) ==
		       V34_PHASE3_TRANSMITTER_OK;
	case V34_TRAINING_TRANSMITTER_J_PRIME: {
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
		model->phase4 = (union v34_training_transmitter_phase4_state){ 0 };
		model->phase4.trn = trn;
		model->stage = V34_TRAINING_TRANSMITTER_PHASE4_TRN;
		return true;
	}
	case V34_TRAINING_TRANSMITTER_PHASE4_TRN: {
		uint8_t quarter_point;
		uint8_t rotation;

		if (v34_phase4_trn_next(
			    &model->phase4.trn, point, &quarter_point,
			    &rotation) != V34_PHASE4_TRN_OK)
			return false;
		model->phase4_trn_symbols++;
		return true;
	}
	case V34_TRAINING_TRANSMITTER_PHASE4_MP: {
		uint8_t quarter_point;
		uint8_t rotation;
		bool complete;

		return v34_phase4_mp_next(
			       &model->phase4.mp, point, &quarter_point,
			       &rotation, &complete) == V34_PHASE4_MP_OK;
	}
	case V34_TRAINING_TRANSMITTER_PHASE4_E: {
		uint8_t quarter_point;
		uint8_t rotation;

		return v34_phase4_e_sequence_next(
			       &model->phase4.e, point, &quarter_point,
			       &rotation) == V34_PHASE4_E_SEQUENCE_OK;
	}
	}
	return false;
}

static bool protocol_state_matches(
	const struct v34_training_transmitter *actual,
	const struct model_training *model)
{
	if (actual->stage != model->stage ||
	    actual->phase4_trn_symbols != model->phase4_trn_symbols ||
	    actual->phase3.stage != model->phase3.stage ||
	    actual->phase3.trn_symbols != model->phase3.trn_symbols ||
	    actual->phase3.j_symbols != model->phase3.j_symbols)
		return false;
	switch (model->stage) {
	case V34_TRAINING_TRANSMITTER_PHASE3:
		return true;
	case V34_TRAINING_TRANSMITTER_J_PRIME:
		return actual->phase4.j_prime.emitted_symbols ==
		       model->phase4.j_prime.emitted_symbols &&
		       actual->phase4.j_prime.scrambler.history ==
		       model->phase4.j_prime.scrambler.history;
	case V34_TRAINING_TRANSMITTER_PHASE4_TRN:
		return actual->phase4.trn.scrambler.history ==
		       model->phase4.trn.scrambler.history &&
		       actual->phase4.trn.has_last_symbol ==
		       model->phase4.trn.has_last_symbol &&
		       actual->phase4.trn.last_clockwise_quarter_turns ==
		       model->phase4.trn.last_clockwise_quarter_turns;
	case V34_TRAINING_TRANSMITTER_PHASE4_MP:
		return memcmp(
			       actual->phase4.mp.frame,
			       model->phase4.mp.frame,
			       sizeof(actual->phase4.mp.frame)) == 0 &&
		       actual->phase4.mp.scrambler.history ==
			       model->phase4.mp.scrambler.history &&
		       actual->phase4.mp.scrambler.polynomial ==
			       model->phase4.mp.scrambler.polynomial &&
		       actual->phase4.mp.frame_bit_count ==
			       model->phase4.mp.frame_bit_count &&
		       actual->phase4.mp.bit_index ==
			       model->phase4.mp.bit_index &&
		       actual->phase4.mp.previous_clockwise_quarter_turns ==
			       model->phase4.mp.previous_clockwise_quarter_turns;
	case V34_TRAINING_TRANSMITTER_PHASE4_E:
		return actual->phase4.e.scrambler.history ==
			       model->phase4.e.scrambler.history &&
		       actual->phase4.e.scrambler.polynomial ==
			       model->phase4.e.scrambler.polynomial &&
		       actual->phase4.e.emitted_bits ==
			       model->phase4.e.emitted_bits &&
		       actual->phase4.e.previous_clockwise_quarter_turns ==
			       model->phase4.e.previous_clockwise_quarter_turns;
	}
	return false;
}

static bool run_composed_sequence(
	enum v34_constellation_size constellation)
{
	struct v34_phase3_profile profile = profile_fixture();
	struct v34_mp_parameters mp = mp_parameters(false);
	struct v34_mp_type1_parameters mp_prime =
		mp_type1_parameters(true);
	struct v34_training_transmitter actual;
	struct model_training model;
	uint32_t samples = 0;
	unsigned int stages_seen = 0;
	bool mp_prime_started = false;

	CHECK(v34_training_transmitter_init(
		      &actual, &profile, constellation, 8000, 5000) ==
		      V34_TRAINING_TRANSMITTER_OK &&
	      model_init(&model, &profile, constellation),
	      "selection %u did not initialize", constellation);
	while (model.stage != V34_TRAINING_TRANSMITTER_PHASE4_E ||
	       !v34_phase4_e_sequence_is_complete(&model.phase4.e)) {
		struct qam_point point;
		const struct qam_point *point_pointer = NULL;
		int16_t expected;
		int16_t sample;
		bool accepted;

		CHECK(samples < 7000,
		      "selection %u exceeded its line-sample bound",
		      constellation);
		if (model.stage == V34_TRAINING_TRANSMITTER_PHASE3) {
			stages_seen |= 1u << model.phase3.stage;
			if (model.phase3.stage == V34_PHASE3_TRANSMITTER_TRN &&
			    model.phase3.trn_symbols ==
				    V34_PHASE3_TRN_MIN_SYMBOLS) {
				CHECK(v34_training_transmitter_start_j(&actual) ==
					      V34_TRAINING_TRANSMITTER_OK &&
				      v34_phase3_transmitter_start_j(&model.phase3) ==
					      V34_PHASE3_TRANSMITTER_OK,
				      "selection %u did not enter J", constellation);
			}
			if (model.phase3.stage == V34_PHASE3_TRANSMITTER_J &&
			    model.phase3.j_symbols ==
				    V34_PHASE3_J_PATTERN_SYMBOLS) {
				CHECK(v34_training_transmitter_finish_j(&actual) ==
					      V34_TRAINING_TRANSMITTER_OK &&
				      model_finish_j(&model),
				      "selection %u did not enter J-prime",
				      constellation);
				stages_seen |= 1u << V34_PHASE3_TRANSMITTER_COMPLETE;
			}
		}
		if (model.stage == V34_TRAINING_TRANSMITTER_PHASE4_TRN &&
		    model.phase4_trn_symbols == 600) {
			CHECK(v34_training_transmitter_start_mp_type0(
				      &actual, &mp) == V34_TRAINING_TRANSMITTER_OK &&
			      model_start_mp_type0(&model, &mp),
			      "selection %u did not enter MP", constellation);
		}
		if (model.stage == V34_TRAINING_TRANSMITTER_PHASE4_MP &&
		    v34_phase4_mp_record_is_complete(&model.phase4.mp)) {
			if (!mp_prime_started) {
				CHECK(v34_training_transmitter_start_mp_type1(
					      &actual, &mp_prime) ==
					      V34_TRAINING_TRANSMITTER_OK &&
				      model_start_mp_type1(&model, &mp_prime),
				      "selection %u did not enter MP-prime",
				      constellation);
				mp_prime_started = true;
			} else {
				CHECK(v34_training_transmitter_start_e(&actual) ==
					      V34_TRAINING_TRANSMITTER_OK &&
				      model_start_e(&model),
				      "selection %u did not enter E", constellation);
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
		      v34_training_transmitter_sample(&actual, &sample) ==
			      V34_TRAINING_TRANSMITTER_OK &&
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
		      (((1u << (V34_PHASE3_TRANSMITTER_COMPLETE + 1u)) - 1u) |
		       (1u << (8u + V34_TRAINING_TRANSMITTER_PHASE3)) |
		       (1u << (8u + V34_TRAINING_TRANSMITTER_J_PRIME)) |
		       (1u << (8u + V34_TRAINING_TRANSMITTER_PHASE4_TRN)) |
		       (1u << (8u + V34_TRAINING_TRANSMITTER_PHASE4_MP)) |
		       (1u << (8u + V34_TRAINING_TRANSMITTER_PHASE4_E))) &&
	      actual.line.generated_samples == samples &&
	      mp_prime_started &&
	      v34_training_transmitter_phase4_e_complete(&actual) &&
	      !v34_training_transmitter_phase4_trn_ready(&actual) &&
	      !v34_training_transmitter_phase4_mp_record_complete(&actual),
	      "selection %u missed a Phase 4 transmit stage", constellation);
	return true;
}

static bool test_composed_line(void)
{
	return run_composed_sequence(V34_CONSTELLATION_4_POINT) &&
	       run_composed_sequence(V34_CONSTELLATION_16_POINT);
}

static bool advance_to_phase4(struct v34_training_transmitter *training)
{
	for (uint32_t sample_count = 0; sample_count < 5000; ++sample_count) {
		int16_t ignored;

		if (training->stage == V34_TRAINING_TRANSMITTER_PHASE3 &&
		    training->phase3.stage == V34_PHASE3_TRANSMITTER_TRN &&
		    training->phase3.trn_symbols == V34_PHASE3_TRN_MIN_SYMBOLS &&
		    v34_training_transmitter_start_j(training) !=
			    V34_TRAINING_TRANSMITTER_OK)
			return false;
		if (training->stage == V34_TRAINING_TRANSMITTER_PHASE3 &&
		    training->phase3.stage == V34_PHASE3_TRANSMITTER_J &&
		    training->phase3.j_symbols == V34_PHASE3_J_PATTERN_SYMBOLS &&
		    v34_training_transmitter_finish_j(training) !=
			    V34_TRAINING_TRANSMITTER_OK)
			return false;
		if (training->stage == V34_TRAINING_TRANSMITTER_PHASE4_TRN)
			return true;
		if (v34_training_transmitter_sample(training, &ignored) !=
		    V34_TRAINING_TRANSMITTER_OK)
			return false;
	}
	return false;
}

static bool advance_phase4_trn_to(
	struct v34_training_transmitter *training, uint32_t target_symbols)
{
	for (uint32_t sample_count = 0; sample_count < 2000; ++sample_count) {
		int16_t ignored;

		if (training->stage != V34_TRAINING_TRANSMITTER_PHASE4_TRN)
			return false;
		if (training->phase4_trn_symbols >= target_symbols)
			return true;
		if (v34_training_transmitter_sample(training, &ignored) !=
		    V34_TRAINING_TRANSMITTER_OK)
			return false;
	}
	return false;
}

static bool complete_mp_record(struct v34_training_transmitter *training)
{
	for (uint32_t sample_count = 0; sample_count < 1000; ++sample_count) {
		int16_t ignored;

		if (v34_training_transmitter_phase4_mp_record_complete(training))
			return true;
		if (v34_training_transmitter_sample(training, &ignored) !=
		    V34_TRAINING_TRANSMITTER_OK)
			return false;
	}
	return false;
}

static bool complete_e(struct v34_training_transmitter *training)
{
	for (uint32_t sample_count = 0; sample_count < 100; ++sample_count) {
		int16_t ignored;

		if (v34_training_transmitter_phase4_e_complete(training))
			return true;
		if (v34_training_transmitter_sample(training, &ignored) !=
		    V34_TRAINING_TRANSMITTER_OK)
			return false;
	}
	return false;
}

static bool drain_to_point_boundary(struct v34_training_transmitter *training)
{
	for (unsigned int sample_count = 0; sample_count < 8; ++sample_count) {
		int16_t ignored;

		if (v34_training_line_needs_point(&training->line))
			return true;
		if (v34_training_transmitter_sample(training, &ignored) !=
		    V34_TRAINING_TRANSMITTER_OK)
			return false;
	}
	return false;
}

static bool test_copy_controls_and_overflow(void)
{
	struct v34_phase3_profile profile = profile_fixture();
	struct v34_training_transmitter first;
	struct v34_training_transmitter second;
	struct v34_training_transmitter before;
	int16_t sample = 1234;

	CHECK(v34_training_transmitter_init(
		      &first, &profile, V34_CONSTELLATION_16_POINT,
		      8000, 5000) == V34_TRAINING_TRANSMITTER_OK,
	      "control fixture did not initialize");
	before = first;
	CHECK(v34_training_transmitter_start_j(&first) ==
		      V34_TRAINING_TRANSMITTER_INVALID_STATE &&
	      v34_training_transmitter_finish_j(&first) ==
		      V34_TRAINING_TRANSMITTER_INVALID_STATE &&
	      v34_training_transmitter_sample(&first, NULL) ==
		      V34_TRAINING_TRANSMITTER_INVALID_ARGUMENT &&
	      memcmp(&first, &before, sizeof(first)) == 0,
	      "early controls or null output mutated state");
	CHECK(advance_to_phase4(&first),
	      "control fixture did not reach Phase 4");
	second = first;
	for (unsigned int index = 0; index < 257; ++index) {
		int16_t first_sample;
		int16_t second_sample;

		CHECK(v34_training_transmitter_sample(&first, &first_sample) ==
			      V34_TRAINING_TRANSMITTER_OK &&
		      v34_training_transmitter_sample(&second, &second_sample) ==
			      V34_TRAINING_TRANSMITTER_OK &&
		      first_sample == second_sample &&
		      memcmp(&first, &second, sizeof(first)) == 0,
		      "copied caller training diverged at sample %u", index);
	}
	before = first;
	CHECK(v34_training_transmitter_start_j(&first) ==
		      V34_TRAINING_TRANSMITTER_INVALID_STATE &&
	      v34_training_transmitter_finish_j(&first) ==
		      V34_TRAINING_TRANSMITTER_INVALID_STATE &&
	      memcmp(&first, &before, sizeof(first)) == 0,
	      "completed Phase 3 controls changed Phase 4 state");
	while (!v34_training_line_needs_point(&first.line))
		CHECK(v34_training_transmitter_sample(&first, &sample) ==
			      V34_TRAINING_TRANSMITTER_OK,
		      "overflow fixture did not reach a point boundary");
	first.phase4_trn_symbols = UINT32_MAX;
	CHECK(v34_training_transmitter_is_valid(&first),
	      "overflow fixture was not valid");
	before = first;
	sample = 1234;
	CHECK(v34_training_transmitter_sample(&first, &sample) ==
		      V34_TRAINING_TRANSMITTER_OUT_OF_RANGE &&
	      memcmp(&first, &before, sizeof(first)) == 0 && sample == 1234,
	      "Phase 4 symbol overflow mutated state or output");
	return true;
}

static bool test_phase4_controls(void)
{
	struct v34_phase3_profile profile = profile_fixture();
	struct v34_mp_parameters mp_prime = mp_parameters(true);
	struct v34_mp_type1_parameters mp = mp_type1_parameters(false);
	struct v34_mp_type1_parameters invalid = mp;
	struct v34_training_transmitter training;
	struct v34_training_transmitter before;
	int16_t sample = 1234;

	CHECK(v34_training_transmitter_init(
		      &training, &profile, V34_CONSTELLATION_16_POINT,
		      8000, 5000) == V34_TRAINING_TRANSMITTER_OK &&
	      advance_to_phase4(&training),
	      "Phase 4 control fixture did not initialize");
	before = training;
	CHECK(v34_training_transmitter_start_mp_type0(
		      &training, &mp_prime) == V34_TRAINING_TRANSMITTER_INVALID_STATE &&
	      v34_training_transmitter_start_mp_type0(&training, NULL) ==
		      V34_TRAINING_TRANSMITTER_INVALID_ARGUMENT &&
	      v34_training_transmitter_start_mp_type1(&training, NULL) ==
		      V34_TRAINING_TRANSMITTER_INVALID_ARGUMENT &&
	      v34_training_transmitter_start_e(&training) ==
		      V34_TRAINING_TRANSMITTER_INVALID_STATE &&
	      !v34_training_transmitter_phase4_mp_record_complete(&training) &&
	      !v34_training_transmitter_phase4_e_complete(&training) &&
	      memcmp(&training, &before, sizeof(training)) == 0,
	      "early Phase 4 control changed state");
	CHECK(advance_phase4_trn_to(
		      &training, V34_PHASE4_TRN_MIN_SYMBOLS) &&
	      v34_training_transmitter_phase4_trn_ready(&training),
	      "Phase 4 control fixture did not reach the TRN minimum");
	invalid.common.maximum_call_to_answer_rate_2400_bps_units = 0;
	before = training;
	CHECK(v34_training_transmitter_start_mp_type1(&training, &invalid) ==
		      V34_TRAINING_TRANSMITTER_INVALID_PARAMETERS &&
	      memcmp(&training, &before, sizeof(training)) == 0 &&
	      v34_training_transmitter_start_mp_type1(&training, &mp) ==
		      V34_TRAINING_TRANSMITTER_OK &&
	      training.stage == V34_TRAINING_TRANSMITTER_PHASE4_MP,
	      "first MP record did not validate transactionally");
	before = training;
	CHECK(v34_training_transmitter_start_mp_type0(
		      &training, &mp_prime) == V34_TRAINING_TRANSMITTER_INVALID_STATE &&
	      v34_training_transmitter_start_e(&training) ==
		      V34_TRAINING_TRANSMITTER_INVALID_STATE &&
	      memcmp(&training, &before, sizeof(training)) == 0 &&
	      complete_mp_record(&training) &&
	      drain_to_point_boundary(&training),
	      "active MP record accepted a transition");
	before = training;
	sample = 1234;
	CHECK(v34_training_transmitter_sample(&training, &sample) ==
		      V34_TRAINING_TRANSMITTER_INVALID_STATE &&
	      sample == 1234 && memcmp(&training, &before, sizeof(training)) == 0 &&
	      v34_training_transmitter_start_mp_type0(
		      &training, &mp_prime) == V34_TRAINING_TRANSMITTER_OK &&
	      complete_mp_record(&training) &&
	      v34_training_transmitter_start_e(&training) ==
		      V34_TRAINING_TRANSMITTER_OK &&
	      complete_e(&training) &&
	      drain_to_point_boundary(&training),
	      "MP-prime or E transition failed");
	before = training;
	sample = 1234;
	CHECK(v34_training_transmitter_sample(&training, &sample) ==
		      V34_TRAINING_TRANSMITTER_INVALID_STATE &&
	      sample == 1234 && memcmp(&training, &before, sizeof(training)) == 0 &&
	      v34_training_transmitter_start_mp_type0(
		      &training, &mp_prime) == V34_TRAINING_TRANSMITTER_INVALID_STATE &&
	      v34_training_transmitter_start_e(&training) ==
		      V34_TRAINING_TRANSMITTER_INVALID_STATE &&
	      memcmp(&training, &before, sizeof(training)) == 0,
	      "completed E emitted or accepted an extra transition");
	return true;
}

static bool test_initialization(void)
{
	struct v34_phase3_profile profile = profile_fixture();
	struct v34_phase3_profile answering = profile;
	struct v34_phase3_profile with_md = profile;
	struct v34_training_transmitter training;
	struct v34_training_transmitter before;

	answering.local_role = V34_ANSWERING_STATION;
	with_md.md_length_35_ms_units = 1;
	with_md.md_symbol_count = 105;
	memset(&training, 0xa5, sizeof(training));
	before = training;
	CHECK(v34_training_transmitter_init(
		      NULL, &profile, V34_CONSTELLATION_4_POINT, 8000, 5000) ==
		      V34_TRAINING_TRANSMITTER_INVALID_ARGUMENT &&
	      v34_training_transmitter_init(
		      &training, NULL, V34_CONSTELLATION_4_POINT, 8000, 5000) ==
		      V34_TRAINING_TRANSMITTER_INVALID_ARGUMENT &&
	      v34_training_transmitter_init(
		      &training, &answering, V34_CONSTELLATION_4_POINT,
		      8000, 5000) == V34_TRAINING_TRANSMITTER_INVALID_ARGUMENT &&
	      v34_phase3_profile_is_valid(&with_md) &&
	      v34_training_transmitter_init(
		      &training, &with_md, V34_CONSTELLATION_4_POINT,
		      8000, 5000) == V34_TRAINING_TRANSMITTER_INVALID_ARGUMENT &&
	      v34_training_transmitter_init(
		      &training, &profile, (enum v34_constellation_size)8,
		      8000, 5000) == V34_TRAINING_TRANSMITTER_INVALID_ARGUMENT &&
	      v34_training_transmitter_init(
		      &training, &profile, V34_CONSTELLATION_4_POINT,
		      7000, 5000) == V34_TRAINING_TRANSMITTER_OUT_OF_RANGE &&
	      memcmp(&training, &before, sizeof(training)) == 0,
	      "invalid caller training initialization mutated its destination");
	CHECK(v34_training_transmitter_init(
		      &training, &profile, V34_CONSTELLATION_4_POINT,
		      8000, INT16_MAX) == V34_TRAINING_TRANSMITTER_CLIPPING_RISK &&
	      memcmp(&training, &before, sizeof(training)) == 0,
	      "clipping caller training mutated its destination");
	CHECK(!v34_training_transmitter_is_valid(NULL) &&
	      !v34_training_transmitter_phase4_trn_ready(NULL) &&
	      !v34_training_transmitter_phase4_mp_record_complete(NULL) &&
	      !v34_training_transmitter_phase4_e_complete(NULL) &&
	      strcmp(v34_training_transmitter_result_name(
		     V34_TRAINING_TRANSMITTER_CLIPPING_RISK),
		     "clipping risk") == 0 &&
	      strcmp(v34_training_transmitter_result_name(
		     V34_TRAINING_TRANSMITTER_INVALID_PARAMETERS),
		     "invalid parameters") == 0 &&
	      strcmp(v34_training_transmitter_result_name(
		     (enum v34_training_transmitter_result)99),
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
		{ "Phase 4 controls", test_phase4_controls },
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
