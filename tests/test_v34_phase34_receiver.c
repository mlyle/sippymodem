// SPDX-License-Identifier: BSD-3-Clause

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
};

struct line_source {
	struct v34_training_transmitter training;
	struct v34_mp_parameters initial_mp;
	struct v34_mp_parameters final_mp;
	unsigned int completed_records;
};

static struct v34_phase3_profile profile(enum v34_station_role role)
{
	return (struct v34_phase3_profile){
		.symbol_rate_numerator = 3000,
		.symbol_rate_denominator = 1,
		.carrier_frequency_numerator = 2000,
		.carrier_frequency_denominator = 1,
		.symbol_rate = V34_SYMBOL_RATE_3000,
		.local_role = role,
		.preemphasis_filter_index = 3,
		.projected_data_rate_2400_bps_units = 8,
		.high_carrier = true,
	};
}

static struct v34_mp_parameters mp_parameters(bool acknowledges_peer)
{
	return (struct v34_mp_parameters){
		.maximum_call_to_answer_rate_2400_bps_units = 8,
		.maximum_answer_to_call_rate_2400_bps_units = 8,
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

static bool source_init(
	struct line_source *source,
	const struct v34_phase3_profile *transmitting_profile)
{
	struct line_source initialized = {
		.initial_mp = mp_parameters(false),
		.final_mp = mp_parameters(true),
	};

	if (v34_training_transmitter_init(
		    &initialized.training, transmitting_profile,
		    V34_CONSTELLATION_16_POINT, SAMPLE_RATE, 5000) !=
	    V34_TRAINING_TRANSMITTER_OK)
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
		struct qam_point b1 = { 0.25, -0.25 };
		const struct qam_point *point =
			v34_training_line_needs_point(&source->training.line) ?
				&b1 :
				NULL;
		bool accepted;

		return v34_training_line_sample(
			       &source->training.line, point, sample, &accepted) ==
			       V34_TRAINING_LINE_OK &&
		       accepted == (point != NULL);
	}
	return prepare_source(source) &&
	       v34_training_transmitter_sample(&source->training, sample) ==
		       V34_TRAINING_TRANSMITTER_OK;
}

static bool test_validation(void)
{
	struct v34_phase3_profile valid = profile(V34_CALLING_STATION);
	struct v34_phase3_profile invalid = valid;
	struct v34_phase34_receiver receiver;
	struct v34_phase34_receiver saved;
	struct v34_phase3_decision_handoff handoff;
	struct v34_phase3_decision_handoff saved_handoff;
	struct v34_mp_receive_event event;
	struct v34_mp_receive_event saved_event;

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
	memset(&event, 0x6b, sizeof(event));
	saved_event = event;
	for (unsigned int sample = 0; sample < 37u; ++sample)
		CHECK(v34_phase34_receiver_sample(&receiver, 0, &event) ==
			      V34_PHASE34_RECEIVER_OK,
		      "leading silence failed at sample %u", sample);
	CHECK(memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
	      memcmp(&event, &saved_event, sizeof(event)) == 0,
	      "leading silence changed receiver state or output");
	memset(&handoff, 0x7c, sizeof(handoff));
	saved_handoff = handoff;
	CHECK(v34_phase34_receiver_sample(&receiver, 1, NULL) ==
		      V34_PHASE34_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase34_receiver_sample(
		      &receiver, 1,
		      (struct v34_mp_receive_event *)&receiver) ==
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

static bool test_training_case(enum v34_station_role role)
{
	struct v34_phase3_profile transmitting_profile = profile(role);
	struct v34_phase34_receiver receiver;
	struct v34_phase34_receiver saved;
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
	bool final_record = false;
	bool e_complete = false;

	CHECK(v34_phase34_receiver_init(
		      &receiver, &transmitting_profile,
		      V34_CONSTELLATION_16_POINT) ==
		      V34_PHASE34_RECEIVER_OK &&
	      source_init(&source, &transmitting_profile),
	      "role %u training pair did not initialize", role);
	for (unsigned int index = 0;
	     index < SAMPLE_LIMIT && !e_complete; ++index) {
		struct v34_mp_receive_event event;
		struct v34_mp_receive_event saved_event;
		enum v34_phase34_receiver_result result;
		int16_t sample;

		CHECK(source_sample(&source, &sample),
		      "role %u source stopped at sample %u", role, index);
		memset(&event, 0x5d, sizeof(event));
		saved_event = event;
		result = v34_phase34_receiver_sample(&receiver, sample, &event);
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
			if (event.type == V34_MP_RECEIVE_REJECTED) {
				rejected_records++;
				CHECK(v34_phase34_receiver_continue_mp(&receiver) ==
					      V34_PHASE34_RECEIVER_OK,
				      "role %u rejected MP did not resume", role);
			} else if (event.type == V34_MP_RECEIVE_TYPE0 &&
				   event.frame_result == V34_MP_OK &&
				   parameters_match(
					   &event.parameters.type0,
					   &source.initial_mp)) {
				initial_records++;
				CHECK(v34_phase34_receiver_continue_mp(&receiver) ==
					      V34_PHASE34_RECEIVER_OK,
				      "role %u initial MP did not resume", role);
			} else {
				CHECK(event.type == V34_MP_RECEIVE_TYPE0 &&
				      event.frame_result == V34_MP_OK &&
				      !final_record &&
				      parameters_match(
					      &event.parameters.type0,
					      &source.final_mp) &&
				      v34_phase34_receiver_start_e(&receiver) ==
					      V34_PHASE34_RECEIVER_OK,
				      "role %u final MP was not accepted", role);
				final_record = true;
			}
			continue;
		case V34_PHASE34_RECEIVER_E_COMPLETE:
			e_complete = true;
			break;
		default:
			CHECK(false,
			      "role %u training failed at sample %u, result %u",
			      role, index, result);
		}
		CHECK(memcmp(&event, &saved_event, sizeof(event)) == 0,
		      "role %u non-record result changed its event", role);
	}
	CHECK(e_complete && timing_events == 1u &&
	      phase3_s_events == 1u && pp_events == 1u && j_events == 1u &&
	      phase4_s_events ==
		      (role == V34_ANSWERING_STATION ? 1u : 0u) &&
	      trn_events ==
		      (role == V34_ANSWERING_STATION ? 1u : 0u) &&
	      initial_records != 0u && final_record &&
	      source.completed_records >= 3u &&
	      v34_phase34_receiver_is_valid(&receiver) &&
	      v34_phase34_receiver_is_complete(&receiver) &&
	      !v34_phase34_receiver_has_failed(&receiver),
	      "role %u receiver did not reach E: timing=%u S3=%u PP=%u "
	      "J=%u S4=%u TRN=%u initial=%u final=%u source=%u "
	      "rejects=%u stage=%u semantic=%u failed=%u",
	      role, timing_events, phase3_s_events, pp_events, j_events,
	      phase4_s_events, trn_events, initial_records, final_record,
	      source.completed_records, rejected_records, receiver.stage,
	      receiver.stage == V34_PHASE34_RECEIVER_PHASE3 ?
		      receiver.semantic.phase3.stage :
		      receiver.semantic.phase4.stage,
	      receiver.failed);
	CHECK(v34_phase34_receiver_phase3_handoff(&receiver, &handoff) ==
		      V34_PHASE34_RECEIVER_OK &&
	      v34_phase3_decision_handoff_is_valid(&handoff) &&
	      receiver.tracking_started && receiver.tracking_symbols != 0u &&
	      receiver.equalizer.epoch != 0u &&
	      !receiver.equalizer.decision_pending,
	      "role %u receiver did not retain its physical lineage", role);
	saved = receiver;
	{
		struct v34_mp_receive_event event;
		struct v34_mp_receive_event saved_event;

		memset(&event, 0x4c, sizeof(event));
		saved_event = event;
		CHECK(v34_phase34_receiver_sample(&receiver, 0, &event) ==
			      V34_PHASE34_RECEIVER_COMPLETE &&
		      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
		      memcmp(&event, &saved_event, sizeof(event)) == 0,
		      "role %u complete receiver consumed another sample", role);
	}
	return true;
}

static bool test_training(void)
{
	return test_training_case(V34_CALLING_STATION) &&
	       test_training_case(V34_ANSWERING_STATION);
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "validation", test_validation },
		{ "continuous Phase 3 and Phase 4", test_training },
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
