// SPDX-License-Identifier: BSD-3-Clause

#include <g711.h>
#include <v34_phase2_probe_receiver.h>

#include "test.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_NOMINAL_AMPLITUDE = 7000,
	TEST_EXTRA_L2_SAMPLES = 317,
};

enum test_codec {
	TEST_CODEC_LINEAR,
	TEST_CODEC_PCMU,
	TEST_CODEC_PCMA,
};

static int16_t codec_round_trip(int16_t sample, enum test_codec codec)
{
	switch (codec) {
	case TEST_CODEC_LINEAR:
		return sample;
	case TEST_CODEC_PCMU:
		return g711_decode_pcmu(
			g711_encode_pcmu(sample));
	case TEST_CODEC_PCMA:
		return g711_decode_pcma(
			g711_encode_pcma(sample));
	}
	return 0;
}

static double bin_peak(
	const struct v34_phase2_probe_spectrum_bin *bin)
{
	return hypot(bin->in_phase_peak, bin->quadrature_peak);
}

static bool feed_measurement_window(
	struct v34_phase2_probe_receiver *receiver,
	struct v34_phase2_probe_transmitter *transmitter,
	enum v34_phase2_probe_level level,
	enum test_codec codec,
	enum v34_phase2_probe_receiver_event_kind final_event)
{
	for (size_t index = 0;
	     index < V34_PHASE2_PROBE_L1_SAMPLES; ++index) {
		struct v34_phase2_probe_receiver_event event;
		bool final_sample = index + 1u ==
				    V34_PHASE2_PROBE_L1_SAMPLES;
		enum v34_phase2_probe_receiver_event_kind expected_kind =
			final_sample ?
				final_event :
				V34_PHASE2_PROBE_RECEIVER_EVENT_NONE;
		uint32_t expected_l2_samples =
			final_sample &&
				final_event ==
					V34_PHASE2_PROBE_RECEIVER_EVENT_MEASUREMENT_READY ?
				V34_PHASE2_PROBE_L1_SAMPLES :
				0;
		int16_t sample;

		CHECK(v34_phase2_probe_transmitter_sample(
			      transmitter, level, &sample) ==
			      V34_PHASE2_PROBE_OK &&
		      v34_phase2_probe_receiver_sample(
			      receiver, codec_round_trip(sample, codec), false,
			      &event) ==
			      V34_PHASE2_PROBE_RECEIVER_OK,
		      "probe receive window failed at sample %zu", index);
		CHECK(event.kind == expected_kind &&
		      event.l2_samples == expected_l2_samples,
		      "probe receive window emitted the wrong event at sample %zu",
		      index);
	}
	return true;
}

static bool measurement_spectrum_is_plausible(
	const struct v34_phase2_probe_receiver_measurement *measurement,
	enum test_codec codec)
{
	double expected_l2_peak =
		(double)TEST_NOMINAL_AMPLITUDE /
		sqrt((double)V34_PHASE2_PROBE_TONE_COUNT);
	double l1_power = 0.0;
	double l2_power = 0.0;
	double peak_tolerance = codec == TEST_CODEC_LINEAR ? 0.5 : 40.0;
	double omitted_tolerance = codec == TEST_CODEC_LINEAR ? 0.5 : 32.0;

	CHECK(measurement->l1.sample_count ==
		      V34_PHASE2_PROBE_L1_SAMPLES &&
	      measurement->l2.sample_count >=
		      V34_PHASE2_PROBE_PERIOD_SAMPLES,
	      "probe receiver did not preserve complete measurement windows");
	for (unsigned int harmonic = 1;
	     harmonic <= V34_PHASE2_PROBE_HIGHEST_HARMONIC;
	     ++harmonic) {
		const struct v34_phase2_probe_spectrum_bin *l1_bin =
			&measurement->l1.harmonics[harmonic - 1u];
		const struct v34_phase2_probe_spectrum_bin *l2_bin =
			&measurement->l2.harmonics[harmonic - 1u];

		if (!v34_phase2_probe_harmonic_is_transmitted(harmonic)) {
			CHECK(bin_peak(l1_bin) < omitted_tolerance * 2.1 &&
				      bin_peak(l2_bin) < omitted_tolerance,
			      "omitted harmonic %u leaked through codec %d: L1 %.3f L2 %.3f",
			      harmonic, codec, bin_peak(l1_bin), bin_peak(l2_bin));
		} else {
			CHECK(fabs(bin_peak(l2_bin) - expected_l2_peak) <
				      peak_tolerance,
			      "harmonic %u changed through codec %d: %.3f",
			      harmonic, codec, bin_peak(l2_bin));
		}
		l1_power += l1_bin->mean_square;
		l2_power += l2_bin->mean_square;
	}
	CHECK(l2_power > 0.0 && l1_power / l2_power > 3.94 &&
		      l1_power / l2_power < 4.03,
	      "L1/L2 power ratio changed through codec %d: %.6f",
	      codec, l1_power / l2_power);
	return true;
}

