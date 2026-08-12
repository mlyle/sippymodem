// SPDX-License-Identifier: BSD-3-Clause

#include <v34_phase4_line_receiver.h>

#include <v34_constellation.h>
#include <v34_phase4_e_sequence.h>
#include <v34_phase4_mp.h>
#include <v34_phase4_trn.h>

#include "test.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
	SAMPLE_RATE = 8000,
	FRONTEND_SAMPLE_LIMIT = 40000,
	FRONTEND_SETTLE_SYMBOLS = 64,
	EQUALIZER_TRAINING_SYMBOLS = 1536,
	CONTROL_TRN_SYMBOLS = V34_PHASE4_TRN_MIN_SYMBOLS + 384,
	SCRIPT_SAMPLE_LIMIT = 60000,
};

static const double LINE_GAIN = 6000.0;

static const struct qam_point B1_POINTS[] = {
	{ 0.20, -0.35 },
	{ -0.45, 0.15 },
	{ 0.30, 0.40 },
	{ -0.25, -0.30 },
	{ 0.40, -0.10 },
};

static struct v34_phase3_profile profile_for(
	enum v34_station_role role)
{
	return (struct v34_phase3_profile){
		.symbol_rate_numerator = 3000,
		.symbol_rate_denominator = 1,
		.carrier_frequency_numerator = 2000,
		.carrier_frequency_denominator = 1,
		.symbol_rate = V34_SYMBOL_RATE_3000,
		.local_role = role,
		.preemphasis_filter_index = 0,
		.projected_data_rate_2400_bps_units = 8,
		.high_carrier = true,
	};
}

static struct v34_mp_parameters parameters(bool acknowledges_peer)
{
	return (struct v34_mp_parameters){
		.maximum_call_to_answer_rate_2400_bps_units = 10,
		.maximum_answer_to_call_rate_2400_bps_units = 9,
		.auxiliary_channel_enabled = true,
		.remote_transmitter_trellis = V34_TRELLIS_32_STATE,
		.remote_transmitter_nonlinear_encoding = true,
		.remote_transmitter_expanded_shaping = true,
		.acknowledges_peer_mp = acknowledges_peer,
		.data_rate_capability_mask = 0x1a55,
		.asymmetric_rates_enabled = true,
	};
}

static bool parameters_are_equal(
	const struct v34_mp_parameters *left,
	const struct v34_mp_parameters *right)
{
	uint8_t left_frame[V34_MP_TYPE0_FRAME_BYTES];
	uint8_t right_frame[V34_MP_TYPE0_FRAME_BYTES];

	return v34_mp_type0_encode(
		       left, left_frame, sizeof(left_frame)) == V34_MP_OK &&
	       v34_mp_type0_encode(
		       right, right_frame, sizeof(right_frame)) == V34_MP_OK &&
	       memcmp(left_frame, right_frame, sizeof(left_frame)) == 0;
}

struct line_source {
	struct qam_modulator modulator;
	struct v34_phase4_trn trn;
	struct v34_phase4_mp mp;
	struct v34_phase4_e_sequence e;
	struct qam_point point;
	struct v34_mp_parameters initial_parameters;
	struct v34_mp_parameters final_parameters;
	unsigned int mp_record;
	unsigned int b1_point;
	bool record_complete;
	enum {
		SOURCE_CONSTANT,
		SOURCE_TRN,
		SOURCE_MP,
		SOURCE_E,
		SOURCE_B1,
	} stage;
};

static bool source_sample(
	struct line_source *source, int16_t *sample, bool *accepted)
{
	double generated;
	long rounded;

	if (qam_modulator_sample_f64(
		    &source->modulator, &source->point, &generated,
		    accepted) != QAM_OK ||
	    !isfinite(generated) || generated * LINE_GAIN <= INT16_MIN - 0.5 ||
	    generated * LINE_GAIN >= INT16_MAX + 0.5)
		return false;
	rounded = lround(generated * LINE_GAIN);
	if (rounded < INT16_MIN || rounded > INT16_MAX)
		return false;
	*sample = (int16_t)rounded;
	return true;
}

static bool source_start_trn(
	struct line_source *source, enum v34_station_role role,
	enum v34_constellation_size constellation)
{
	uint8_t quarter_point;
	uint8_t clockwise_quarter_turns;

