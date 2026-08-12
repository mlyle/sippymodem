// SPDX-License-Identifier: BSD-3-Clause

#include <v34_phase3_caller.h>
#include <v34_phase3_line_receiver.h>
#include <v34_training_line.h>

#include "test.h"

#include <math.h>
#include <string.h>

enum {
	SAMPLE_RATE = 8000,
	TRN_SYMBOLS = 640,
	SAMPLE_LIMIT = 30000,
};

struct line_source {
	struct v34_training_line line;
	struct v34_phase3_caller symbols;
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
	struct line_source *source,
	const struct v34_phase3_profile *transmitting_profile)
{
	return v34_training_line_init(
		       &source->line, transmitting_profile,
		       SAMPLE_RATE, 6000) == V34_TRAINING_LINE_OK &&
	       v34_phase3_caller_init(
		       &source->symbols, V34_CONSTELLATION_16_POINT) ==
		       V34_PHASE3_CALLER_OK;
}

static bool source_sample(struct line_source *source, int16_t *sample)
{
	struct qam_point point;
	const struct qam_point *point_pointer = NULL;
	bool accepting = v34_training_line_needs_point(&source->line);
	bool accepted;

	if (accepting) {
		if (source->symbols.stage == V34_PHASE3_CALLER_TRN &&
		    source->symbols.trn_symbols == TRN_SYMBOLS &&
		    v34_phase3_caller_start_j(&source->symbols) !=
			    V34_PHASE3_CALLER_OK)
			return false;
		if (v34_phase3_caller_next(&source->symbols, &point) !=
		    V34_PHASE3_CALLER_OK)
			return false;
		point_pointer = &point;
	}
	return v34_training_line_sample(
		       &source->line, point_pointer, sample, &accepted) ==
		       V34_TRAINING_LINE_OK &&
	       accepted == accepting;
}

static bool test_generated_line_timeline(void)
{
	struct v34_phase3_profile transmitting_profile = profile();
	struct v34_phase3_line_receiver_configuration config =
		configuration();
	struct v34_phase3_line_receiver receiver;
	struct v34_phase3_line_receiver complete;
	struct v34_phase3_line_handoff handoff;
	struct v34_phase3_line_handoff unchanged;
	struct line_source source;
	unsigned int timing_events = 0;
	unsigned int s_events = 0;
	unsigned int pp_events = 0;
	unsigned int j_events = 0;
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
	      memcmp(&handoff.demodulator, &receiver.demodulator,
		     sizeof(handoff.demodulator)) == 0 &&
	      memcmp(&handoff.transmitting_profile,
		     &transmitting_profile,
		     sizeof(transmitting_profile)) == 0,
	      "Phase 3 line did not preserve its continuous handoff");
	complete = receiver;
	CHECK(v34_phase3_line_receiver_sample(&receiver, 0) ==
		      V34_PHASE3_LINE_RECEIVER_COMPLETE &&
	      memcmp(&receiver, &complete, sizeof(receiver)) == 0,
	      "completed Phase 3 line consumed another sample");
	return true;
}

static bool test_validation(void)
{
	struct v34_phase3_profile transmitting_profile = profile();
	struct v34_phase3_line_receiver_configuration config =
		configuration();
	struct v34_phase3_line_receiver receiver;
	struct v34_phase3_line_receiver saved;
	struct v34_phase3_line_receiver corrupt;
	struct qam_channel channel;
	struct qam_channel saved_channel;

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
	corrupt = receiver;
	corrupt.demodulator.acquisition_window_symbols++;
	CHECK(!v34_phase3_line_receiver_is_valid(NULL) &&
	      !v34_phase3_line_receiver_is_valid(&corrupt) &&
	      !v34_phase3_line_receiver_has_failed(&corrupt) &&
	      !v34_phase3_line_handoff_is_valid(NULL) &&
	      v34_phase3_line_receiver_sample(NULL, 0) ==
		      V34_PHASE3_LINE_RECEIVER_INVALID_ARGUMENT &&
	      strcmp(v34_phase3_line_receiver_result_name(
		     V34_PHASE3_LINE_RECEIVER_TIMING_ACQUIRED),
		     "timing acquired") == 0 &&
	      strcmp(v34_phase3_line_receiver_result_name(
		     (enum v34_phase3_line_receiver_result)99),
		     "unknown") == 0,
	      "Phase 3 line validation accepted corrupt state");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "generated line timeline", test_generated_line_timeline },
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
