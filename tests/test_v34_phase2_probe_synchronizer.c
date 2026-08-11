// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>
#include <v34_phase2_probe_receiver.h>
#include <v34_phase2_probe_synchronizer.h>
#include <v34_phase2_tone.h>

#include "test.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

enum { TEST_NOMINAL_AMPLITUDE = 7000 };

enum test_codec {
	TEST_CODEC_LINEAR,
	TEST_CODEC_PCMU,
	TEST_CODEC_PCMA,
};

static int16_t transmit_sample(int16_t sample, enum test_codec codec,
			       bool inverted)
{
	int16_t transmitted = inverted ? (int16_t)-(int32_t)sample : sample;

	switch (codec) {
	case TEST_CODEC_LINEAR:
		return transmitted;
	case TEST_CODEC_PCMU:
		return sippymodem_g711_decode_pcmu(
			sippymodem_g711_encode_pcmu(transmitted));
	case TEST_CODEC_PCMA:
		return sippymodem_g711_decode_pcma(
			sippymodem_g711_encode_pcma(transmitted));
	}
	return 0;
}

static bool acquire_probe(
	uint16_t leading_samples, enum test_codec codec, bool inverted,
	struct v34_phase2_probe_synchronizer *synchronizer,
	struct v34_phase2_probe_transmitter *probe_transmitter)
{
	struct v34_phase2_tone_transmitter tone_transmitter;
	struct v34_phase2_probe_synchronizer_event event;
	int16_t expected_replay[
		V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES];

	CHECK(leading_samples <=
		      V34_PHASE2_PROBE_SYNCHRONIZER_SEARCH_SAMPLES &&
	      v34_phase2_tone_transmitter_init(
		      &tone_transmitter, V34_CALLING_STATION,
		      TEST_NOMINAL_AMPLITUDE) == V34_PHASE2_TONE_OK &&
	      v34_phase2_probe_transmitter_init(
		      probe_transmitter, TEST_NOMINAL_AMPLITUDE) ==
		      V34_PHASE2_PROBE_OK &&
	      v34_phase2_probe_synchronizer_init(synchronizer) ==
		      V34_PHASE2_PROBE_SYNCHRONIZER_OK,
	      "probe synchronizer fixture did not initialize");
	for (uint16_t index = 0;
	     index < V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES;
	     ++index) {
		int16_t generated;
		int16_t received;
		bool final_sample =
			index + 1u ==
			V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES;

		if (index < leading_samples) {
			CHECK(v34_phase2_tone_transmitter_sample(
				      &tone_transmitter, &generated) ==
				      V34_PHASE2_TONE_OK,
			      "preceding tone generation failed at sample %u", index);
		} else {
			CHECK(v34_phase2_probe_transmitter_sample(
				      probe_transmitter,
				      V34_PHASE2_PROBE_L1,
				      &generated) == V34_PHASE2_PROBE_OK,
			      "L1 generation failed at sample %u", index);
		}
		received = transmit_sample(generated, codec, inverted);
		if (index >= leading_samples)
			expected_replay[index - leading_samples] = received;
		CHECK(v34_phase2_probe_synchronizer_sample(
			      synchronizer, received, &event) ==
			      V34_PHASE2_PROBE_SYNCHRONIZER_OK,
		      "probe synchronization failed at sample %u", index);
		CHECK(event.kind ==
			      (final_sample ?
				       V34_PHASE2_PROBE_SYNCHRONIZER_EVENT_ACQUIRED :
				       V34_PHASE2_PROBE_SYNCHRONIZER_EVENT_NONE),
		      "probe synchronizer emitted the wrong event at sample %u",
		      index);
	}
	CHECK(event.leading_samples == leading_samples &&
	      event.replay_samples ==
		      V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES -
			      leading_samples &&
	      event.normalized_correlation > 0.99,
	      "probe edge changed at offset %u, codec %d, polarity %d: "
	      "offset %u, replay %u, correlation %.6f",
	      leading_samples, codec, inverted, event.leading_samples,
	      event.replay_samples, event.normalized_correlation);
	CHECK(v34_phase2_probe_synchronizer_replay_count(
		      synchronizer) == event.replay_samples,
	      "probe replay count disagreed with acquisition event");
	for (uint16_t index = 0; index < event.replay_samples; ++index) {
		int16_t replayed = 0;

		CHECK(v34_phase2_probe_synchronizer_replay(
			      synchronizer, index, &replayed) ==
			      V34_PHASE2_PROBE_SYNCHRONIZER_OK &&
		      replayed == expected_replay[index],
		      "probe replay changed sample %u", index);
	}
	{
		struct v34_phase2_probe_synchronizer before =
			*synchronizer;
		int16_t replayed = 1234;

		CHECK(v34_phase2_probe_synchronizer_replay(
			      synchronizer, event.replay_samples, &replayed) ==
			      V34_PHASE2_PROBE_SYNCHRONIZER_INVALID_ARGUMENT &&
		      replayed == 1234 &&
		      v34_phase2_probe_synchronizer_sample(
			      synchronizer, INT16_MAX, &event) ==
			      V34_PHASE2_PROBE_SYNCHRONIZER_OK &&
		      event.kind ==
			      V34_PHASE2_PROBE_SYNCHRONIZER_EVENT_NONE &&
		      memcmp(synchronizer, &before, sizeof(before)) == 0,
		      "acquired probe synchronizer changed terminal state");
	}
	return true;
}