	if (v34_phase4_trn_init(&source->trn, role, constellation) !=
		    V34_PHASE4_TRN_OK ||
	    v34_phase4_trn_next(
		    &source->trn, &source->point, &quarter_point,
		    &clockwise_quarter_turns) !=
		    V34_PHASE4_TRN_OK)
		return false;
	source->stage = SOURCE_TRN;
	return true;
}

static bool source_next_trn(struct line_source *source)
{
	uint8_t quarter_point;
	uint8_t clockwise_quarter_turns;

	return v34_phase4_trn_next(
		       &source->trn, &source->point, &quarter_point,
		       &clockwise_quarter_turns) ==
	       V34_PHASE4_TRN_OK;
}

static bool source_start_mp(struct line_source *source)
{
	uint8_t quarter_point;
	uint8_t clockwise_quarter_turns;

	if (v34_phase4_mp_init_type0_from_trn(
		    &source->mp, &source->trn,
		    &source->initial_parameters) != V34_PHASE4_MP_OK ||
	    v34_phase4_mp_next(
		    &source->mp, &source->point, &quarter_point,
		    &clockwise_quarter_turns,
		    &source->record_complete) != V34_PHASE4_MP_OK)
		return false;
	source->stage = SOURCE_MP;
	source->mp_record = 0;
	return true;
}

static bool source_next_mp(struct line_source *source)
{
	uint8_t quarter_point;
	uint8_t clockwise_quarter_turns;

	if (source->record_complete) {
		source->mp_record++;
		if (source->mp_record < 3u) {
			const struct v34_mp_parameters *next =
				source->mp_record == 1u ?
					&source->initial_parameters :
					&source->final_parameters;

			if (v34_phase4_mp_start_next_type0_record(
				    &source->mp, next) != V34_PHASE4_MP_OK)
				return false;
		} else {
			if (v34_phase4_e_sequence_init_from_mp(
				    &source->e, &source->mp) !=
				    V34_PHASE4_E_SEQUENCE_OK ||
			    v34_phase4_e_sequence_next(
				    &source->e, &source->point,
				    &quarter_point,
				    &clockwise_quarter_turns) !=
				    V34_PHASE4_E_SEQUENCE_OK)
				return false;
			source->stage = SOURCE_E;
			return true;
		}
	}
	return v34_phase4_mp_next(
		       &source->mp, &source->point, &quarter_point,
		       &clockwise_quarter_turns,
		       &source->record_complete) == V34_PHASE4_MP_OK;
}

static bool source_next_e(struct line_source *source)
{
	uint8_t quarter_point;
	uint8_t clockwise_quarter_turns;

	if (v34_phase4_e_sequence_is_complete(&source->e)) {
		source->stage = SOURCE_B1;
		source->b1_point = 0;
		source->point = B1_POINTS[0];
		return true;
	}
	return v34_phase4_e_sequence_next(
		       &source->e, &source->point, &quarter_point,
		       &clockwise_quarter_turns) ==
	       V34_PHASE4_E_SEQUENCE_OK;
}

static bool source_advance(struct line_source *source)
{
	switch (source->stage) {
	case SOURCE_CONSTANT:
		return true;
	case SOURCE_TRN:
		return source_next_trn(source);
	case SOURCE_MP:
		return source_next_mp(source);
	case SOURCE_E:
		return source_next_e(source);
	case SOURCE_B1:
		source->b1_point =
			(source->b1_point + 1u) % ARRAY_LENGTH(B1_POINTS);
		source->point = B1_POINTS[source->b1_point];
		return true;
	}
	return false;
}

static bool train_frontend(
	struct line_source *source, struct qam_demodulator *demodulator,
	struct qam_equalizer *equalizer,
	const struct v34_phase3_profile *profile,
	enum v34_constellation_size constellation)
{
	struct qam_channel channel;
	struct qam_point reference = { 0 };
	size_t settled = 0;
	size_t trained = 0;