static bool check_measurement_and_completion(enum test_codec codec)
{
	struct v34_phase2_probe_transmitter transmitter;
	struct v34_phase2_probe_receiver receiver;
	struct v34_phase2_probe_receiver before;
	struct v34_phase2_probe_receiver_measurement measurement;
	struct v34_phase2_probe_receiver_measurement
		initial_measurement;
	struct v34_phase2_probe_receiver_event event;

	CHECK(v34_phase2_probe_transmitter_init(
		      &transmitter, TEST_NOMINAL_AMPLITUDE) ==
		      V34_PHASE2_PROBE_OK &&
	      v34_phase2_probe_receiver_init(&receiver) ==
		      V34_PHASE2_PROBE_RECEIVER_OK,
	      "probe receiver fixture did not initialize for codec %d", codec);
	CHECK(feed_measurement_window(
		      &receiver, &transmitter, V34_PHASE2_PROBE_L1,
		      codec,
		      V34_PHASE2_PROBE_RECEIVER_EVENT_L2_STARTED) &&
	      receiver.phase ==
		      V34_PHASE2_PROBE_RECEIVER_MEASURE_L2,
	      "probe receiver did not enter L2 for codec %d", codec);
	CHECK(feed_measurement_window(
		      &receiver, &transmitter, V34_PHASE2_PROBE_L2,
		      codec,
		      V34_PHASE2_PROBE_RECEIVER_EVENT_MEASUREMENT_READY) &&
	      receiver.phase ==
		      V34_PHASE2_PROBE_RECEIVER_WAIT_L2_END &&
	      v34_phase2_probe_receiver_measure(
		      &receiver, &measurement) ==
		      V34_PHASE2_PROBE_RECEIVER_OK &&
	      measurement.l2_samples ==
		      V34_PHASE2_PROBE_L1_SAMPLES &&
	      v34_phase2_probe_measurement_is_valid(&measurement) &&
	      measurement_spectrum_is_plausible(&measurement, codec),
	      "probe measurement failed for codec %d", codec);
	initial_measurement = measurement;
	for (size_t index = 0; index < TEST_EXTRA_L2_SAMPLES; ++index) {
		int16_t sample;

		CHECK(v34_phase2_probe_transmitter_sample(
			      &transmitter, V34_PHASE2_PROBE_L2,
			      &sample) == V34_PHASE2_PROBE_OK &&
		      v34_phase2_probe_receiver_sample(
			      &receiver, codec_round_trip(sample, codec), false,
			      &event) ==
			      V34_PHASE2_PROBE_RECEIVER_OK &&
		      event.kind ==
			      V34_PHASE2_PROBE_RECEIVER_EVENT_NONE &&
		      event.l2_samples == 0,
		      "extended L2 changed event state at sample %zu", index);
	}
	CHECK(v34_phase2_probe_receiver_measure(
		      &receiver, &measurement) ==
		      V34_PHASE2_PROBE_RECEIVER_OK &&
	      memcmp(&measurement.l1, &initial_measurement.l1,
		     sizeof(measurement.l1)) == 0 &&
	      memcmp(&measurement.l2, &initial_measurement.l2,
		     sizeof(measurement.l2)) == 0 &&
	      measurement.l2_samples ==
		      V34_PHASE2_PROBE_L1_SAMPLES +
			      TEST_EXTRA_L2_SAMPLES,
	      "extended L2 changed the fixed measurement for codec %d", codec);
	CHECK(v34_phase2_probe_receiver_sample(
		      &receiver, INT16_MIN, true, &event) ==
		      V34_PHASE2_PROBE_RECEIVER_OK &&
	      event.kind ==
		      V34_PHASE2_PROBE_RECEIVER_EVENT_COMPLETED &&
	      event.l2_samples ==
		      V34_PHASE2_PROBE_L1_SAMPLES +
			      TEST_EXTRA_L2_SAMPLES &&
	      receiver.phase ==
		      V34_PHASE2_PROBE_RECEIVER_COMPLETE,
	      "L2 completion consumed the following signal for codec %d", codec);
	before = receiver;
	CHECK(v34_phase2_probe_receiver_sample(
		      &receiver, 1234, false, &event) ==
		      V34_PHASE2_PROBE_RECEIVER_OK &&
	      event.kind ==
		      V34_PHASE2_PROBE_RECEIVER_EVENT_NONE &&
	      memcmp(&receiver, &before, sizeof(receiver)) == 0,
	      "completed receiver changed terminal state for codec %d", codec);
	receiver.l2_samples = V34_PHASE2_PROBE_L1_SAMPLES - 1;
	before = receiver;
	CHECK(!v34_phase2_probe_receiver_is_valid(&receiver) &&
	      v34_phase2_probe_receiver_sample(
		      &receiver, 1234, false, &event) ==
		      V34_PHASE2_PROBE_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &before, sizeof(receiver)) == 0,
	      "completed receiver accepted more measured than received samples");
	return true;
}

