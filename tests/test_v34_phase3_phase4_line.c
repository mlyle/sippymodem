// SPDX-License-Identifier: BSD-3-Clause

#include <v34_caller_training.h>
#include <v34_phase3_line_receiver.h>
#include <v34_phase4_line_handoff.h>

#include "test.h"

#include <math.h>
#include <string.h>

enum {
	SAMPLE_RATE = 8000,
	PHASE3_TRN_SYMBOLS = V34_PHASE3_TRN_MIN_SYMBOLS + 128,
	PHASE3_J_SYMBOLS = V34_PHASE3_J_PATTERN_SYMBOLS * 5,
	PHASE4_TRN_SYMBOLS = V34_PHASE4_TRN_MIN_SYMBOLS + 384,
	SAMPLE_LIMIT = 40000,
};

struct line_source {
	struct v34_caller_training training;
	struct v34_mp_parameters mp;
	unsigned int completed_records;
};

static struct v34_phase3_profile profile(void)
{
	return (struct v34_phase3_profile){
		.symbol_rate_numerator = 3000,
		.symbol_rate_denominator = 1,
		.carrier_frequency_numerator = 2000,
		.carrier_frequency_denominator = 1,
		.symbol_rate = V34_SYMBOL_RATE_3000,
		.local_role = V34_CALLING_STATION,
		.preemphasis_filter_index = 3,
		.projected_data_rate_2400_bps_units = 8,
		.high_carrier = true,
	};
}

static struct v34_mp_parameters mp_parameters(void)
{
	return (struct v34_mp_parameters){
		.maximum_call_to_answer_rate_2400_bps_units = 8,
		.maximum_answer_to_call_rate_2400_bps_units = 8,
		.remote_transmitter_trellis = V34_TRELLIS_16_STATE,
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
		.mp = mp_parameters(),
	};

	if (v34_caller_training_init(
		    &initialized.training, transmitting_profile,
		    V34_CONSTELLATION_16_POINT, SAMPLE_RATE, 5000) !=
	    V34_CALLER_TRAINING_OK)
		return false;
	*source = initialized;
	return true;
}

static bool prepare_source(struct line_source *source)
{
	struct v34_caller_training *training = &source->training;

	if (training->stage == V34_CALLER_TRAINING_PHASE3 &&
	    training->phase3.stage == V34_PHASE3_CALLER_TRN &&
	    training->phase3.trn_symbols == PHASE3_TRN_SYMBOLS)
		return v34_caller_training_start_j(training) ==
		       V34_CALLER_TRAINING_OK;
	if (training->stage == V34_CALLER_TRAINING_PHASE3 &&
	    training->phase3.stage == V34_PHASE3_CALLER_J &&
	    training->phase3.j_symbols == PHASE3_J_SYMBOLS)
		return v34_caller_training_finish_j(training) ==
		       V34_CALLER_TRAINING_OK;
	if (training->stage == V34_CALLER_TRAINING_PHASE4_TRN &&
	    training->phase4_trn_symbols == PHASE4_TRN_SYMBOLS)
		return v34_caller_training_start_mp_type0(
			       training, &source->mp) ==
		       V34_CALLER_TRAINING_OK;
	if (v34_caller_training_phase4_mp_record_complete(training)) {
		source->completed_records++;
		return v34_caller_training_start_mp_type0(
			       training, &source->mp) ==
		       V34_CALLER_TRAINING_OK;
	}
	return true;
}

static bool source_sample(struct line_source *source, int16_t *sample)
{
	return prepare_source(source) &&
	       v34_caller_training_sample(&source->training, sample) ==
		       V34_CALLER_TRAINING_OK;
}

static struct v34_phase3_line_receiver_configuration
phase3_configuration(void)
{
	return (struct v34_phase3_line_receiver_configuration){
		.s_gain_adaptation = 0.1,
		.pp_equalizer_adaptation = 0.2,
		.tracking_equalizer_adaptation = 0.05,
	};
}

static bool initialize_phase4(
	const struct v34_phase3_line_receiver *phase3,
	struct v34_phase4_line_receiver *phase4)
{
	struct v34_phase3_line_handoff handoff;
	struct v34_phase3_line_handoff corrupt;
	struct v34_phase4_line_receiver unchanged;

	memset(phase4, 0xa5, sizeof(*phase4));
	unchanged = *phase4;
	if (v34_phase3_line_receiver_handoff(phase3, &handoff) !=
	    V34_PHASE3_LINE_RECEIVER_OK)
		return false;
	corrupt = handoff;
	corrupt.demodulator.timing_locked = false;
	if (v34_phase4_line_receiver_init_from_phase3(
		    NULL, &handoff, 0.03) !=
		    V34_PHASE4_LINE_RECEIVER_INVALID_ARGUMENT ||
	    v34_phase4_line_receiver_init_from_phase3(
		    phase4, NULL, 0.03) !=
		    V34_PHASE4_LINE_RECEIVER_INVALID_ARGUMENT ||
	    memcmp(phase4, &unchanged, sizeof(*phase4)) != 0 ||
	    v34_phase4_line_receiver_init_from_phase3(
		    phase4, &corrupt, 0.03) !=
		    V34_PHASE4_LINE_RECEIVER_INVALID_ARGUMENT ||
	    memcmp(phase4, &unchanged, sizeof(*phase4)) != 0 ||
	    v34_phase4_line_receiver_init_from_phase3(
		    phase4, &handoff, NAN) !=
		    V34_PHASE4_LINE_RECEIVER_INVALID_ARGUMENT ||
	    memcmp(phase4, &unchanged, sizeof(*phase4)) != 0 ||
	    v34_phase4_line_receiver_init_from_phase3(
		    phase4, &handoff, 0.03) !=
		    V34_PHASE4_LINE_RECEIVER_OK)
		return false;
	return phase4->demodulator.carrier_phase ==
		       handoff.demodulator.carrier_phase &&
	       phase4->demodulator.locked_phase_units ==
		       handoff.demodulator.locked_phase_units &&
	       memcmp(&phase4->control.equalizer,
		      &handoff.phase3.j.equalizer.equalizer,
		      sizeof(phase4->control.equalizer)) == 0 &&
	       phase4->control.mp_receiver.constellation ==
		       handoff.phase3.j.detection.sequence.remote_constellation;
}

