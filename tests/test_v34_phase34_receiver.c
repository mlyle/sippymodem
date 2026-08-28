// SPDX-License-Identifier: BSD-3-Clause

#include <v34_data_mapping.h>
#include <v34_data_stream.h>
#include <v34_mapping_parameters.h>
#include <v34_phase34_receiver.h>
#include <v34_training_transmitter.h>

#include "test.h"

#include <string.h>

enum {
	SAMPLE_RATE = 8000,
	TEST_INITIAL_RECEIVE_GAIN = 0x0600,
	PHASE3_TRN_SYMBOLS = V34_PHASE3_TRN_MIN_SYMBOLS + 128,
	PHASE3_J_SYMBOLS = V34_PHASE3_J_PATTERN_SYMBOLS * 5,
	PHASE4_TRN_SYMBOLS = V34_PHASE4_TRN_MIN_SYMBOLS + 384,
	SAMPLE_LIMIT = 40000,
};

struct line_source {
	struct v34_training_transmitter training;
	struct v34_data_mapping mapping;
	struct v34_data_stream data;
	struct v34_data_direction_parameters parameters;
	struct v34_mp_parameters initial_mp;
	struct v34_mp_parameters final_mp;
	unsigned int completed_records;
	bool data_started;
};

static struct v34_data_direction_parameters data_parameters(uint8_t rate)
{
	return (struct v34_data_direction_parameters){
		.precoder_coefficients = {
			{ 0, 0 },
			{ 0, 0 },
			{ 0, 0 },
		},
		.trellis_selection = V34_TRELLIS_16_STATE,
		.nonlinear_parameter = V34_NONLINEAR_THETA_ZERO,
		.primary_data_rate_2400_bps_units = rate,
	};
}

static bool profile(
	struct v34_phase3_profile *selected, enum v34_station_role role,
	enum v34_symbol_rate symbol_rate, uint8_t rate, bool high_carrier,
	uint8_t preemphasis_filter_index)
{
	const struct v34_info1_probe_result probe = {
		.high_carrier = high_carrier,
		.preemphasis_filter_index = preemphasis_filter_index,
		.projected_data_rate_2400_bps_units = rate,
	};

	return v34_phase3_profile_init(
		       selected, role, symbol_rate, &probe, 0, 0) ==
	       V34_PHASE3_PROFILE_OK;
}

static struct v34_mp_parameters mp_parameters(
	uint8_t rate, bool acknowledges_peer)
{
	return (struct v34_mp_parameters){
		.maximum_call_to_answer_rate_2400_bps_units = rate,
		.maximum_answer_to_call_rate_2400_bps_units = rate,
		.remote_transmitter_trellis = V34_TRELLIS_16_STATE,
		.acknowledges_peer_mp = acknowledges_peer,
		.data_rate_capability_mask = 0x1555,
	};
}