static bool replay_into_receiver(
	uint16_t leading_samples, enum test_codec codec, bool inverted)
{
	struct v34_phase2_probe_synchronizer synchronizer;
	struct v34_phase2_probe_transmitter transmitter;
	struct v34_phase2_probe_receiver receiver;
	struct v34_phase2_probe_receiver_event event;
	uint16_t replay_samples;
	uint32_t l1_samples;

	CHECK(acquire_probe(leading_samples, codec, inverted, &synchronizer,
			    &transmitter) &&
	      v34_phase2_probe_receiver_init(&receiver) ==
		      V34_PHASE2_PROBE_RECEIVER_OK,
	      "probe replay fixture did not initialize");
	replay_samples =
		v34_phase2_probe_synchronizer_replay_count(
			&synchronizer);
	for (uint16_t index = 0; index < replay_samples; ++index) {
		int16_t sample;

		CHECK(v34_phase2_probe_synchronizer_replay(
			      &synchronizer, index, &sample) ==
			      V34_PHASE2_PROBE_SYNCHRONIZER_OK &&
		      v34_phase2_probe_receiver_sample(
			      &receiver, sample, false, &event) ==
			      V34_PHASE2_PROBE_RECEIVER_OK &&
		      event.kind ==
			      V34_PHASE2_PROBE_RECEIVER_EVENT_NONE,
		      "probe replay failed at sample %u", index);
	}
	l1_samples = replay_samples;
	while (l1_samples < V34_PHASE2_PROBE_L1_SAMPLES) {
		int16_t generated;
		bool final_sample =
			l1_samples + 1u == V34_PHASE2_PROBE_L1_SAMPLES;

		CHECK(v34_phase2_probe_transmitter_sample(
			      &transmitter, V34_PHASE2_PROBE_L1,
			      &generated) == V34_PHASE2_PROBE_OK &&
		      v34_phase2_probe_receiver_sample(
			      &receiver,
			      transmit_sample(generated, codec, inverted), false,
			      &event) == V34_PHASE2_PROBE_RECEIVER_OK &&
		      event.kind ==
			      (final_sample ?
				       V34_PHASE2_PROBE_RECEIVER_EVENT_L2_STARTED :
				       V34_PHASE2_PROBE_RECEIVER_EVENT_NONE),
		      "probe receiver boundary changed at sample %u", l1_samples);
		l1_samples++;
	}
	CHECK(receiver.phase ==
		      V34_PHASE2_PROBE_RECEIVER_MEASURE_L2 &&
	      receiver.l1_spectrum.samples_observed ==
		      V34_PHASE2_PROBE_L1_SAMPLES,
	      "probe replay did not preserve the complete L1 window");
	return true;
}