static bool test_continuous_training_line(void)
{
	struct v34_phase3_profile transmitting_profile = profile();
	struct v34_phase3_line_receiver_configuration config =
		phase3_configuration();
	struct v34_phase3_line_receiver phase3;
	struct v34_phase4_line_receiver phase4;
	struct line_source source;
	unsigned int timing_events = 0;
	unsigned int s_events = 0;
	unsigned int pp_events = 0;
	unsigned int j_events = 0;
	unsigned int rejected_records = 0;
	bool receiving_phase4 = false;
	bool mp_received = false;

	CHECK(v34_phase3_line_receiver_init(
		      &phase3, &transmitting_profile, SAMPLE_RATE,
		      &config) == V34_PHASE3_LINE_RECEIVER_OK &&
	      source_init(&source, &transmitting_profile),
	      "continuous Phase 3/4 line did not initialize");
	for (unsigned int index = 0;
	     index < SAMPLE_LIMIT && !mp_received; ++index) {
		int16_t sample;

		CHECK(source_sample(&source, &sample),
		      "continuous source stopped at sample %u", index);
		if (!receiving_phase4) {
			enum v34_phase3_line_receiver_result result =
				v34_phase3_line_receiver_sample(&phase3, sample);

			switch (result) {
			case V34_PHASE3_LINE_RECEIVER_OK:
				break;
			case V34_PHASE3_LINE_RECEIVER_TIMING_ACQUIRED:
				timing_events++;
				break;
			case V34_PHASE3_LINE_RECEIVER_S_DETECTED:
				s_events++;
				break;
			case V34_PHASE3_LINE_RECEIVER_PP_COMPLETE:
				pp_events++;
				break;
			case V34_PHASE3_LINE_RECEIVER_J_DETECTED:
				j_events++;
				CHECK(initialize_phase4(&phase3, &phase4),
				      "Phase 3 handoff did not initialize Phase 4");
				receiving_phase4 = true;
				break;
			default:
				CHECK(false, "Phase 3 failed at sample %u: %s",
				      index,
				      v34_phase3_line_receiver_result_name(result));
			}
			continue;
		}
		{
			struct v34_mp_receive_event event = { 0 };
			enum v34_phase4_line_receiver_result result =
				v34_phase4_line_receiver_sample(
					&phase4, sample, &event);

			CHECK(result != V34_PHASE4_LINE_RECEIVER_FAILED &&
			      result != V34_PHASE4_LINE_RECEIVER_INVALID_ARGUMENT &&
			      result != V34_PHASE4_LINE_RECEIVER_INVALID_STATE &&
			      result != V34_PHASE4_LINE_RECEIVER_NUMERIC_FAILURE &&
			      result !=
				      V34_PHASE4_LINE_RECEIVER_SEQUENCE_MISMATCH,
			      "Phase 4 failed at sample %u: %s", index,
			      v34_phase4_line_receiver_result_name(result));
			if (result != V34_PHASE4_LINE_RECEIVER_RECORD_READY)
				continue;
			if (event.type == V34_MP_RECEIVE_REJECTED) {
				rejected_records++;
				CHECK(v34_phase4_line_receiver_continue_mp(
					      &phase4) ==
					      V34_PHASE4_LINE_RECEIVER_OK,
				      "rejected Phase 4 record did not resume");
				continue;
			}
			CHECK(event.type == V34_MP_RECEIVE_TYPE0 &&
			      event.frame_result == V34_MP_OK &&
			      parameters_match(
				      &event.parameters.type0, &source.mp),
			      "Phase 4 did not recover the generated MP record");
			mp_received = true;
		}
	}
	CHECK(mp_received && receiving_phase4 && timing_events == 1 &&
	      s_events == 1 && pp_events == 1 && j_events == 1 &&
	      source.completed_records != 0 &&
	      v34_phase4_line_receiver_is_valid(&phase4) &&
	      !v34_phase4_line_receiver_has_failed(&phase4),
	      "continuous Phase 3/4 timeline did not reach MP (%u rejects)",
	      rejected_records);
	return true;
}

int main(void)
{
	fprintf(stderr, "[ RUN      ] continuous training line\n");
	if (!test_continuous_training_line()) {
		fprintf(stderr, "[  FAILED  ] continuous training line\n");
		return 1;
	}
	fprintf(stderr, "[       OK ] continuous training line\n");
	return 0;
}
