// SPDX-License-Identifier: BSD-3-Clause

#include <v34_phase3_line_receiver.h>
#include <v34_phase3_line_transmitter.h>

#include "test.h"

#include <math.h>
#include <string.h>

enum {
	SAMPLE_RATE = 8000,
	TRN_SYMBOLS = 640,
	SAMPLE_LIMIT = 30000,
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
		.preemphasis_filter_index = 0,
		.projected_data_rate_2400_bps_units = 8,
		.high_carrier = true,
	};
}

static struct v34_phase3_line_receiver_configuration configuration(void)
{
	return (struct v34_phase3_line_receiver_configuration){
		.s_gain_adaptation = 0.1,
		.pp_equalizer_adaptation = 0.2,
		.tracking_equalizer_adaptation = 0.05,
	};
}

static bool source_init(
	struct v34_phase3_line_transmitter *source,
	const struct v34_phase3_profile *transmitting_profile)
{
	return v34_phase3_line_transmitter_init(
		       source, transmitting_profile,
		       V34_CONSTELLATION_16_POINT, SAMPLE_RATE, 6000) ==
	       V34_PHASE3_LINE_TRANSMITTER_OK;
}

static bool source_sample(
	struct v34_phase3_line_transmitter *source, int16_t *sample)
{
	if (source->symbols.stage == V34_PHASE3_TRANSMITTER_TRN &&
	    source->symbols.trn_symbols == TRN_SYMBOLS &&
	    v34_phase3_line_transmitter_start_j(source) !=
		    V34_PHASE3_LINE_TRANSMITTER_OK)
		return false;
	return v34_phase3_line_transmitter_sample(source, sample) ==
	       V34_PHASE3_LINE_TRANSMITTER_OK;
}

static bool run_generated_line_timeline(enum v34_station_role role)
{
	struct v34_phase3_profile transmitting_profile = profile(role);
	struct v34_phase3_line_receiver_configuration config =
		configuration();
	struct v34_phase3_line_receiver receiver;
	struct v34_phase3_line_receiver complete;
	struct v34_phase3_receiver completed_points;
	struct v34_phase3_line_handoff handoff;
	struct v34_phase3_line_handoff unchanged;
	struct v34_phase3_line_transmitter source;
	unsigned int timing_events = 0;
	unsigned int s_events = 0;
	unsigned int pp_events = 0;
	unsigned int j_events = 0;
	uint32_t carrier_phase_before_tracking;
	bool complete_seen = false;

	memset(&handoff, 0x5a, sizeof(handoff));
	unchanged = handoff;
	CHECK(v34_phase3_line_receiver_init(
		      &receiver, &transmitting_profile, SAMPLE_RATE,
		      &config) == V34_PHASE3_LINE_RECEIVER_OK &&
	      source_init(&source, &transmitting_profile) &&
	      v34_phase3_line_receiver_handoff(&receiver, &handoff) ==
		      V34_PHASE3_LINE_RECEIVER_INVALID_STATE &&
	      memcmp(&handoff, &unchanged, sizeof(handoff)) == 0,
	      "Phase 3 line timeline did not initialize transactionally");
	for (unsigned int silence = 0; silence < 37; ++silence)
		CHECK(v34_phase3_line_receiver_sample(&receiver, 0) ==
			      V34_PHASE3_LINE_RECEIVER_OK,
		      "leading silence changed state at %u", silence);
	for (unsigned int index = 0; index < SAMPLE_LIMIT; ++index) {
		int16_t sample;
		enum v34_phase3_line_receiver_result result;

		CHECK(source_sample(&source, &sample),
		      "Phase 3 line source stopped at sample %u", index);
		result = v34_phase3_line_receiver_sample(&receiver, sample);
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
			complete_seen = true;
			break;
		default:
			CHECK(false, "Phase 3 line failed at sample %u: %s",
			      index, v34_phase3_line_receiver_result_name(result));
		}
		if (complete_seen)
			break;
	}
	CHECK(complete_seen && timing_events == 1 && s_events == 1 &&
	      pp_events == 1 && j_events == 1 &&
	      v34_phase3_line_receiver_is_valid(&receiver) &&
	      !v34_phase3_line_receiver_has_failed(&receiver) &&
	      v34_phase3_receiver_is_complete(&receiver.points) &&
	      v34_phase3_line_receiver_handoff(&receiver, &handoff) ==
		      V34_PHASE3_LINE_RECEIVER_OK &&
	      v34_phase3_line_handoff_is_valid(&handoff) &&
	      memcmp(&handoff.line, &receiver.line,
		     sizeof(handoff.line)) == 0 &&
	      memcmp(&handoff.transmitting_profile,
		     &transmitting_profile,
		     sizeof(transmitting_profile)) == 0,
	      "Phase 3 line did not preserve its continuous handoff");
	completed_points = receiver.points;
	carrier_phase_before_tracking = receiver.line.state.qam.carrier_phase;
	for (unsigned int index = 0; index < 31; ++index) {
		int16_t sample;

		CHECK(source_sample(&source, &sample) &&
		      v34_phase3_line_receiver_track_sample(
			      &receiver, sample) == V34_PHASE3_LINE_RECEIVER_OK,
		      "completed Phase 3 line did not track sample %u", index);
	}
	CHECK(memcmp(&receiver.points, &completed_points,
		     sizeof(completed_points)) == 0 &&
	      receiver.line.state.qam.carrier_phase !=
		      carrier_phase_before_tracking &&
	      v34_phase3_line_receiver_handoff(&receiver, &handoff) ==
		      V34_PHASE3_LINE_RECEIVER_OK &&
	      memcmp(&handoff.line, &receiver.line,
		     sizeof(handoff.line)) == 0,
	      "post-J line tracking did not preserve its handoff boundary");
	complete = receiver;
	CHECK(v34_phase3_line_receiver_sample(&receiver, 0) ==
		      V34_PHASE3_LINE_RECEIVER_COMPLETE &&
	      memcmp(&receiver, &complete, sizeof(receiver)) == 0,
	      "completed Phase 3 line consumed another sample");
	return true;
}