static bool test_measurement_and_completion(void)
{
	for (enum test_codec codec = TEST_CODEC_LINEAR;
	     codec <= TEST_CODEC_PCMA; ++codec)
		CHECK(check_measurement_and_completion(codec),
		      "probe receive failed for codec %d", codec);
	return true;
}

static bool test_short_l2(void)
{
	for (enum test_codec codec = TEST_CODEC_LINEAR;
	     codec <= TEST_CODEC_PCMA; ++codec) {
		struct v34_phase2_probe_transmitter transmitter;
		struct v34_phase2_probe_receiver receiver;

		CHECK(v34_phase2_probe_transmitter_init(
			      &transmitter, TEST_NOMINAL_AMPLITUDE) ==
			      V34_PHASE2_PROBE_OK &&
		      v34_phase2_probe_receiver_init(&receiver) ==
			      V34_PHASE2_PROBE_RECEIVER_OK &&
		      feed_measurement_window(
			      &receiver, &transmitter, V34_PHASE2_PROBE_L1,
			      codec, V34_PHASE2_PROBE_RECEIVER_EVENT_L2_STARTED),
		      "short L2 fixture failed for codec %d", codec);
		for (uint32_t count = 0;
		     count < V34_PHASE2_PROBE_L1_SAMPLES; ++count) {
			struct v34_phase2_probe_receiver ended = receiver;
			struct v34_phase2_probe_receiver_event event = { 0 };
			struct v34_phase2_probe_receiver_measurement measurement;
			bool whole_period = count != 0 &&
				count % V34_PHASE2_PROBE_PERIOD_SAMPLES == 0;
			int16_t sample;
			enum v34_phase2_probe_receiver_result result =
				v34_phase2_probe_receiver_sample(
					&ended, INT16_MIN, true, &event);

			if (whole_period) {
				CHECK(result == V34_PHASE2_PROBE_RECEIVER_OK &&
				      event.kind == V34_PHASE2_PROBE_RECEIVER_EVENT_COMPLETED &&
				      event.l2_samples == count &&
				      ended.phase == V34_PHASE2_PROBE_RECEIVER_COMPLETE &&
				      v34_phase2_probe_receiver_is_valid(&ended) &&
				      v34_phase2_probe_receiver_measure(&ended, &measurement) ==
					      V34_PHASE2_PROBE_RECEIVER_OK &&
				      measurement.l2.sample_count == count &&
				      measurement.l2_samples == count &&
				      v34_phase2_probe_measurement_is_valid(&measurement) &&
				      measurement_spectrum_is_plausible(&measurement, codec) &&
				      memcmp(&ended.l2_spectrum, &receiver.l2_spectrum,
					     sizeof(receiver.l2_spectrum)) == 0,
				      "short L2 completion failed: codec %d samples %u",
				      codec, count);
				measurement.l2.sample_count++;
				measurement.l2_samples++;
				CHECK(!v34_phase2_probe_measurement_is_valid(&measurement),
				      "partial-period L2 measurement was accepted");
			} else {
				CHECK(result == V34_PHASE2_PROBE_RECEIVER_INVALID_ARGUMENT &&
				      event.kind == V34_PHASE2_PROBE_RECEIVER_EVENT_NONE &&
				      event.l2_samples == 0 &&
				      memcmp(&ended, &receiver, sizeof(receiver)) == 0,
				      "partial-period completion changed state at %u", count);
			}
			CHECK(v34_phase2_probe_transmitter_sample(
				      &transmitter, V34_PHASE2_PROBE_L2, &sample) ==
				      V34_PHASE2_PROBE_OK &&
			      v34_phase2_probe_receiver_sample(
				      &receiver, codec_round_trip(sample, codec), false,
				      &event) == V34_PHASE2_PROBE_RECEIVER_OK,
			      "short L2 input failed at %u", count);
		}
	}
	return true;
}