static bool test_offsets_and_codecs(void)
{
	for (uint16_t offset = 0;
	     offset <= V34_PHASE2_PROBE_SYNCHRONIZER_SEARCH_SAMPLES;
	     ++offset)
		for (enum test_codec codec = TEST_CODEC_LINEAR;
		     codec <= TEST_CODEC_PCMA; ++codec)
			for (unsigned int polarity = 0; polarity < 2; ++polarity)
				CHECK(replay_into_receiver(
					      offset, codec, polarity != 0),
				      "probe acquisition failed at offset %u, "
				      "codec %d, polarity %u",
				      offset, codec, polarity);
	return true;
}

static bool check_rejected_signal(bool silence)
{
	struct v34_phase2_probe_synchronizer synchronizer;
	struct v34_phase2_probe_synchronizer before;
	struct v34_phase2_probe_synchronizer_event event;
	struct v34_phase2_tone_transmitter tone_transmitter;

	CHECK(v34_phase2_probe_synchronizer_init(&synchronizer) ==
		      V34_PHASE2_PROBE_SYNCHRONIZER_OK &&
	      v34_phase2_tone_transmitter_init(
		      &tone_transmitter, V34_CALLING_STATION,
		      TEST_NOMINAL_AMPLITUDE) == V34_PHASE2_TONE_OK,
	      "rejected-signal fixture did not initialize");
	for (uint16_t index = 0;
	     index < V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES;
	     ++index) {
		int16_t sample = 0;
		bool final_sample =
			index + 1u ==
			V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES;

		if (!silence)
			CHECK(v34_phase2_tone_transmitter_sample(
				      &tone_transmitter, &sample) ==
				      V34_PHASE2_TONE_OK,
			      "false tone generation failed at sample %u", index);
		CHECK(v34_phase2_probe_synchronizer_sample(
			      &synchronizer, sample, &event) ==
			      V34_PHASE2_PROBE_SYNCHRONIZER_OK &&
		      event.kind ==
			      (final_sample ?
				       V34_PHASE2_PROBE_SYNCHRONIZER_EVENT_NOT_FOUND :
				       V34_PHASE2_PROBE_SYNCHRONIZER_EVENT_NONE),
		      "non-probe signal changed acquisition at sample %u", index);
	}
	CHECK(synchronizer.phase ==
		      V34_PHASE2_PROBE_SYNCHRONIZER_NOT_FOUND &&
	      event.replay_samples == 0 &&
	      v34_phase2_probe_synchronizer_replay_count(
		      &synchronizer) == 0,
	      "non-probe signal exposed replay samples");
	before = synchronizer;
	memset(&event, 0x5a, sizeof(event));
	CHECK(v34_phase2_probe_synchronizer_sample(
		      &synchronizer, INT16_MAX, &event) ==
		      V34_PHASE2_PROBE_SYNCHRONIZER_OK &&
	      event.kind ==
		      V34_PHASE2_PROBE_SYNCHRONIZER_EVENT_NONE &&
	      memcmp(&synchronizer, &before, sizeof(synchronizer)) == 0,
	      "terminal rejection state changed");
	return true;
}

static bool test_false_acquisition(void)
{
	return check_rejected_signal(true) && check_rejected_signal(false);
}

