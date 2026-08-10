// SPDX-License-Identifier: BSD-3-Clause

#include <sippymodem_g711.h>
#include <sippymodem_v34_phase2_probe_acquisition.h>
#include <sippymodem_v34_phase2_tone.h>

#include "test.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_NOMINAL_AMPLITUDE = 7000,
	TEST_EXTRA_L2_SAMPLES = 37,
};

enum test_codec {
	TEST_CODEC_LINEAR,
	TEST_CODEC_PCMU,
	TEST_CODEC_PCMA,
};

struct test_stream {
	struct sippymodem_v34_phase2_probe_acquisition acquisition;
	struct sippymodem_v34_phase2_probe_transmitter probe_transmitter;
	struct sippymodem_v34_phase2_tone_transmitter tone_transmitter;
	enum test_codec codec;
	uint16_t leading_samples;
	uint32_t line_samples;
	uint32_t probe_samples;
};

static int16_t codec_round_trip(int16_t sample, enum test_codec codec)
{
	switch (codec) {
	case TEST_CODEC_LINEAR:
		return sample;
	case TEST_CODEC_PCMU:
		return sippymodem_g711_decode_pcmu(
			sippymodem_g711_encode_pcmu(sample));
	case TEST_CODEC_PCMA:
		return sippymodem_g711_decode_pcma(
			sippymodem_g711_encode_pcma(sample));
	}
	return 0;
}

static bool stream_init(
	struct test_stream *stream, enum test_codec codec,
	uint16_t leading_samples)
{
	struct test_stream initialized = {
		.codec = codec,
		.leading_samples = leading_samples,
	};

	CHECK(leading_samples <=
		      SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_SEARCH_SAMPLES &&
	      sippymodem_v34_phase2_probe_acquisition_init(
		      &initialized.acquisition) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_OK &&
	      sippymodem_v34_phase2_probe_transmitter_init(
		      &initialized.probe_transmitter,
		      TEST_NOMINAL_AMPLITUDE) == SIPPYMODEM_V34_PHASE2_PROBE_OK &&
	      sippymodem_v34_phase2_tone_transmitter_init(
		      &initialized.tone_transmitter,
		      SIPPYMODEM_V34_CALLING_STATION,
		      TEST_NOMINAL_AMPLITUDE) == SIPPYMODEM_V34_PHASE2_TONE_OK,
	      "probe acquisition stream did not initialize");
	*stream = initialized;
	return true;
}

static bool stream_sample(
	struct test_stream *stream,
	struct sippymodem_v34_phase2_probe_acquisition_event *event)
{
	int16_t generated;

	if (stream->line_samples < stream->leading_samples) {
		CHECK(sippymodem_v34_phase2_tone_transmitter_sample(
			      &stream->tone_transmitter, &generated) ==
			      SIPPYMODEM_V34_PHASE2_TONE_OK,
		      "preceding tone generation failed at sample %u",
		      stream->line_samples);
	} else {
		enum sippymodem_v34_phase2_probe_level level =
			stream->probe_samples <
					SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES ?
				SIPPYMODEM_V34_PHASE2_PROBE_L1 :
				SIPPYMODEM_V34_PHASE2_PROBE_L2;

		CHECK(sippymodem_v34_phase2_probe_transmitter_sample(
			      &stream->probe_transmitter, level, &generated) ==
			      SIPPYMODEM_V34_PHASE2_PROBE_OK,
		      "probe generation failed at sample %u",
		      stream->probe_samples);
		stream->probe_samples++;
	}
	stream->line_samples++;
	CHECK(sippymodem_v34_phase2_probe_acquisition_sample(
		      &stream->acquisition,
		      codec_round_trip(generated, stream->codec), false,
		      event) == SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_OK,
	      "probe acquisition failed at line sample %u",
	      stream->line_samples);
	return true;
}

static enum sippymodem_v34_phase2_probe_acquisition_event_kind
expected_stream_event(const struct test_stream *stream)
{
	if (stream->line_samples ==
	    SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES)
		return SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_EVENT_ACQUIRED;
	if (stream->probe_samples == SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES)
		return SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_EVENT_L2_STARTED;
	if (stream->probe_samples ==
	    2u * SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES)
		return SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_EVENT_MEASUREMENT_READY;
	return SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_EVENT_NONE;
}