static bool test_generated_line_timeline(void)
{
	return run_generated_line_timeline(V34_CALLING_STATION) &&
	       run_generated_line_timeline(V34_ANSWERING_STATION);
}

static bool run_transmitter_handoff(enum v34_station_role role)
{
	struct v34_phase3_profile transmitting_profile = profile(role);
	struct v34_phase3_line_transmitter transmitter;
	struct v34_phase3_line_transmitter saved;
	struct v34_training_line handoff;
	struct v34_training_line saved_handoff;
	int16_t sample = 1234;
	bool started_j = false;
	bool finished_j = false;
	bool complete = false;

	memset(&handoff, 0x5a, sizeof(handoff));
	saved_handoff = handoff;
	CHECK(v34_phase3_line_transmitter_init(
		      &transmitter, &transmitting_profile,
		      V34_CONSTELLATION_16_POINT, SAMPLE_RATE, 6000) ==
		      V34_PHASE3_LINE_TRANSMITTER_OK &&
	      v34_phase3_line_transmitter_handoff(
		      &transmitter, &handoff) ==
		      V34_PHASE3_LINE_TRANSMITTER_INVALID_STATE &&
	      memcmp(&handoff, &saved_handoff, sizeof(handoff)) == 0,
	      "Phase 3 transmitter exposed a premature role=%d handoff", role);
	for (unsigned int index = 0; index < SAMPLE_LIMIT; ++index) {
		enum v34_phase3_line_transmitter_result result;

		if (transmitter.symbols.stage ==
			    V34_PHASE3_TRANSMITTER_TRN &&
		    transmitter.symbols.trn_symbols ==
			    V34_PHASE3_TRN_MIN_SYMBOLS) {
			CHECK(!started_j &&
			      v34_phase3_line_transmitter_start_j(
				      &transmitter) ==
				      V34_PHASE3_LINE_TRANSMITTER_OK,
			      "role=%d did not enter J", role);
			started_j = true;
		}
		if (transmitter.symbols.stage ==
			    V34_PHASE3_TRANSMITTER_J &&
		    transmitter.symbols.j_symbols ==
			    V34_PHASE3_J_PATTERN_SYMBOLS) {
			CHECK(!finished_j &&
			      v34_phase3_line_transmitter_finish_j(
				      &transmitter) ==
				      V34_PHASE3_LINE_TRANSMITTER_OK,
			      "role=%d did not finish J", role);
			finished_j = true;
		}
		result = v34_phase3_line_transmitter_sample(
			&transmitter, &sample);
		if (result == V34_PHASE3_LINE_TRANSMITTER_COMPLETE) {
			complete = true;
			break;
		}
		CHECK(result == V34_PHASE3_LINE_TRANSMITTER_OK,
		      "role=%d line stopped at sample %u: %s", role, index,
		      v34_phase3_line_transmitter_result_name(result));
	}
	saved = transmitter;
	sample = 1234;
	CHECK(started_j && finished_j && complete &&
	      v34_phase3_line_transmitter_is_handoff_ready(&transmitter) &&
	      v34_phase3_line_transmitter_sample(&transmitter, &sample) ==
		      V34_PHASE3_LINE_TRANSMITTER_COMPLETE &&
	      sample == 1234 &&
	      memcmp(&transmitter, &saved, sizeof(transmitter)) == 0 &&
	      v34_phase3_line_transmitter_handoff(
		      &transmitter, &handoff) ==
		      V34_PHASE3_LINE_TRANSMITTER_OK &&
	      v34_training_line_is_valid(&handoff) &&
	      memcmp(&handoff, &transmitter.line, sizeof(handoff)) == 0,
	      "role=%d Phase 3 line handoff was not boundary-exact", role);
	return true;
}