	if (v34_data_carrier_channel(profile, SAMPLE_RATE, &channel) !=
		    V34_DATA_CARRIER_OK ||
	    qam_modulator_init(
		    &source->modulator, &channel, INT16_MAX) != QAM_OK ||
	    qam_demodulator_init(demodulator, &channel) != QAM_OK ||
	    v34_training_constellation_point(
		    constellation, 0, 0, &source->point) !=
		    V34_CONSTELLATION_OK)
		return false;
	source->stage = SOURCE_CONSTANT;
	for (size_t index = 0;
	     index < FRONTEND_SAMPLE_LIMIT &&
	     settled < FRONTEND_SETTLE_SYMBOLS;
	     ++index) {
		struct qam_point received;
		int16_t sample;
		bool accepted;
		bool ready;

		if (!source_sample(source, &sample, &accepted) ||
		    qam_demodulator_sample(
			    demodulator, sample, &received, &ready) != QAM_OK)
			return false;
		(void)accepted;
		if (ready) {
			reference = received;
			settled++;
		}
	}
	if (settled != FRONTEND_SETTLE_SYMBOLS ||
	    !qam_demodulator_measure(demodulator).timing_locked ||
	    qam_equalizer_init(
		    equalizer, &reference, &source->point) != QAM_EQUALIZER_OK ||
	    !source_start_trn(source, profile->local_role, constellation))
		return false;
	for (size_t index = 0;
	     index < FRONTEND_SAMPLE_LIMIT &&
	     trained < EQUALIZER_TRAINING_SYMBOLS;
	     ++index) {
		struct qam_equalizer_observation observation = { 0 };
		struct qam_point received;
		struct qam_point target;
		int16_t sample;
		uint8_t quarter_point;
		uint8_t clockwise_quarter_turns;
		bool accepted;
		bool ready;
		bool equalized_ready;

		if (!source_sample(source, &sample, &accepted) ||
		    (accepted && !source_next_trn(source)) ||
		    qam_demodulator_sample(
			    demodulator, sample, &received, &ready) != QAM_OK)
			return false;
		if (!ready)
			continue;
		if (qam_equalizer_observe(
			    equalizer, &received, &observation,
			    &equalized_ready) != QAM_EQUALIZER_OK)
			return false;
		if (equalized_ready &&
		    (v34_training_constellation_slice(
			     constellation, &observation.output,
			     &quarter_point, &clockwise_quarter_turns) !=
			     V34_CONSTELLATION_OK ||
		     v34_training_constellation_point(
			     constellation, quarter_point,
			     clockwise_quarter_turns, &target) !=
			     V34_CONSTELLATION_OK ||
		     qam_equalizer_adapt_observation(
			     equalizer, &observation, &target, 0.05) !=
			     QAM_EQUALIZER_OK))
			return false;
		trained++;
	}
	return trained == EQUALIZER_TRAINING_SYMBOLS &&
	       qam_equalizer_is_valid(equalizer) &&
	       !equalizer->can_adapt;
}

static bool data_configuration(
	const struct v34_phase3_profile *profile,
	struct v34_data_line_receiver_configuration *configuration)
{
	struct v34_symbol_power_measurement measurement = {
		.mean_squared_magnitude = 1.0,
		.symbol_count = 1,
	};
	struct v34_mapping_parameters parameters;
	struct v34_data_line_receiver_configuration configured = {
		.transmitting_profile = *profile,
		.equalizer_adaptation = 0.01,
		.trellis_selection = V34_TRELLIS_16_STATE,
	};

	if (v34_frame_geometry_init(
		    &configured.geometry, profile->symbol_rate, 4, false) !=
		    V34_FRAME_SCHEDULE_OK ||
	    v34_mapping_parameters_init(
		    &parameters, &configured.geometry, false) !=
		    V34_MAPPING_PARAMETERS_OK ||
	    v34_power_normalizer_init(
		    &configured.normalizer, &parameters,
		    V34_NONLINEAR_THETA_ZERO, &measurement, &measurement) !=
		    V34_SYMBOL_POWER_OK)
		return false;
	configured.precoder_coefficients[0] =
		(struct v34_q14_coefficient){ 4096, 2048 };
	configured.precoder_coefficients[1] =
		(struct v34_q14_coefficient){ -2048, 1024 };
	configured.precoder_coefficients[2] =
		(struct v34_q14_coefficient){ 1024, -2048 };
	*configuration = configured;
	return true;
}