static bool parameters_match(
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

static bool received_all_ones(
	const struct v34_received_mapping_frame *received)
{
	for (size_t bit = 0; bit < received->primary_bit_count; ++bit)
		if (received->primary_bits[bit] != 1u)
			return false;
	return received->auxiliary_bit_count == 0u ||
	       received->auxiliary_bit == 1u;
}

static bool source_init(
	struct line_source *source,
	const struct v34_phase3_profile *transmitting_profile, uint8_t rate)
{
	int16_t nominal_peak_amplitude =
		transmitting_profile->symbol_rate == V34_SYMBOL_RATE_2800 &&
			!transmitting_profile->high_carrier ?
			7000 :
			5000;
	struct line_source initialized = {
		.parameters = data_parameters(rate),
		.initial_mp = mp_parameters(rate, false),
		.final_mp = mp_parameters(rate, true),
	};

	if (v34_training_transmitter_init(
		    &initialized.training, transmitting_profile,
		    V34_CONSTELLATION_16_POINT, SAMPLE_RATE,
		    nominal_peak_amplitude) !=
		    V34_TRAINING_TRANSMITTER_OK ||
	    v34_training_transmitter_select_phase4_constellation(
		    &initialized.training, V34_CONSTELLATION_16_POINT) !=
		    V34_TRAINING_TRANSMITTER_OK ||
	    v34_data_mapping_prepare(
		    &initialized.mapping, transmitting_profile,
		    &initialized.parameters, false) != V34_DATA_MAPPING_OK)
		return false;
	*source = initialized;
	return true;
}

static bool prepare_source(struct line_source *source)
{
	struct v34_training_transmitter *training = &source->training;

	if (training->stage == V34_TRAINING_TRANSMITTER_PHASE3 &&
	    training->phase3.stage == V34_PHASE3_TRANSMITTER_TRN &&
	    training->phase3.trn_symbols == PHASE3_TRN_SYMBOLS)
		return v34_training_transmitter_start_j(training) ==
		       V34_TRAINING_TRANSMITTER_OK;
	if (training->stage == V34_TRAINING_TRANSMITTER_PHASE3 &&
	    training->phase3.stage == V34_PHASE3_TRANSMITTER_J &&
	    training->phase3.j_symbols == PHASE3_J_SYMBOLS)
		return v34_training_transmitter_finish_j(training) ==
		       V34_TRAINING_TRANSMITTER_OK;
	if (training->stage == V34_TRAINING_TRANSMITTER_PHASE4_TRN &&
	    training->phase4_trn_symbols == PHASE4_TRN_SYMBOLS)
		return v34_training_transmitter_start_mp_type0(
			       training, &source->initial_mp) ==
		       V34_TRAINING_TRANSMITTER_OK;
	if (v34_training_transmitter_phase4_mp_record_complete(training)) {
		source->completed_records++;
		if (source->completed_records >= 3u)
			return v34_training_transmitter_start_e(training) ==
			       V34_TRAINING_TRANSMITTER_OK;
		return v34_training_transmitter_start_mp_type0(
			       training,
			       source->completed_records == 2u ?
				       &source->final_mp :
				       &source->initial_mp) ==
		       V34_TRAINING_TRANSMITTER_OK;
	}
	return true;
}

static bool source_sample(struct line_source *source, int16_t *sample)
{
	if (v34_training_transmitter_phase4_e_complete(&source->training)) {
		bool accepted;

		if (!source->data_started) {
			if (v34_data_stream_continue_prepared_b1(
				    &source->data, &source->training.line,
				    &source->mapping) != V34_DATA_STREAM_OK)
				return false;
			source->data_started = true;
		}
		return v34_data_stream_sample(
			       &source->data, NULL, NULL, sample, &accepted) ==
		       V34_DATA_STREAM_OK;
	}
	return prepare_source(source) &&
	       v34_training_transmitter_sample(&source->training, sample) ==
		       V34_TRAINING_TRANSMITTER_OK;
}

static bool test_validation(void)
{
	const struct v34_info1_probe_result reduced_probe = {
		.preemphasis_filter_index = 3,
		.projected_data_rate_2400_bps_units = 8,
	};
	struct v34_phase3_profile valid;
	struct v34_phase3_profile invalid;
	struct v34_phase3_profile reduced;
	struct v34_phase34_receiver receiver;
	struct v34_phase34_receiver saved;
	struct v34_phase3_decision_handoff handoff;
	struct v34_phase3_decision_handoff saved_handoff;
	struct v34_phase34_receive_output output;
	struct v34_phase34_receive_output saved_output;

	CHECK(profile(
		      &valid, V34_CALLING_STATION, V34_SYMBOL_RATE_3000, 8,
		      true, 3) &&
	      v34_phase3_profile_init(
		      &reduced, V34_CALLING_STATION, V34_SYMBOL_RATE_2800,
		      &reduced_probe, 0, 0) == V34_PHASE3_PROFILE_OK,
	      "validation profile did not initialize");
	invalid = valid;
	memset(&receiver, 0xa5, sizeof(receiver));
	saved = receiver;
	invalid.symbol_rate_denominator = 0;
	CHECK(v34_phase34_receiver_init(
		      NULL, &valid, V34_CONSTELLATION_16_POINT,
		      TEST_INITIAL_RECEIVE_GAIN) ==
		      V34_PHASE34_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase34_receiver_init(
		      &receiver, &invalid, V34_CONSTELLATION_16_POINT,
		      TEST_INITIAL_RECEIVE_GAIN) ==
		      V34_PHASE34_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
	      v34_phase34_receiver_init(
		      &receiver, &valid, (enum v34_constellation_size)8,
		      TEST_INITIAL_RECEIVE_GAIN) ==
		      V34_PHASE34_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase34_receiver_init(
		      &receiver, &valid, V34_CONSTELLATION_16_POINT, 0) ==
		      V34_PHASE34_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
	      "invalid initialization changed its destination");
	CHECK(v34_phase34_receiver_init(
		      &receiver, &valid, V34_CONSTELLATION_16_POINT,
		      TEST_INITIAL_RECEIVE_GAIN) ==
		      V34_PHASE34_RECEIVER_OK &&
	      v34_phase34_receiver_is_valid(&receiver) &&
	      receiver.timing.gain.gain == TEST_INITIAL_RECEIVE_GAIN,
	      "valid receiver did not initialize");
	CHECK(v34_phase34_receiver_init(
		      &receiver, &reduced, V34_CONSTELLATION_16_POINT,
		      TEST_INITIAL_RECEIVE_GAIN) ==
		      V34_PHASE34_RECEIVER_OK &&
	      receiver.timing.gain.gain == TEST_INITIAL_RECEIVE_GAIN,
	      "profile-independent receive gain changed");
	CHECK(v34_phase34_receiver_init(
		      &receiver, &valid, V34_CONSTELLATION_16_POINT,
		      TEST_INITIAL_RECEIVE_GAIN) ==
		      V34_PHASE34_RECEIVER_OK,
	      "validation receiver did not restore");
	saved = receiver;
	memset(&output, 0x6b, sizeof(output));
	saved_output = output;
	for (unsigned int sample = 0; sample < 37u; ++sample)
		CHECK(v34_phase34_receiver_sample(&receiver, 0, &output) ==
			      V34_PHASE34_RECEIVER_OK,
		      "leading silence failed at sample %u", sample);
	CHECK(!v34_receive_timing_is_acquired(&receiver.timing) &&
	      memcmp(&receiver.equalizer, &saved.equalizer,
		     sizeof(receiver.equalizer)) == 0 &&
	      memcmp(&receiver.semantic, &saved.semantic,
		     sizeof(receiver.semantic)) == 0 &&
	      memcmp(&output, &saved_output, sizeof(output)) == 0,
	      "leading silence advanced semantic reception or output");
	saved = receiver;
	memset(&handoff, 0x7c, sizeof(handoff));
	saved_handoff = handoff;
	CHECK(v34_phase34_receiver_sample(&receiver, 1, NULL) ==
		      V34_PHASE34_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase34_receiver_sample(
		      &receiver, 1,
		      (struct v34_phase34_receive_output *)&receiver) ==
		      V34_PHASE34_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase34_receiver_continue_mp(&receiver) ==
		      V34_PHASE34_RECEIVER_INVALID_STATE &&
	      v34_phase34_receiver_expect_e(&receiver) ==
		      V34_PHASE34_RECEIVER_INVALID_STATE &&
	      v34_phase34_receiver_phase3_handoff(&receiver, NULL) ==
		      V34_PHASE34_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase34_receiver_phase3_handoff(&receiver, &handoff) ==
		      V34_PHASE34_RECEIVER_INVALID_STATE &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
	      memcmp(&handoff, &saved_handoff, sizeof(handoff)) == 0,
	      "invalid controls changed an incomplete receiver");
	return true;
}

static bool test_tone_owned_acquisition(void)
{
	struct v34_phase3_profile transmitting_profile;
	struct v34_phase34_receiver receiver;
	struct v34_adaptive_equalizer saved_equalizer;
	union v34_phase34_receiver_semantic_state saved_semantic;
	struct v34_phase34_receive_output output;
	struct v34_phase34_receive_output saved_output;

	CHECK(profile(
		      &transmitting_profile, V34_CALLING_STATION,
		      V34_SYMBOL_RATE_3000, 8, true, 3) &&
	      v34_phase34_receiver_init(
		      &receiver, &transmitting_profile,
		      V34_CONSTELLATION_16_POINT,
		      TEST_INITIAL_RECEIVE_GAIN) ==
		      V34_PHASE34_RECEIVER_OK,
	      "tone-owned receiver did not initialize");
	saved_equalizer = receiver.equalizer;
	saved_semantic = receiver.semantic;
	memset(&output, 0x6d, sizeof(output));
	saved_output = output;
	for (unsigned int index = 0; index < 96u; ++index) {
		int16_t sample = (index & 1u) != 0u ? 400 : -400;

		CHECK(v34_phase34_receiver_sample(&receiver, sample, &output) ==
			      V34_PHASE34_RECEIVER_OK,
		      "codec noise failed at sample %u", index);
	}
	CHECK(!v34_receive_timing_is_acquired(&receiver.timing) &&
	      memcmp(
		      &receiver.equalizer, &saved_equalizer,
		      sizeof(saved_equalizer)) == 0 &&
	      memcmp(
		      &receiver.semantic, &saved_semantic,
		      sizeof(saved_semantic)) == 0 &&
	      memcmp(&output, &saved_output, sizeof(output)) == 0 &&
	      v34_phase34_receiver_is_valid(&receiver),
	      "codec noise advanced semantic reception");

	CHECK(v34_phase34_receiver_init(
		      &receiver, &transmitting_profile,
		      V34_CONSTELLATION_16_POINT,
		      TEST_INITIAL_RECEIVE_GAIN) ==
		      V34_PHASE34_RECEIVER_OK,
	      "impulse receiver did not initialize");
	for (unsigned int index = 0; index < 96u; ++index) {
		int16_t sample = index == 0u ? INT16_MAX : 0;

		CHECK(v34_phase34_receiver_sample(&receiver, sample, &output) ==
			      V34_PHASE34_RECEIVER_OK,
		      "isolated impulse failed at sample %u", index);
	}
	CHECK(!v34_receive_timing_is_acquired(&receiver.timing) &&
	      v34_phase34_receiver_is_valid(&receiver),
	      "isolated impulse acquired S timing");

	CHECK(v34_phase34_receiver_init(
		      &receiver, &transmitting_profile,
		      V34_CONSTELLATION_16_POINT,
		      TEST_INITIAL_RECEIVE_GAIN) ==
		      V34_PHASE34_RECEIVER_OK &&
	      v34_phase34_receiver_sample(&receiver, 400, &output) ==
		      V34_PHASE34_RECEIVER_OK,
	      "corrupt-state receiver did not initialize");
	receiver.timing.s_detector.carrier = V34_S_TONE_CARRIER_COUNT;
	{
		struct v34_phase34_receiver saved = receiver;

		CHECK(!v34_phase34_receiver_is_valid(&receiver) &&
		      v34_phase34_receiver_sample(&receiver, 400, &output) ==
			      V34_PHASE34_RECEIVER_INVALID_STATE &&
		      memcmp(&receiver, &saved, sizeof(saved)) == 0 &&
		      memcmp(&output, &saved_output, sizeof(output)) == 0,
		      "corrupt timing state was not rejected transactionally");
	}
	return true;
}

static bool test_training_case(
	enum v34_station_role role, enum v34_symbol_rate symbol_rate,
	uint8_t rate, bool high_carrier, uint8_t preemphasis_filter_index)
{
	struct v34_phase3_profile transmitting_profile;
	struct v34_phase34_receiver receiver;
	struct v34_phase3_decision_handoff handoff;
	struct line_source source;
	unsigned int timing_events = 0;
	unsigned int phase3_s_events = 0;
	unsigned int pp_events = 0;
	unsigned int j_events = 0;
	unsigned int phase4_s_events = 0;
	unsigned int trn_events = 0;
	unsigned int initial_records = 0;
	unsigned int rejected_records = 0;
	unsigned int b1_frames = 0;
	unsigned int mapping_frames = 0;
	uint64_t phase3_preroll_epoch = 0;
	struct v34_adaptive_equalizer phase4_rearm_equalizer;
	struct v34_receive_timing phase4_rearm_timing;
	bool final_record = false;
	bool e_complete = false;
	bool b1_complete = false;
	bool phase4_equalizer_saved = false;

	CHECK(profile(
		      &transmitting_profile, role, symbol_rate, rate,
		      high_carrier, preemphasis_filter_index) &&
	      v34_phase34_receiver_init(
		      &receiver, &transmitting_profile,
		      V34_CONSTELLATION_16_POINT,
		      TEST_INITIAL_RECEIVE_GAIN) ==
		      V34_PHASE34_RECEIVER_OK &&
	      source_init(&source, &transmitting_profile, rate),
	      "role %u rate %u training pair did not initialize", role, rate);
	for (unsigned int index = 0;
	     index < SAMPLE_LIMIT && mapping_frames < 3u; ++index) {
		struct v34_phase34_receive_output output;
		struct v34_phase34_receive_output saved_output;
		enum v34_phase34_receiver_result result;
		int16_t sample;

		CHECK(source_sample(&source, &sample),
		      "role %u rate %u source stopped at sample %u", role,
		      rate, index);
		memset(&output, 0x5d, sizeof(output));
		saved_output = output;
		result = v34_phase34_receiver_sample(&receiver, sample, &output);
		if (receiver.equalizer.epoch > 0u &&
		    receiver.equalizer.epoch <
			    V34_ADAPTIVE_EQUALIZER_FIRST_SIGNAL_EPOCH &&
		    receiver.stage == V34_PHASE34_RECEIVER_PHASE3 &&
		    receiver.semantic.phase3.stage ==
			    V34_PHASE3_DECISION_RECEIVER_S) {
			CHECK(receiver.semantic.phase3.s.buffered_decisions == 0u &&
			      !receiver.semantic.phase3.s.target_seeded &&
			      !receiver.equalizer.decision_pending,
			      "role %u rate %u changed Phase 3 S during preroll",
			      role, rate);
			if (phase3_preroll_epoch != receiver.equalizer.epoch) {
				CHECK(receiver.equalizer.epoch ==
					      phase3_preroll_epoch + 1u,
				      "role %u rate %u skipped Phase 3 preroll epoch",
				      role, rate);
				phase3_preroll_epoch = receiver.equalizer.epoch;
			}
		}
		switch (result) {
		case V34_PHASE34_RECEIVER_OK:
			break;
		case V34_PHASE34_RECEIVER_TIMING_ACQUIRED:
			CHECK(receiver.stage == V34_PHASE34_RECEIVER_PHASE3 &&
			      receiver.equalizer.epoch == 0u &&
			      receiver.equalizer.inputs_since_observation == 0u &&
			      !receiver.equalizer.decision_pending &&
			      receiver.semantic.phase3.stage ==
					V34_PHASE3_DECISION_RECEIVER_S &&
			      receiver.semantic.phase3.s.buffered_decisions == 0u &&
			      !receiver.semantic.phase3.s.target_seeded,
			      "role %u rate %u adapted before S acquisition",
			      role, rate);
			timing_events++;
			break;
		case V34_PHASE34_RECEIVER_PHASE3_S_DETECTED:
			CHECK(timing_events == 1u,
			      "role %u rate %u accepted S before timing", role,
			      rate);
			phase3_s_events++;
			break;
		case V34_PHASE34_RECEIVER_PP_COMPLETE:
			pp_events++;
			break;
		case V34_PHASE34_RECEIVER_J_DETECTED:
			if (role == V34_ANSWERING_STATION) {
				phase4_rearm_equalizer = receiver.equalizer;
				phase4_rearm_timing = receiver.timing;
				phase4_equalizer_saved = true;
				CHECK(receiver.stage ==
					      V34_PHASE34_RECEIVER_PHASE4 &&
				      receiver.semantic.phase4.stage ==
					      V34_PHASE4_DECISION_RECEIVER_ANSWER_S &&
				      receiver.tracking_started &&
				      !receiver.phase4_s_rearmed &&
				      v34_phase34_receiver_rearm_phase4_s(
					      &receiver) ==
					      V34_PHASE34_RECEIVER_OK &&
				      receiver.phase4_s_rearmed &&
				      memcmp(&receiver.timing,
					     &phase4_rearm_timing,
					     sizeof(receiver.timing)) == 0 &&
				      memcmp(&receiver.equalizer,
					     &phase4_rearm_equalizer,
					     sizeof(receiver.equalizer)) == 0 &&
				      !receiver.tracking_started,
				      "role %u rate %u lost the Phase 4 receive lineage",
				      role, rate);
			}
			j_events++;
			break;
		case V34_PHASE34_RECEIVER_PHASE4_S_DETECTED:
			CHECK(receiver.tracking_started &&
			      receiver.tracking_symbols == 0u &&
			      v34_phase3_s_receiver_is_complete(
				      &receiver.semantic.phase4.answer_s),
			      "role %u rate %u did not start Phase 4 tracking",
			      role, rate);
			phase4_s_events++;
			break;
		case V34_PHASE34_RECEIVER_TRN_COMPLETE:
			trn_events++;
			break;
		case V34_PHASE34_RECEIVER_RECORD_READY:
			if (output.mp.type == V34_MP_RECEIVE_REJECTED) {
				rejected_records++;
				CHECK(v34_phase34_receiver_continue_mp(&receiver) ==
					      V34_PHASE34_RECEIVER_OK,
				      "role %u rate %u rejected MP did not resume",
				      role, rate);
			} else if (output.mp.type == V34_MP_RECEIVE_TYPE0 &&
				   output.mp.frame_result == V34_MP_OK &&
				   parameters_match(
					   &output.mp.parameters.type0,
					   &source.initial_mp)) {
				initial_records++;
				CHECK(v34_phase34_receiver_continue_mp(&receiver) ==
					      V34_PHASE34_RECEIVER_OK,
				      "role %u rate %u initial MP did not resume",
				      role, rate);
			} else {
				CHECK(output.mp.type == V34_MP_RECEIVE_TYPE0 &&
				      output.mp.frame_result == V34_MP_OK &&
				      !final_record &&
				      parameters_match(
					      &output.mp.parameters.type0,
					      &source.final_mp) &&
				      v34_phase34_receiver_expect_e(&receiver) ==
					      V34_PHASE34_RECEIVER_OK,
				      "role %u rate %u final MP was not accepted",
				      role, rate);
				final_record = true;
			}
			continue;
		case V34_PHASE34_RECEIVER_E_COMPLETE: {
			struct v34_phase34_receiver saved = receiver;
			struct v34_phase34_receiver feedback_receiver = receiver;
			struct v34_q14_coefficient
				feedback_coefficients[V34_PRECODER_TAP_COUNT];
			struct v34_q14_coefficient
				zero_coefficients[V34_PRECODER_TAP_COUNT];
			struct v34_q14_coefficient changed_coefficients[
				V34_PRECODER_TAP_COUNT];
			struct v34_frame_geometry wrong =
				source.mapping.schedule.geometry;

			wrong.symbol_rate =
				source.mapping.schedule.geometry.symbol_rate ==
						V34_SYMBOL_RATE_2800 ?
					V34_SYMBOL_RATE_3000 :
					V34_SYMBOL_RATE_2800;
			CHECK(v34_phase34_receiver_is_complete(&receiver) &&
			      v34_phase34_receiver_finish_feedback_training(
				      &receiver, false,
				      (struct v34_q14_coefficient *)&receiver) ==
				      V34_PHASE34_RECEIVER_INVALID_ARGUMENT &&
			      v34_phase34_receiver_start_data(
				      &receiver,
				      (const struct v34_power_normalizer *)&receiver,
				      &source.mapping.schedule.geometry, false,
				      source.parameters.precoder_coefficients,
				      source.parameters.trellis_selection) ==
				      V34_PHASE34_RECEIVER_INVALID_ARGUMENT &&
			      v34_phase34_receiver_start_data(
				      &receiver, &source.mapping.normalizer,
				      (const struct v34_frame_geometry *)&receiver,
				      false,
				      source.parameters.precoder_coefficients,
				      source.parameters.trellis_selection) ==
				      V34_PHASE34_RECEIVER_INVALID_ARGUMENT &&
			      v34_phase34_receiver_start_data(
				      &receiver, &source.mapping.normalizer,
				      &source.mapping.schedule.geometry, false,
				      (const struct v34_q14_coefficient *)&receiver,
				      source.parameters.trellis_selection) ==
				      V34_PHASE34_RECEIVER_INVALID_ARGUMENT &&
			      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
			      v34_phase34_receiver_start_data(
				      &receiver, &source.mapping.normalizer,
				      &source.mapping.schedule.geometry,
				      source.parameters.expanded_shaping,
				      source.parameters.precoder_coefficients,
				      source.parameters.trellis_selection) ==
				      V34_PHASE34_RECEIVER_INVALID_STATE &&
			      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
			      v34_phase34_receiver_finish_feedback_training(
				      &feedback_receiver, true,
				      feedback_coefficients) ==
				      V34_PHASE34_RECEIVER_OK &&
			      v34_phase34_receiver_finish_feedback_training(
				      &receiver, false, zero_coefficients) ==
				      V34_PHASE34_RECEIVER_OK,
			      "role %u rate %u did not freeze feedback at E", role,
			      rate);
			CHECK(memcmp(zero_coefficients,
				     source.parameters.precoder_coefficients,
				     sizeof(zero_coefficients)) == 0 &&
			      !feedback_receiver.equalizer.decision_pending &&
			      feedback_receiver.feedback_training_complete &&
			      feedback_receiver.precoder_selected &&
			      receiver.feedback_training_complete &&
			      !receiver.precoder_selected,
			      "role %u rate %u selected the wrong feedback path", role,
			      rate);
			changed_coefficients[0] = feedback_coefficients[0];
			changed_coefficients[1] = feedback_coefficients[1];
			changed_coefficients[2] = feedback_coefficients[2];
			changed_coefficients[0].real ^= 1;
			saved = feedback_receiver;
			CHECK(v34_phase34_receiver_start_data(
				      &feedback_receiver,
				      &source.mapping.normalizer,
				      &source.mapping.schedule.geometry,
				      source.parameters.expanded_shaping,
				      changed_coefficients,
				      source.parameters.trellis_selection) ==
				      V34_PHASE34_RECEIVER_INVALID_ARGUMENT &&
			      memcmp(&feedback_receiver, &saved,
				     sizeof(feedback_receiver)) == 0 &&
			      v34_phase34_receiver_start_data(
				      &receiver, &source.mapping.normalizer, &wrong,
				      source.parameters.expanded_shaping,
				      source.parameters.precoder_coefficients,
				      source.parameters.trellis_selection) ==
				      V34_PHASE34_RECEIVER_INVALID_ARGUMENT &&
			      v34_phase34_receiver_start_data(
				      &feedback_receiver,
				      &source.mapping.normalizer,
				      &source.mapping.schedule.geometry,
				      source.parameters.expanded_shaping,
				      feedback_coefficients,
				      source.parameters.trellis_selection) ==
				      V34_PHASE34_RECEIVER_OK &&
			      feedback_receiver.equalizer.feedback_mode ==
				      V34_ADAPTIVE_EQUALIZER_FEEDBACK_NONE &&
			      feedback_receiver.timing.timing.mode ==
				      V34_TIMING_CONTROL_HOLD &&
			      v34_phase34_receiver_is_valid(
				      &feedback_receiver) &&
			      v34_phase34_receiver_start_data(
				      &receiver,
				      &source.mapping.normalizer,
				      &source.mapping.schedule.geometry,
				      source.parameters.expanded_shaping,
				      source.parameters.precoder_coefficients,
				      source.parameters.trellis_selection) ==
				      V34_PHASE34_RECEIVER_OK &&
			      receiver.equalizer.feedback_mode ==
				      V34_ADAPTIVE_EQUALIZER_FEEDBACK_NONE &&
			      receiver.timing.timing.mode ==
				      V34_TIMING_CONTROL_HOLD &&
			      receiver.timing.timing.proportional_gain == 0x01f4 &&
			      receiver.timing.timing.integral_gain == 0x0001 &&
			      receiver.timing.timing.timeout == -1,
			      "role %u rate %u did not enter data at E", role,
			      rate);
			saved = receiver;
			CHECK(v34_phase34_receiver_start_data(
				      &receiver, &source.mapping.normalizer,
				      &source.mapping.schedule.geometry,
				      source.parameters.expanded_shaping,
				      source.parameters.precoder_coefficients,
				      source.parameters.trellis_selection) ==
				      V34_PHASE34_RECEIVER_INVALID_STATE &&
			      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
			      "role %u rate %u repeated data start changed state",
			      role, rate);
			saved = receiver;
			CHECK(v34_phase34_receiver_set_data_timing_frozen(
				      &receiver, true) == V34_PHASE34_RECEIVER_OK &&
			      receiver.timing.timing.mode ==
				      V34_TIMING_CONTROL_LOCKED &&
			      v34_phase34_receiver_is_valid(&receiver) &&
			      v34_phase34_receiver_set_data_timing_frozen(
				      &receiver, false) == V34_PHASE34_RECEIVER_OK &&
			      receiver.timing.timing.mode ==
				      V34_TIMING_CONTROL_HOLD &&
			      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
			      "role %u rate %u timing freeze changed data lineage",
			      role, rate);
			e_complete = true;
			break;
		}
		case V34_PHASE34_RECEIVER_INTERVAL_READY:
			continue;
		case V34_PHASE34_RECEIVER_B1_IN_PROGRESS:
			CHECK(received_all_ones(&output.received),
			      "role %u rate %u B1 frame changed after %u bits, "
			      "errors=%u",
			      role, rate,
			      receiver.semantic.data.data_receiver.data_receiver
				      .b1_received_bits,
			      receiver.semantic.data.data_receiver.data_receiver
				      .b1_error_bits);
			b1_frames++;
			continue;
		case V34_PHASE34_RECEIVER_B1_COMPLETE:
			CHECK(received_all_ones(&output.received),
			      "role %u rate %u final B1 frame changed", role,
			      rate);
			b1_frames++;
			b1_complete = true;
			continue;
		case V34_PHASE34_RECEIVER_MAPPING_FRAME_READY:
			CHECK(b1_complete && received_all_ones(&output.received),
			      "role %u rate %u emitted mapped data before B1",
			      role, rate);
			mapping_frames++;
			continue;
		default:
			CHECK(false,
			      "role %u rate %u training failed at sample %u, "
			      "result %u",
			      role, rate, index, result);
		}
		CHECK(memcmp(&output, &saved_output, sizeof(output)) == 0,
		      "role %u rate %u non-record result changed its event",
		      role, rate);
	}
	CHECK(phase3_preroll_epoch + 1u ==
		      V34_ADAPTIVE_EQUALIZER_FIRST_SIGNAL_EPOCH &&
	      (role != V34_ANSWERING_STATION || phase4_equalizer_saved),
	      "role %u rate %u did not preserve receive training: phase3=%llu",
	      role, rate, (unsigned long long)phase3_preroll_epoch);
	CHECK(e_complete && b1_complete && mapping_frames >= 3u &&
	      b1_frames ==
		      source.mapping.schedule.geometry
			      .mapping_frames_per_data_frame &&
	      timing_events == 1u &&
	      phase3_s_events == 1u && pp_events == 1u && j_events == 1u &&
	      phase4_s_events ==
		      (role == V34_ANSWERING_STATION ? 1u : 0u) &&
	      trn_events == 1u &&
	      initial_records != 0u && final_record &&
	      source.completed_records >= 3u && source.data_started &&
	      v34_phase34_receiver_is_valid(&receiver) &&
	      receiver.stage == V34_PHASE34_RECEIVER_DATA &&
	      receiver.equalizer.feedback_mode ==
		      V34_ADAPTIVE_EQUALIZER_FEEDBACK_NONE &&
	      !v34_phase34_receiver_is_complete(&receiver) &&
	      !v34_phase34_receiver_has_failed(&receiver),
	      "role %u symbol=%u rate=%u receiver did not reach E: "
	      "timing=%u S3=%u PP=%u "
	      "J=%u S4=%u TRN=%u initial=%u final=%u source=%u "
	      "rejects=%u B1=%u mapped=%u stage=%u failed=%u",
	      role, symbol_rate, rate, timing_events, phase3_s_events,
	      pp_events, j_events,
	      phase4_s_events, trn_events, initial_records, final_record,
	      source.completed_records, rejected_records, b1_frames,
	      mapping_frames, receiver.stage, receiver.failed);
	CHECK(v34_phase34_receiver_phase3_handoff(&receiver, &handoff) ==
		      V34_PHASE34_RECEIVER_OK &&
	      v34_phase3_decision_handoff_is_valid(&handoff) &&
	      receiver.tracking_started && receiver.tracking_symbols != 0u &&
	      receiver.equalizer.epoch != 0u &&
	      !receiver.equalizer.decision_pending,
	      "role %u rate %u receiver did not retain its physical lineage",
	      role, rate);
	return true;
}

static bool test_training(void)
{
	static const struct {
		enum v34_symbol_rate symbol_rate;
		bool high_carrier;
		bool caller_transmit_only;
		uint8_t preemphasis_filter_index;
	} cases[] = {
		{ V34_SYMBOL_RATE_2400, false, false, 0 },
		{ V34_SYMBOL_RATE_2400, true, false, 0 },
		{ V34_SYMBOL_RATE_2743, false, false, 0 },
		{ V34_SYMBOL_RATE_2743, true, false, 0 },
		{ V34_SYMBOL_RATE_2800, false, true, 0 },
		{ V34_SYMBOL_RATE_2800, true, false, 0 },
		{ V34_SYMBOL_RATE_3000, false, false, 2 },
		{ V34_SYMBOL_RATE_3000, true, false, 2 },
		{ V34_SYMBOL_RATE_3200, false, true, 0 },
		{ V34_SYMBOL_RATE_3200, true, false, 0 },
		{ V34_SYMBOL_RATE_3429, false, false, 7 },
	};
	bool success = true;

	for (size_t index = 0; index < ARRAY_LENGTH(cases); ++index) {
		struct v34_frame_geometry geometry;
		uint8_t selected_rates[V34_INFO1_MAX_DATA_RATE_2400_BPS_UNITS];
		uint8_t selected_count = 0;

		for (uint8_t rate = 1; rate <= V34_INFO1_MAX_DATA_RATE_2400_BPS_UNITS;
		     ++rate) {
			if (v34_frame_geometry_init(
				    &geometry, cases[index].symbol_rate, rate,
				    false) != V34_FRAME_SCHEDULE_OK)
				continue;
			selected_rates[selected_count++] = rate;
		}
		CHECK(selected_count != 0,
		      "symbol rate %u has no supported data geometry",
		      cases[index].symbol_rate);
		/* Exercise only the profile this receiver advertises to its peer.
		 * The transmit owner independently covers every requestable
		 * profile. */
		for (uint8_t selection = 0; selection < selected_count;
		     ++selection)
			for (unsigned int role = V34_CALLING_STATION;
			     role <= V34_ANSWERING_STATION; ++role) {
				if (cases[index].caller_transmit_only &&
				    role != V34_CALLING_STATION)
					continue;
				if (!test_training_case(
					    (enum v34_station_role)role,
					    cases[index].symbol_rate,
					    selected_rates[selection],
					    cases[index].high_carrier,
					    cases[index].preemphasis_filter_index))
					success = false;
			}
	}
	return success;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "validation", test_validation },
		{ "tone-owned acquisition", test_tone_owned_acquisition },
		{ "continuous Phase 3 through data", test_training },
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