static bool test_transmitter_handoff(void)
{
	return run_transmitter_handoff(V34_CALLING_STATION) &&
	       run_transmitter_handoff(V34_ANSWERING_STATION);
}

static bool test_transmitter_validation(void)
{
	struct v34_phase3_profile transmitting_profile =
		profile(V34_CALLING_STATION);
	struct v34_phase3_profile with_md = transmitting_profile;
	struct v34_phase3_line_transmitter transmitter;
	struct v34_phase3_line_transmitter saved;
	int16_t sample = 1234;

	with_md.md_length_35_ms_units = 1;
	with_md.md_symbol_count = 105;
	memset(&transmitter, 0xa5, sizeof(transmitter));
	saved = transmitter;
	CHECK(v34_phase3_profile_is_valid(&with_md) &&
	      v34_phase3_line_transmitter_init(
		      NULL, &transmitting_profile,
		      V34_CONSTELLATION_4_POINT, SAMPLE_RATE, 6000) ==
		      V34_PHASE3_LINE_TRANSMITTER_INVALID_ARGUMENT &&
	      v34_phase3_line_transmitter_init(
		      &transmitter, &with_md, V34_CONSTELLATION_4_POINT,
		      SAMPLE_RATE, 6000) ==
		      V34_PHASE3_LINE_TRANSMITTER_INVALID_ARGUMENT &&
	      v34_phase3_line_transmitter_init(
		      &transmitter, &transmitting_profile,
		      (enum v34_constellation_size)99, SAMPLE_RATE, 6000) ==
		      V34_PHASE3_LINE_TRANSMITTER_INVALID_ARGUMENT &&
	      v34_phase3_line_transmitter_init(
		      &transmitter, &transmitting_profile,
		      V34_CONSTELLATION_4_POINT, 1000, 6000) ==
		      V34_PHASE3_LINE_TRANSMITTER_OUT_OF_RANGE &&
	      v34_phase3_line_transmitter_init(
		      &transmitter, &transmitting_profile,
		      V34_CONSTELLATION_4_POINT, SAMPLE_RATE, INT16_MAX) ==
		      V34_PHASE3_LINE_TRANSMITTER_CLIPPING_RISK &&
	      memcmp(&transmitter, &saved, sizeof(transmitter)) == 0,
	      "invalid Phase 3 transmitter initialization changed its owner");
	CHECK(v34_phase3_line_transmitter_init(
		      &transmitter, &transmitting_profile,
		      V34_CONSTELLATION_4_POINT, SAMPLE_RATE, 6000) ==
		      V34_PHASE3_LINE_TRANSMITTER_OK,
	      "Phase 3 transmitter validation fixture did not initialize");
	saved = transmitter;
	CHECK(v34_phase3_line_transmitter_start_j(&transmitter) ==
		      V34_PHASE3_LINE_TRANSMITTER_INVALID_STATE &&
	      v34_phase3_line_transmitter_finish_j(&transmitter) ==
		      V34_PHASE3_LINE_TRANSMITTER_INVALID_STATE &&
	      v34_phase3_line_transmitter_sample(&transmitter, NULL) ==
		      V34_PHASE3_LINE_TRANSMITTER_INVALID_ARGUMENT &&
	      memcmp(&transmitter, &saved, sizeof(transmitter)) == 0 &&
	      sample == 1234 &&
	      !v34_phase3_line_transmitter_is_valid(NULL) &&
	      !v34_phase3_line_transmitter_is_handoff_ready(NULL) &&
	      strcmp(v34_phase3_line_transmitter_result_name(
		     V34_PHASE3_LINE_TRANSMITTER_NUMERIC_FAILURE),
		     "numeric failure") == 0 &&
	      strcmp(v34_phase3_line_transmitter_result_name(
		     (enum v34_phase3_line_transmitter_result)99),
		     "unknown") == 0,
	      "Phase 3 transmitter validation was not transactional");
	return true;
}