static bool handle_record(
	struct v34_phase4_line_receiver *receiver,
	const struct v34_mp_receive_event *event,
	const struct v34_mp_parameters *initial_parameters,
	const struct v34_mp_parameters *final_parameters,
	unsigned int *initial_records, bool *final_record)
{
	if (event->type == V34_MP_RECEIVE_REJECTED)
		return v34_phase4_line_receiver_continue_mp(receiver) ==
		       V34_PHASE4_LINE_RECEIVER_OK;
	if (event->type != V34_MP_RECEIVE_TYPE0 ||
	    event->frame_result != V34_MP_OK)
		return false;
	if (parameters_are_equal(
		    &event->parameters.type0, initial_parameters)) {
		(*initial_records)++;
		return v34_phase4_line_receiver_continue_mp(receiver) ==
		       V34_PHASE4_LINE_RECEIVER_OK;
	}
	if (!parameters_are_equal(
		    &event->parameters.type0, final_parameters) ||
	    *final_record)
		return false;
	*final_record = true;
	return v34_phase4_line_receiver_start_e(receiver) ==
	       V34_PHASE4_LINE_RECEIVER_OK;
}

static bool run_line_case(
	enum v34_station_role role,
	enum v34_constellation_size constellation)
{
	struct v34_phase3_profile profile = profile_for(role);
	struct line_source source = {
		.initial_parameters = parameters(false),
		.final_parameters = parameters(true),
	};
	struct v34_data_line_receiver_configuration data_config;
	struct v34_phase4_line_receiver receiver;
	struct v34_data_line_receiver data_receiver;
	struct v34_data_line_receiver saved_data_receiver;
	struct qam_demodulator demodulator;
	struct qam_equalizer equalizer;
	unsigned int control_trn_symbols = 0;
	unsigned int initial_records = 0;
	bool final_record = false;
	bool e_complete = false;