static bool test_validation(void)
{
	struct v34_phase2_probe_synchronizer synchronizer;
	struct v34_phase2_probe_synchronizer before;
	struct v34_phase2_probe_synchronizer_event event = {
		.kind = V34_PHASE2_PROBE_SYNCHRONIZER_EVENT_ACQUIRED,
		.leading_samples = 12,
		.replay_samples = 34,
		.normalized_correlation = 0.5,
	};
	struct v34_phase2_probe_synchronizer_event event_before =
		event;
	int16_t replayed = 1234;

	CHECK(v34_phase2_probe_synchronizer_init(&synchronizer) ==
		      V34_PHASE2_PROBE_SYNCHRONIZER_OK &&
	      v34_phase2_probe_synchronizer_is_valid(
		      &synchronizer) &&
	      v34_phase2_probe_synchronizer_replay_count(
		      &synchronizer) == 0 &&
	      v34_phase2_probe_synchronizer_replay(
		      &synchronizer, 0, &replayed) ==
		      V34_PHASE2_PROBE_SYNCHRONIZER_INVALID_ARGUMENT &&
	      replayed == 1234,
	      "unacquired replay validation changed");
	before = synchronizer;
	CHECK(v34_phase2_probe_synchronizer_sample(
		      &synchronizer, 1, NULL) ==
		      V34_PHASE2_PROBE_SYNCHRONIZER_INVALID_ARGUMENT &&
	      memcmp(&synchronizer, &before, sizeof(synchronizer)) == 0 &&
	      v34_phase2_probe_synchronizer_sample(
		      NULL, 1, &event) ==
		      V34_PHASE2_PROBE_SYNCHRONIZER_INVALID_ARGUMENT &&
	      memcmp(&event, &event_before, sizeof(event)) == 0,
	      "null sample validation was not transactional");
	synchronizer.samples_observed =
		V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES;
	before = synchronizer;
	CHECK(!v34_phase2_probe_synchronizer_is_valid(
		      &synchronizer) &&
	      v34_phase2_probe_synchronizer_sample(
		      &synchronizer, 1, &event) ==
		      V34_PHASE2_PROBE_SYNCHRONIZER_INVALID_ARGUMENT &&
	      memcmp(&synchronizer, &before, sizeof(synchronizer)) == 0 &&
	      memcmp(&event, &event_before, sizeof(event)) == 0 &&
	      v34_phase2_probe_synchronizer_replay_count(
		      &synchronizer) == 0,
	      "inconsistent collection state was accepted");
	CHECK(v34_phase2_probe_synchronizer_init(&synchronizer) ==
		      V34_PHASE2_PROBE_SYNCHRONIZER_OK,
	      "probe synchronizer did not reinitialize");
	synchronizer.normalized_correlation = NAN;
	CHECK(!v34_phase2_probe_synchronizer_is_valid(
		      &synchronizer),
	      "non-finite correlation was accepted");
	CHECK(v34_phase2_probe_synchronizer_init(NULL) ==
		      V34_PHASE2_PROBE_SYNCHRONIZER_INVALID_ARGUMENT &&
	      !v34_phase2_probe_synchronizer_is_valid(NULL) &&
	      v34_phase2_probe_synchronizer_replay(
		      NULL, 0, &replayed) ==
		      V34_PHASE2_PROBE_SYNCHRONIZER_INVALID_ARGUMENT &&
	      v34_phase2_probe_synchronizer_replay(
		      &synchronizer, 0, NULL) ==
		      V34_PHASE2_PROBE_SYNCHRONIZER_INVALID_ARGUMENT,
	      "probe synchronizer null validation changed");
	CHECK(strcmp(
		      v34_phase2_probe_synchronizer_result_name(
			      V34_PHASE2_PROBE_SYNCHRONIZER_OK),
		      "ok") == 0 &&
	      strcmp(
		      v34_phase2_probe_synchronizer_result_name(
			      (enum v34_phase2_probe_synchronizer_result)99),
		      "unknown") == 0,
	      "probe synchronizer result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "offsets and codecs", test_offsets_and_codecs },
		{ "false acquisition", test_false_acquisition },
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