static bool test_validation(void)
{
	struct v34_phase3_profile transmitting_profile =
		profile(V34_CALLING_STATION);
	struct v34_phase3_line_receiver_configuration config =
		configuration();
	struct v34_phase3_line_receiver receiver;
	struct v34_phase3_line_receiver saved;
	struct v34_phase3_line_receiver corrupt;
	struct v34_line_frontend fixed;
	struct v34_line_frontend saved_fixed;
	struct qam_channel channel;
	struct qam_channel saved_channel;
	struct qam_point point = { 91.0, -37.0 };
	struct qam_point saved_point = point;
	bool ready = true;

	memset(&receiver, 0xa5, sizeof(receiver));
	saved = receiver;
	config.s_gain_adaptation = NAN;
	CHECK(!v34_phase3_line_receiver_configuration_is_valid(&config) &&
	      v34_phase3_line_receiver_init(
		      &receiver, &transmitting_profile, SAMPLE_RATE,
		      &config) == V34_PHASE3_LINE_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
	      "invalid Phase 3 line configuration changed its destination");
	config = configuration();
	memset(&channel, 0x3c, sizeof(channel));
	saved_channel = channel;
	CHECK(v34_training_line_channel(
		      &transmitting_profile, 1000, &channel) ==
		      V34_TRAINING_LINE_OUT_OF_RANGE &&
	      memcmp(&channel, &saved_channel, sizeof(channel)) == 0 &&
	      v34_phase3_line_receiver_init(
		      &receiver, &transmitting_profile, 1000,
		      &config) == V34_PHASE3_LINE_RECEIVER_OUT_OF_RANGE &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0,
	      "out-of-band Phase 3 channel was not rejected transactionally");
	CHECK(v34_phase3_line_receiver_init(
		      &receiver, &transmitting_profile, SAMPLE_RATE,
		      &config) == V34_PHASE3_LINE_RECEIVER_OK,
	      "valid Phase 3 line validation fixture did not initialize");
	saved = receiver;
	corrupt = receiver;
	corrupt.line.state.qam.acquisition_window_symbols++;
	CHECK(!v34_phase3_line_receiver_is_valid(NULL) &&
	      !v34_phase3_line_receiver_is_valid(&corrupt) &&
	      !v34_phase3_line_receiver_has_failed(&corrupt) &&
	      !v34_phase3_line_handoff_is_valid(NULL) &&
	      v34_phase3_line_receiver_sample(NULL, 0) ==
		      V34_PHASE3_LINE_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase3_line_receiver_track_sample(NULL, 0) ==
		      V34_PHASE3_LINE_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase3_line_receiver_track_sample(&receiver, 0) ==
		      V34_PHASE3_LINE_RECEIVER_INVALID_STATE &&
	      memcmp(&receiver, &saved, sizeof(receiver)) == 0 &&
	      strcmp(v34_phase3_line_receiver_result_name(
		     V34_PHASE3_LINE_RECEIVER_TIMING_ACQUIRED),
		     "timing acquired") == 0 &&
	      strcmp(v34_phase3_line_receiver_result_name(
		     (enum v34_phase3_line_receiver_result)99),
		     "unknown") == 0,
	      "Phase 3 line validation accepted corrupt state");
	memset(&fixed, 0xa5, sizeof(fixed));
	saved_fixed = fixed;
	CHECK(v34_line_frontend_init_fixed(NULL, &transmitting_profile) ==
		      V34_LINE_FRONTEND_INVALID_ARGUMENT &&
	      v34_line_frontend_init_fixed(&fixed, NULL) ==
		      V34_LINE_FRONTEND_INVALID_ARGUMENT &&
	      memcmp(&fixed, &saved_fixed, sizeof(fixed)) == 0 &&
	      v34_line_frontend_init_fixed(&fixed, &transmitting_profile) ==
		      V34_LINE_FRONTEND_OK &&
	      fixed.mode == V34_LINE_FRONTEND_FIXED &&
	      !fixed.fixed_signal_started &&
	      !v34_line_frontend_timing_acquired(&fixed),
	      "fixed Phase 3 frontend did not initialize transactionally");
	saved_fixed = fixed;
	CHECK(v34_line_frontend_sample(&fixed, 0, &point, &ready) ==
		      V34_LINE_FRONTEND_OK &&
	      !ready && memcmp(&point, &saved_point, sizeof(point)) == 0 &&
	      memcmp(&fixed, &saved_fixed, sizeof(fixed)) == 0 &&
	      v34_line_frontend_sample(
		      &fixed, 32, (struct qam_point *)&fixed, &ready) ==
		      V34_LINE_FRONTEND_INVALID_ARGUMENT &&
	      memcmp(&fixed, &saved_fixed, sizeof(fixed)) == 0,
	      "fixed Phase 3 silence or alias handling changed state");
	ready = true;
	CHECK(v34_line_frontend_sample(&fixed, 32, &point, &ready) ==
		      V34_LINE_FRONTEND_OK &&
	      !ready && fixed.fixed_signal_started &&
	      fixed.state.fixed.carrier.phase ==
		      fixed.state.fixed.carrier.phase_step &&
	      memcmp(&point, &saved_point, sizeof(point)) == 0,
	      "fixed Phase 3 frontend did not start at qualified input");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated line timeline", test_generated_line_timeline },
		{ "transmitter handoff", test_transmitter_handoff },
		{ "transmitter validation", test_transmitter_validation },
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