static bool stream_to_measurement(struct test_stream *stream)
{
	while (stream->probe_samples <
	       2u * SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES) {
		struct sippymodem_v34_phase2_probe_acquisition_event event;
		enum sippymodem_v34_phase2_probe_acquisition_event_kind expected;

		CHECK(stream_sample(stream, &event),
		      "probe measurement stream failed");
		expected = expected_stream_event(stream);
		CHECK(event.kind == expected,
		      "probe stream event changed at line sample %u: %d, expected %d",
		      stream->line_samples, event.kind, expected);
		if (event.kind ==
		    SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_EVENT_ACQUIRED)
			CHECK(event.leading_samples == stream->leading_samples &&
			      event.replay_samples ==
				      SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES -
					      stream->leading_samples &&
			      event.normalized_correlation > 0.99,
			      "probe acquisition diagnostics changed");
		if (event.kind ==
		    SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_EVENT_MEASUREMENT_READY)
			CHECK(event.l2_samples ==
				      SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES,
			      "probe measurement reported the wrong L2 duration");
	}
	CHECK(stream->acquisition.phase ==
		      SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_RECEIVING &&
	      stream->acquisition.receiver.phase ==
		      SIPPYMODEM_V34_PHASE2_PROBE_RECEIVER_WAIT_L2_END,
	      "probe stream did not reach a complete measurement");
	return true;
}

static double spectrum_power(
	const struct sippymodem_v34_phase2_probe_spectrum_measurement *spectrum)
{
	double power = 0.0;

	for (size_t index = 0;
	     index < SIPPYMODEM_V34_PHASE2_PROBE_HIGHEST_HARMONIC; ++index)
		power += spectrum->harmonics[index].mean_square;
	return power;
}

static bool check_measurement_and_completion(
	enum test_codec codec, uint16_t leading_samples)
{
	struct test_stream stream;
	struct sippymodem_v34_phase2_probe_receiver_measurement measurement;
	struct sippymodem_v34_phase2_probe_receiver_measurement initial;
	struct sippymodem_v34_phase2_probe_acquisition before;
	struct sippymodem_v34_phase2_probe_acquisition_event event;
	double l1_power;
	double l2_power;

	CHECK(stream_init(&stream, codec, leading_samples) &&
	      stream_to_measurement(&stream) &&
	      sippymodem_v34_phase2_probe_acquisition_measure(
		      &stream.acquisition, &measurement) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_OK,
	      "probe measurement did not complete for codec %d", codec);
	l1_power = spectrum_power(&measurement.l1);
	l2_power = spectrum_power(&measurement.l2);
	CHECK(measurement.l1.sample_count ==
		      SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES &&
	      measurement.l2.sample_count ==
		      SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES &&
	      measurement.l2_samples ==
		      SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES &&
	      l2_power > 0.0 && l1_power / l2_power > 3.9 &&
	      l1_power / l2_power < 4.1,
	      "probe acquisition changed measured windows for codec %d",
	      codec);
	initial = measurement;
	for (unsigned int index = 0; index < TEST_EXTRA_L2_SAMPLES; ++index) {
		CHECK(stream_sample(&stream, &event) &&
		      event.kind ==
			      SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_EVENT_NONE,
		      "extended L2 emitted an event at sample %u", index);
	}
	CHECK(sippymodem_v34_phase2_probe_acquisition_measure(
		      &stream.acquisition, &measurement) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_OK &&
	      measurement.l2_samples ==
		      SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES +
			      TEST_EXTRA_L2_SAMPLES &&
	      memcmp(&measurement.l1, &initial.l1, sizeof(measurement.l1)) ==
		      0 &&
	      memcmp(&measurement.l2, &initial.l2, sizeof(measurement.l2)) ==
		      0,
	      "extended L2 changed fixed probe measurements");
	CHECK(sippymodem_v34_phase2_probe_acquisition_sample(
		      &stream.acquisition, INT16_MIN, true, &event) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_OK &&
	      event.kind ==
		      SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_EVENT_COMPLETED &&
	      event.l2_samples ==
		      SIPPYMODEM_V34_PHASE2_PROBE_L1_SAMPLES +
			      TEST_EXTRA_L2_SAMPLES &&
	      stream.acquisition.phase ==
		      SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_COMPLETE,
	      "probe acquisition did not complete without consuming the next signal");
	before = stream.acquisition;
	CHECK(sippymodem_v34_phase2_probe_acquisition_sample(
		      &stream.acquisition, 1234, false, &event) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_OK &&
	      event.kind ==
		      SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_EVENT_NONE &&
	      memcmp(&stream.acquisition, &before, sizeof(before)) == 0,
	      "completed probe acquisition changed terminal state");
	return true;
}