	CHECK(train_frontend(
		      &source, &demodulator, &equalizer, &profile,
		      constellation) &&
	      data_configuration(&profile, &data_config) &&
	      v34_phase4_line_receiver_init(
		      &receiver, &demodulator, &equalizer, &profile,
		      constellation, 0.03) == V34_PHASE4_LINE_RECEIVER_OK,
	      "Phase 4 line fixture did not initialize role=%d constellation=%d",
	      role, constellation);
	memset(&data_receiver, 0xa5, sizeof(data_receiver));
	saved_data_receiver = data_receiver;
	CHECK(v34_phase4_line_receiver_start_data(
		      &receiver, &data_receiver, &data_config) ==
		      V34_PHASE4_LINE_RECEIVER_INVALID_STATE &&
	      memcmp(&data_receiver, &saved_data_receiver,
		     sizeof(data_receiver)) == 0,
	      "Phase 4 line exported data before E");
	for (size_t index = 0;
	     index < SCRIPT_SAMPLE_LIMIT && !e_complete;
	     ++index) {
		struct v34_mp_receive_event event;
		struct v34_mp_receive_event saved_event;
		int16_t sample;
		bool accepted;
		enum v34_phase4_line_receiver_result result;

		CHECK(source_sample(&source, &sample, &accepted),
		      "Phase 4 line source failed at sample %zu", index);
		if (accepted) {
			if (source.stage == SOURCE_TRN &&
			    ++control_trn_symbols == CONTROL_TRN_SYMBOLS)
				CHECK(source_start_mp(&source),
				      "Phase 4 MP source did not start");
			else
				CHECK(source_advance(&source),
				      "Phase 4 source did not advance");
		}
		memset(&event, 0x5a, sizeof(event));
		saved_event = event;
		result = v34_phase4_line_receiver_sample(
			&receiver, sample, &event);
		CHECK(result != V34_PHASE4_LINE_RECEIVER_FAILED &&
		      result != V34_PHASE4_LINE_RECEIVER_NUMERIC_FAILURE &&
		      result != V34_PHASE4_LINE_RECEIVER_SEQUENCE_MISMATCH &&
		      result != V34_PHASE4_LINE_RECEIVER_INVALID_ARGUMENT &&
		      result != V34_PHASE4_LINE_RECEIVER_INVALID_STATE,
		      "Phase 4 line failed at sample %zu: %s", index,
		      v34_phase4_line_receiver_result_name(result));
		if (result == V34_PHASE4_LINE_RECEIVER_RECORD_READY) {
			struct v34_phase4_line_receiver paused = receiver;
			struct v34_mp_receive_event ignored = { 0 };

			CHECK(v34_phase4_line_receiver_sample(
				      &receiver, sample, &ignored) ==
				      V34_PHASE4_LINE_RECEIVER_INVALID_STATE &&
			      memcmp(&receiver, &paused, sizeof(receiver)) == 0 &&
			      handle_record(
				      &receiver, &event,
				      &source.initial_parameters,
				      &source.final_parameters,
				      &initial_records, &final_record),
			      "Phase 4 record decision was not transactional");
		} else {
			CHECK(memcmp(&event, &saved_event, sizeof(event)) == 0,
			      "non-record line result changed the MP event");
		}
		e_complete =
			result == V34_PHASE4_LINE_RECEIVER_E_COMPLETE;
	}
	CHECK(e_complete && initial_records != 0 && final_record &&
	      v34_phase4_line_receiver_is_valid(&receiver) &&
	      !v34_phase4_line_receiver_has_failed(&receiver) &&
	      receiver.control.stage ==
		      V34_PHASE4_EQUALIZER_RECEIVER_STAGE_COMPLETE,
	      "Phase 4 line did not complete MP/E role=%d constellation=%d",
	      role, constellation);
	CHECK(v34_phase4_line_receiver_start_data(
		      &receiver, &data_receiver, &data_config) ==
		      V34_PHASE4_LINE_RECEIVER_OK &&
	      v34_data_line_receiver_is_valid(&data_receiver) &&
	      data_receiver.equalizer_receiver.training_outputs_remaining == 0 &&
	      memcmp(&data_receiver.demodulator, &receiver.demodulator,
		     sizeof(data_receiver.demodulator)) == 0,
	      "Phase 4 line did not preserve its completed data boundary");
	{
		struct v34_phase4_line_receiver saved = receiver;
		struct v34_mp_receive_event event;
		struct v34_mp_receive_event saved_event;

		memset(&event, 0x6b, sizeof(event));
		saved_event = event;
		CHECK(v34_phase4_line_receiver_sample(
			      &receiver, 0, &event) ==
			      V34_PHASE4_LINE_RECEIVER_COMPLETE &&
		      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
		      memcmp(&event, &saved_event, sizeof(event)) == 0,
		      "completed Phase 4 line consumed another sample");
	}
	return true;
}

static bool test_generated_line_sequences(void)
{
	static const enum v34_station_role ROLES[] = {
		V34_CALLING_STATION,
		V34_ANSWERING_STATION,
	};
	static const enum v34_constellation_size CONSTELLATIONS[] = {
		V34_CONSTELLATION_4_POINT,
		V34_CONSTELLATION_16_POINT,
	};

	for (size_t role = 0; role < ARRAY_LENGTH(ROLES); ++role)
		for (size_t constellation = 0;
		     constellation < ARRAY_LENGTH(CONSTELLATIONS);
		     ++constellation)
			if (!run_line_case(
				    ROLES[role], CONSTELLATIONS[constellation]))
				return false;
	return true;
}

static bool test_result_names(void)
{
	return strcmp(v34_phase4_line_receiver_result_name(
			      V34_PHASE4_LINE_RECEIVER_OK),
		      "ok") == 0 &&
	       strcmp(v34_phase4_line_receiver_result_name(
			      V34_PHASE4_LINE_RECEIVER_RECORD_READY),
		      "record ready") == 0 &&
	       strcmp(v34_phase4_line_receiver_result_name(
			      V34_PHASE4_LINE_RECEIVER_E_COMPLETE),
		      "E complete") == 0 &&
	       strcmp(v34_phase4_line_receiver_result_name(
			      V34_PHASE4_LINE_RECEIVER_SEQUENCE_MISMATCH),
		      "sequence mismatch") == 0 &&
	       strcmp(v34_phase4_line_receiver_result_name(
			      (enum v34_phase4_line_receiver_result)99),
		      "unknown") == 0;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated line sequences", test_generated_line_sequences },
		{ "result names", test_result_names },
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