static bool test_l2_limit(void)
{
	struct v34_phase2_probe_transmitter transmitter;
	struct v34_phase2_probe_receiver receiver;
	struct v34_phase2_probe_receiver_event event;
	uint32_t expected_limit =
		V34_PHASE2_PROBE_L2_RECEIVE_LIMIT_SAMPLES;

	CHECK(v34_phase2_probe_transmitter_init(
		      &transmitter, TEST_NOMINAL_AMPLITUDE) ==
		      V34_PHASE2_PROBE_OK &&
	      v34_phase2_probe_receiver_init(&receiver) ==
		      V34_PHASE2_PROBE_RECEIVER_OK &&
	      feed_measurement_window(
		      &receiver, &transmitter, V34_PHASE2_PROBE_L1,
		      TEST_CODEC_LINEAR,
		      V34_PHASE2_PROBE_RECEIVER_EVENT_L2_STARTED) &&
	      feed_measurement_window(
		      &receiver, &transmitter, V34_PHASE2_PROBE_L2,
		      TEST_CODEC_LINEAR,
		      V34_PHASE2_PROBE_RECEIVER_EVENT_MEASUREMENT_READY),
	      "L2 limit fixture failed");
	while (receiver.l2_samples < expected_limit) {
		uint32_t next_count = receiver.l2_samples + 1u;

		CHECK(v34_phase2_probe_receiver_sample(
			      &receiver, 0, false, &event) ==
			      V34_PHASE2_PROBE_RECEIVER_OK,
		      "L2 limit failed at sample %u", next_count);
		if (next_count == expected_limit)
			CHECK(event.kind ==
				      V34_PHASE2_PROBE_RECEIVER_EVENT_LIMIT_REACHED &&
				      event.l2_samples == expected_limit,
			      "L2 limit did not report its exact boundary");
		else
			CHECK(event.kind ==
				      V34_PHASE2_PROBE_RECEIVER_EVENT_NONE &&
				      event.l2_samples == 0,
			      "L2 limit reported an early event");
	}
	CHECK(receiver.phase ==
		      V34_PHASE2_PROBE_RECEIVER_LIMIT_REACHED &&
	      receiver.l2_samples == expected_limit &&
	      v34_phase2_probe_receiver_is_valid(&receiver),
	      "L2 receive limit state changed");
	return true;
}