static bool test_measurement_and_completion(void)
{
	static const uint16_t offsets[] = {
		0,
		7,
		SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_SEARCH_SAMPLES,
	};

	for (enum test_codec codec = TEST_CODEC_LINEAR;
	     codec <= TEST_CODEC_PCMA; ++codec)
		CHECK(check_measurement_and_completion(codec, offsets[codec]),
		      "probe acquisition failed for codec %d", codec);
	return true;
}

static bool test_receive_limit(void)
{
	struct test_stream stream;
	struct sippymodem_v34_phase2_probe_acquisition_event event;

	CHECK(stream_init(&stream, TEST_CODEC_LINEAR, 5) &&
	      stream_to_measurement(&stream),
	      "probe receive-limit fixture did not initialize");
	while (stream.acquisition.receiver.l2_samples <
	       SIPPYMODEM_V34_PHASE2_PROBE_L2_RECEIVE_LIMIT_SAMPLES) {
		uint32_t next_l2 = stream.acquisition.receiver.l2_samples + 1u;

		CHECK(stream_sample(&stream, &event) &&
		      event.kind ==
			      (next_l2 ==
				       SIPPYMODEM_V34_PHASE2_PROBE_L2_RECEIVE_LIMIT_SAMPLES ?
				       SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_EVENT_LIMIT_REACHED :
				       SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_EVENT_NONE),
		      "probe receive limit changed at L2 sample %u", next_l2);
	}
	CHECK(stream.acquisition.phase ==
		      SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_LIMIT_REACHED &&
	      event.l2_samples ==
		      SIPPYMODEM_V34_PHASE2_PROBE_L2_RECEIVE_LIMIT_SAMPLES,
	      "probe acquisition did not preserve the receive limit");
	return true;
}

static bool test_not_found(void)
{
	struct sippymodem_v34_phase2_probe_acquisition acquisition;
	struct sippymodem_v34_phase2_probe_acquisition before;
	struct sippymodem_v34_phase2_probe_acquisition_event event;

	CHECK(sippymodem_v34_phase2_probe_acquisition_init(&acquisition) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_OK,
	      "probe rejection fixture did not initialize");
	for (uint16_t index = 0;
	     index < SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES;
	     ++index) {
		bool final_sample =
			index + 1u ==
			SIPPYMODEM_V34_PHASE2_PROBE_SYNCHRONIZER_CAPTURE_SAMPLES;

		CHECK(sippymodem_v34_phase2_probe_acquisition_sample(
			      &acquisition, 0, false, &event) ==
			      SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_OK &&
		      event.kind ==
			      (final_sample ?
				       SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_EVENT_NOT_FOUND :
				       SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_EVENT_NONE),
		      "silence changed probe rejection at sample %u", index);
	}
	CHECK(acquisition.phase ==
		      SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_NOT_FOUND &&
	      event.normalized_correlation == 0.0,
	      "silence acquired as a probe");
	before = acquisition;
	CHECK(sippymodem_v34_phase2_probe_acquisition_sample(
		      &acquisition, INT16_MAX, true, &event) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_OK &&
	      event.kind ==
		      SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_EVENT_NONE &&
	      memcmp(&acquisition, &before, sizeof(before)) == 0,
	      "rejected acquisition changed terminal state");
	return true;
}

