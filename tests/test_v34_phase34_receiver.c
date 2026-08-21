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
	PHASE3_TRN_SYMBOLS = V34_PHASE3_TRN_MIN_SYMBOLS + 128,
	PHASE3_J_SYMBOLS = V34_PHASE3_J_PATTERN_SYMBOLS * 5,
	PHASE4_TRN_SYMBOLS = V34_PHASE4_TRN_MIN_SYMBOLS + 384,
	SAMPLE_LIMIT = 40000,
	MINIMUM_3000_RATE_2400_BPS_UNITS = 2,
	MAXIMUM_3000_RATE_2400_BPS_UNITS = 12,
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

static const struct v34_q14_coefficient FEEDBACK_COEFFICIENTS[] = {
	{ 1, 0 },
	{ 0, 0 },
	{ 0, 0 },
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

static struct v34_phase3_profile profile(
	enum v34_station_role role, uint8_t rate)
{
	return (struct v34_phase3_profile){
		.symbol_rate_numerator = 3000,
		.symbol_rate_denominator = 1,
		.carrier_frequency_numerator = 2000,
		.carrier_frequency_denominator = 1,
		.symbol_rate = V34_SYMBOL_RATE_3000,
		.local_role = role,
		.preemphasis_filter_index = 3,
		.projected_data_rate_2400_bps_units = rate,
		.high_carrier = true,
	};
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
	struct line_source initialized = {
		.parameters = data_parameters(rate),
		.initial_mp = mp_parameters(rate, false),
		.final_mp = mp_parameters(rate, true),
	};

	if (v34_training_transmitter_init(
		    &initialized.training, transmitting_profile,
		    V34_CONSTELLATION_16_POINT, SAMPLE_RATE, 5000) !=
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
	struct v34_phase3_profile valid =
		profile(V34_CALLING_STATION, 8);
	struct v34_phase3_profile invalid = valid;
	struct v34_phase34_receiver receiver;
	struct v34_phase34_receiver saved;
	struct v34_phase3_decision_handoff handoff;
	struct v34_phase3_decision_handoff saved_handoff;
	struct v34_phase34_receive_output output;
	struct v34_phase34_receive_output saved_output;

	memset(&receiver, 0xa5, sizeof(receiver));
	saved = receiver;
	invalid.symbol_rate_denominator = 0;
	CHECK(v34_phase34_receiver_init(
		      NULL, &valid, V34_CONSTELLATION_16_POINT) ==
		      V34_PHASE34_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase34_receiver_init(
		      &receiver, &invalid, V34_CONSTELLATION_16_POINT) ==
		      V34_PHASE34_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
	      v34_phase34_receiver_init(
		      &receiver, &valid, (enum v34_constellation_size)8) ==
		      V34_PHASE34_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
	      "invalid initialization changed its destination");
	CHECK(v34_phase34_receiver_init(
		      &receiver, &valid, V34_CONSTELLATION_16_POINT) ==
		      V34_PHASE34_RECEIVER_OK &&
	      v34_phase34_receiver_is_valid(&receiver),
	      "valid receiver did not initialize");
	saved = receiver;
	memset(&output, 0x6b, sizeof(output));
	saved_output = output;
	for (unsigned int sample = 0; sample < 37u; ++sample)
		CHECK(v34_phase34_receiver_sample(&receiver, 0, &output) ==
			      V34_PHASE34_RECEIVER_OK,
		      "leading silence failed at sample %u", sample);
	CHECK(memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
	      memcmp(&output, &saved_output, sizeof(output)) == 0,
	      "leading silence changed receiver state or output");
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
	      v34_phase34_receiver_start_e(&receiver) ==
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

static bool test_training_case(enum v34_station_role role, uint8_t rate)
{
	struct v34_phase3_profile transmitting_profile = profile(role, rate);
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
	bool final_record = false;
	bool e_complete = false;
	bool b1_complete = false;

	CHECK(v34_phase34_receiver_init(
		      &receiver, &transmitting_profile,
		      V34_CONSTELLATION_16_POINT) ==
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
		switch (result) {
		case V34_PHASE34_RECEIVER_OK:
			break;
		case V34_PHASE34_RECEIVER_TIMING_ACQUIRED:
			timing_events++;
			break;
		case V34_PHASE34_RECEIVER_PHASE3_S_DETECTED:
			phase3_s_events++;
			break;
		case V34_PHASE34_RECEIVER_PP_COMPLETE:
			pp_events++;
			break;
		case V34_PHASE34_RECEIVER_J_DETECTED:
			j_events++;
			break;
		case V34_PHASE34_RECEIVER_PHASE4_S_DETECTED:
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
				      v34_phase34_receiver_start_e(&receiver) ==
					      V34_PHASE34_RECEIVER_OK,
				      "role %u rate %u final MP was not accepted",
				      role, rate);
				final_record = true;
			}
			continue;
		case V34_PHASE34_RECEIVER_E_COMPLETE: {
			struct v34_phase34_receiver saved = receiver;
			struct v34_phase34_receiver feedback_receiver = receiver;
			struct v34_frame_geometry wrong =
				source.mapping.schedule.geometry;

			wrong.symbol_rate = V34_SYMBOL_RATE_2800;
			CHECK(v34_phase34_receiver_is_complete(&receiver) &&
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
				      &receiver, &source.mapping.normalizer, &wrong,
				      false,
				      source.parameters.precoder_coefficients,
				      source.parameters.trellis_selection) ==
				      V34_PHASE34_RECEIVER_INVALID_ARGUMENT &&
			      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
			      v34_phase34_receiver_start_data(
				      &feedback_receiver,
				      &source.mapping.normalizer,
				      &source.mapping.schedule.geometry, false,
				      FEEDBACK_COEFFICIENTS,
				      source.parameters.trellis_selection) ==
				      V34_PHASE34_RECEIVER_OK &&
			      feedback_receiver.equalizer.feedback_mode ==
				      V34_ADAPTIVE_EQUALIZER_FEEDBACK_APPLY &&
			      v34_phase34_receiver_is_valid(
				      &feedback_receiver) &&
			      v34_phase34_receiver_start_data(
				      &receiver, &source.mapping.normalizer,
				      &source.mapping.schedule.geometry, false,
				      source.parameters.precoder_coefficients,
				      source.parameters.trellis_selection) ==
				      V34_PHASE34_RECEIVER_OK,
			      "role %u rate %u did not enter data at E", role,
			      rate);
			saved = receiver;
			CHECK(v34_phase34_receiver_start_data(
				      &receiver, &source.mapping.normalizer,
				      &source.mapping.schedule.geometry, false,
				      source.parameters.precoder_coefficients,
				      source.parameters.trellis_selection) ==
				      V34_PHASE34_RECEIVER_INVALID_STATE &&
			      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
			      "role %u rate %u repeated data start changed state",
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
	CHECK(e_complete && b1_complete && mapping_frames >= 3u &&
	      b1_frames ==
		      source.mapping.schedule.geometry
			      .mapping_frames_per_data_frame &&
	      timing_events == 1u &&
	      phase3_s_events == 1u && pp_events == 1u && j_events == 1u &&
	      phase4_s_events ==
		      (role == V34_ANSWERING_STATION ? 1u : 0u) &&
	      trn_events ==
		      (role == V34_ANSWERING_STATION ? 1u : 0u) &&
	      initial_records != 0u && final_record &&
	      source.completed_records >= 3u && source.data_started &&
	      v34_phase34_receiver_is_valid(&receiver) &&
	      receiver.stage == V34_PHASE34_RECEIVER_DATA &&
	      receiver.equalizer.feedback_mode ==
		      V34_ADAPTIVE_EQUALIZER_FEEDBACK_NONE &&
	      !v34_phase34_receiver_is_complete(&receiver) &&
	      !v34_phase34_receiver_has_failed(&receiver),
	      "role %u rate %u receiver did not reach E: timing=%u S3=%u PP=%u "
	      "J=%u S4=%u TRN=%u initial=%u final=%u source=%u "
	      "rejects=%u B1=%u mapped=%u stage=%u failed=%u",
	      role, rate, timing_events, phase3_s_events, pp_events, j_events,
	      phase4_s_events, trn_events, initial_records, final_record,
	      source.completed_records, rejected_records, b1_frames,
	      mapping_frames, receiver.stage,
	      receiver.failed);
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
	for (uint8_t rate = MINIMUM_3000_RATE_2400_BPS_UNITS;
	     rate <= MAXIMUM_3000_RATE_2400_BPS_UNITS; ++rate) {
		if (!test_training_case(V34_CALLING_STATION, rate) ||
		    !test_training_case(V34_ANSWERING_STATION, rate))
			return false;
	}
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "validation", test_validation },
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