static bool test_validation(void)
{
	struct v34_phase2_probe_transmitter transmitter;
	struct v34_phase2_probe_receiver receiver;
	struct v34_phase2_probe_receiver before;
	struct v34_phase2_probe_receiver_event event = {
		.kind = V34_PHASE2_PROBE_RECEIVER_EVENT_COMPLETED,
		.l2_samples = 99,
	};
	struct v34_phase2_probe_receiver_event event_before = event;
	struct v34_phase2_probe_receiver_measurement measurement;
	struct v34_phase2_probe_receiver_measurement
		measurement_before;

	CHECK(v34_phase2_probe_receiver_init(&receiver) ==
		      V34_PHASE2_PROBE_RECEIVER_OK,
	      "validation fixture did not initialize");
	before = receiver;
	memset(&measurement, 0xa5, sizeof(measurement));
	measurement_before = measurement;
	CHECK(v34_phase2_probe_receiver_sample(
		      &receiver, 1, true, &event) ==
		      V34_PHASE2_PROBE_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase2_probe_receiver_measure(
		      &receiver, &measurement) ==
		      V34_PHASE2_PROBE_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &before, sizeof(receiver)) == 0 &&
	      memcmp(&event, &event_before, sizeof(event)) == 0 &&
	      memcmp(&measurement, &measurement_before,
		     sizeof(measurement)) == 0,
	      "premature L2 operations changed caller-owned state");
	receiver.phase = V34_PHASE2_PROBE_RECEIVER_MEASURE_L2;
	before = receiver;
	CHECK(!v34_phase2_probe_receiver_is_valid(&receiver) &&
	      v34_phase2_probe_receiver_sample(
		      &receiver, 1, false, &event) ==
		      V34_PHASE2_PROBE_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &before, sizeof(receiver)) == 0,
	      "inconsistent L2 phase was accepted");
	CHECK(v34_phase2_probe_transmitter_init(
		      &transmitter, TEST_NOMINAL_AMPLITUDE) ==
		      V34_PHASE2_PROBE_OK &&
	      v34_phase2_probe_receiver_init(&receiver) ==
		      V34_PHASE2_PROBE_RECEIVER_OK &&
	      feed_measurement_window(
		      &receiver, &transmitter, V34_PHASE2_PROBE_L1,
		      TEST_CODEC_LINEAR,
		      V34_PHASE2_PROBE_RECEIVER_EVENT_L2_STARTED),
	      "L2 validation fixture did not reach its boundary");
	before = receiver;
	CHECK(v34_phase2_probe_receiver_sample(
		      &receiver, 1, true, &event) ==
		      V34_PHASE2_PROBE_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &before, sizeof(receiver)) == 0 &&
	      memcmp(&event, &event_before, sizeof(event)) == 0,
	      "partial L2 measurement accepted completion");
	receiver.l1_spectrum.harmonics[0].in_phase_sum = NAN;
	before = receiver;
	CHECK(!v34_phase2_probe_receiver_is_valid(&receiver) &&
	      v34_phase2_probe_receiver_sample(
		      &receiver, 1, false, &event) ==
		      V34_PHASE2_PROBE_RECEIVER_INVALID_ARGUMENT &&
	      memcmp(&receiver, &before, sizeof(receiver)) == 0,
	      "non-finite probe receiver state was accepted");
	memset(&measurement, 0, sizeof(measurement));
	measurement.l1.sample_count = V34_PHASE2_PROBE_L1_SAMPLES;
	measurement.l2.sample_count = V34_PHASE2_PROBE_L1_SAMPLES;
	measurement.l2_samples = V34_PHASE2_PROBE_L1_SAMPLES;
	CHECK(v34_phase2_probe_measurement_is_valid(&measurement),
	      "finite completed probe measurement was rejected");
	measurement.l1.harmonics[0].mean_square = -1.0;
	CHECK(!v34_phase2_probe_measurement_is_valid(&measurement) &&
	      !v34_phase2_probe_measurement_is_valid(NULL),
	      "invalid completed probe measurement was accepted");
	CHECK(v34_phase2_probe_receiver_init(&receiver) ==
		      V34_PHASE2_PROBE_RECEIVER_OK &&
	      v34_phase2_probe_receiver_init(NULL) ==
		      V34_PHASE2_PROBE_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase2_probe_receiver_sample(
		      NULL, 0, false, &event) ==
		      V34_PHASE2_PROBE_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase2_probe_receiver_sample(
		      &receiver, 0, false, NULL) ==
		      V34_PHASE2_PROBE_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase2_probe_receiver_measure(NULL, &measurement) ==
		      V34_PHASE2_PROBE_RECEIVER_INVALID_ARGUMENT &&
	      v34_phase2_probe_receiver_measure(&receiver, NULL) ==
		      V34_PHASE2_PROBE_RECEIVER_INVALID_ARGUMENT &&
	      !v34_phase2_probe_receiver_is_valid(NULL),
	      "probe receiver null validation changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "measurement and completion", test_measurement_and_completion },
		{ "short L2 completion", test_short_l2 },
		{ "L2 receive limit", test_l2_limit },
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