static bool test_validation(void)
{
	struct sippymodem_v34_phase2_probe_acquisition acquisition;
	struct sippymodem_v34_phase2_probe_acquisition before;
	struct sippymodem_v34_phase2_probe_acquisition_event event = {
		.kind = SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_EVENT_ACQUIRED,
		.leading_samples = 4,
		.replay_samples = 172,
		.normalized_correlation = 0.9,
		.l2_samples = 99,
	};
	struct sippymodem_v34_phase2_probe_acquisition_event event_before =
		event;
	struct sippymodem_v34_phase2_probe_receiver_measurement measurement;
	struct sippymodem_v34_phase2_probe_receiver_measurement
		measurement_before;

	memset(&measurement, 0x5a, sizeof(measurement));
	measurement_before = measurement;
	CHECK(sippymodem_v34_phase2_probe_acquisition_init(&acquisition) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_OK &&
	      sippymodem_v34_phase2_probe_acquisition_is_valid(&acquisition),
	      "probe acquisition validation fixture did not initialize");
	before = acquisition;
	CHECK(sippymodem_v34_phase2_probe_acquisition_sample(
		      &acquisition, 1, true, &event) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_INVALID_ARGUMENT &&
	      memcmp(&acquisition, &before, sizeof(before)) == 0 &&
	      memcmp(&event, &event_before, sizeof(event)) == 0 &&
	      sippymodem_v34_phase2_probe_acquisition_measure(
		      &acquisition, &measurement) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_INVALID_ARGUMENT &&
	      memcmp(&measurement, &measurement_before, sizeof(measurement)) ==
		      0,
	      "early completion or measurement was not transactional");
	acquisition.receiver.l1_spectrum.harmonics[0].in_phase_sum = 1.0;
	before = acquisition;
	CHECK(!sippymodem_v34_phase2_probe_acquisition_is_valid(&acquisition) &&
	      sippymodem_v34_phase2_probe_acquisition_sample(
		      &acquisition, 1, false, &event) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_INVALID_ARGUMENT &&
	      memcmp(&acquisition, &before, sizeof(before)) == 0 &&
	      memcmp(&event, &event_before, sizeof(event)) == 0,
	      "non-empty dormant receiver was accepted");
	CHECK(sippymodem_v34_phase2_probe_acquisition_init(&acquisition) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_OK,
	      "probe acquisition did not reinitialize");
	acquisition.phase =
		SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_RECEIVING;
	CHECK(!sippymodem_v34_phase2_probe_acquisition_is_valid(&acquisition),
	      "inconsistent acquisition phase was accepted");
	CHECK(sippymodem_v34_phase2_probe_acquisition_init(&acquisition) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_OK &&
	      sippymodem_v34_phase2_probe_acquisition_init(NULL) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_INVALID_ARGUMENT &&
	      !sippymodem_v34_phase2_probe_acquisition_is_valid(NULL) &&
	      sippymodem_v34_phase2_probe_acquisition_sample(
		      NULL, 0, false, &event) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_INVALID_ARGUMENT &&
	      sippymodem_v34_phase2_probe_acquisition_sample(
		      &acquisition, 0, false, NULL) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_INVALID_ARGUMENT &&
	      sippymodem_v34_phase2_probe_acquisition_measure(
		      NULL, &measurement) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_INVALID_ARGUMENT &&
	      sippymodem_v34_phase2_probe_acquisition_measure(
		      &acquisition, NULL) ==
		      SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_INVALID_ARGUMENT,
	      "probe acquisition null validation changed");
	CHECK(strcmp(
		      sippymodem_v34_phase2_probe_acquisition_result_name(
			      SIPPYMODEM_V34_PHASE2_PROBE_ACQUISITION_OK),
		      "ok") == 0 &&
	      strcmp(
		      sippymodem_v34_phase2_probe_acquisition_result_name(
			      (enum sippymodem_v34_phase2_probe_acquisition_result)99),
		      "unknown") == 0,
	      "probe acquisition result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "measurement and completion", test_measurement_and_completion },
		{ "receive limit", test_receive_limit },
		{ "not found", test_not_found },
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
