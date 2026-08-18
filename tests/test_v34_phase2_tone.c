// SPDX-License-Identifier: BSD-3-Clause

#include <v34_phase2_tone.h>
#include <g711.h>

#include "test.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
	TEST_AMPLITUDE = 12000,
	TEST_SAMPLES = V34_PHASE2_SAMPLE_RATE / 10,
};

enum test_codec {
	TEST_CODEC_LINEAR,
	TEST_CODEC_PCMU,
	TEST_CODEC_PCMA,
};

static const double TEST_PI = 3.1415926535897932384626433832795;

static double tone_projection(
	const int16_t *samples, size_t count, uint32_t frequency_hz)
{
	double in_phase = 0.0;
	double quadrature = 0.0;

	for (size_t index = 0; index < count; ++index) {
		double angle = 2.0 * TEST_PI * (double)frequency_hz *
			       (double)index /
			       V34_PHASE2_SAMPLE_RATE;

		in_phase += samples[index] * cos(angle);
		quadrature += samples[index] * sin(angle);
	}
	return in_phase * in_phase + quadrature * quadrature;
}

static bool render(
	enum v34_station_role role,
	int16_t samples[TEST_SAMPLES],
	struct v34_phase2_tone_transmitter *transmitter)
{
	CHECK(v34_phase2_tone_transmitter_init(
		      transmitter, role, TEST_AMPLITUDE) ==
		      V34_PHASE2_TONE_OK,
	      "Phase 2 tone transmitter did not initialize");
	for (size_t index = 0; index < TEST_SAMPLES; ++index)
		CHECK(v34_phase2_tone_transmitter_sample(
			      transmitter, &samples[index]) ==
			      V34_PHASE2_TONE_OK,
		      "Phase 2 tone generation failed at sample %zu", index);
	return true;
}

static bool test_tone_levels(void)
{
	struct v34_phase2_tone_transmitter calling;
	struct v34_phase2_tone_transmitter answering;
	int16_t calling_samples[TEST_SAMPLES];
	int16_t answering_samples[TEST_SAMPLES];
	double answering_carrier;
	double answering_guard;

	CHECK(render(
		      V34_CALLING_STATION,
		      calling_samples, &calling) &&
		      render(
			      V34_ANSWERING_STATION,
			      answering_samples, &answering),
	      "Phase 2 tone fixture failed");
	CHECK(calling.carrier_amplitude == TEST_AMPLITUDE &&
		      calling.guard_amplitude == 0,
	      "Tone B changed its nominal level");
	CHECK(answering.carrier_amplitude >= 10690 &&
		      answering.carrier_amplitude <= 10700 &&
		      answering.guard_amplitude == TEST_AMPLITUDE,
	      "Tone A changed its carrier or guard level");
	CHECK(tone_projection(
		      calling_samples, TEST_SAMPLES,
		      V34_PHASE2_CALLING_CARRIER_HZ) >
		      tone_projection(
			      calling_samples, TEST_SAMPLES,
			      V34_PHASE2_ANSWERING_CARRIER_HZ) *
			      1000.0,
	      "Tone B omitted its 1200 Hz carrier");
	answering_carrier = tone_projection(
		answering_samples, TEST_SAMPLES,
		V34_PHASE2_ANSWERING_CARRIER_HZ);
	answering_guard = tone_projection(
		answering_samples, TEST_SAMPLES,
		V34_PHASE2_ANSWERING_GUARD_HZ);
	CHECK(answering_guard > answering_carrier * 1.24 &&
		      answering_guard < answering_carrier * 1.28,
	      "Tone A did not retain its nominal guard and -1 dB carrier");
	return true;
}

static bool test_phase_reversal(void)
{
	struct v34_phase2_tone_transmitter baseline;
	struct v34_phase2_tone_transmitter reversed;
	int16_t baseline_samples[TEST_SAMPLES];
	int16_t reversed_samples[TEST_SAMPLES];
	int16_t sum[TEST_SAMPLES];
	int16_t difference[TEST_SAMPLES];

	CHECK(v34_phase2_tone_transmitter_init(
		      &baseline, V34_ANSWERING_STATION,
		      TEST_AMPLITUDE) == V34_PHASE2_TONE_OK,
	      "Tone A reversal fixture did not initialize");
	for (size_t index = 0; index < 37; ++index) {
		int16_t discarded;

		CHECK(v34_phase2_tone_transmitter_sample(
			      &baseline, &discarded) ==
			      V34_PHASE2_TONE_OK,
		      "Tone A prelude failed");
	}
	reversed = baseline;
	CHECK(v34_phase2_tone_transmitter_reverse(&reversed) ==
		      V34_PHASE2_TONE_OK,
	      "Tone A did not accept a phase reversal");
	for (size_t index = 0; index < TEST_SAMPLES; ++index) {
		CHECK(v34_phase2_tone_transmitter_sample(
			      &baseline, &baseline_samples[index]) ==
			      V34_PHASE2_TONE_OK &&
		      v34_phase2_tone_transmitter_sample(
			      &reversed, &reversed_samples[index]) ==
			      V34_PHASE2_TONE_OK,
		      "Tone A comparison failed at sample %zu", index);
		sum[index] = (int16_t)(
			baseline_samples[index] + reversed_samples[index]);
		difference[index] = (int16_t)(
			baseline_samples[index] - reversed_samples[index]);
	}
	CHECK(baseline.guard_phase == reversed.guard_phase &&
		      baseline.carrier_phase ==
			      reversed.carrier_phase + UINT32_C(0x80000000),
	      "Tone A reversal disturbed guard continuity");
	CHECK(tone_projection(
		      sum, TEST_SAMPLES,
		      V34_PHASE2_ANSWERING_GUARD_HZ) >
		      tone_projection(
			      sum, TEST_SAMPLES,
			      V34_PHASE2_ANSWERING_CARRIER_HZ) *
			      1000.0 &&
		      tone_projection(
			      difference, TEST_SAMPLES,
			      V34_PHASE2_ANSWERING_CARRIER_HZ) >
		      tone_projection(
			      difference, TEST_SAMPLES,
			      V34_PHASE2_ANSWERING_GUARD_HZ) *
			      1000.0,
	      "Tone A reversal did not invert only its carrier");
	return true;
}

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

static bool feed_sample(
	struct v34_phase2_tone_detector *detector,
	int16_t sample, enum test_codec codec, size_t sample_index,
	size_t *started_count, size_t *reversal_count, size_t *ended_count,
	struct v34_phase2_tone_event *last_reversal)
{
	struct v34_phase2_tone_event event;

	CHECK(v34_phase2_tone_detector_sample(
		      detector, codec_round_trip(sample, codec), &event) ==
		      V34_PHASE2_TONE_OK,
	      "tone detector failed at sample %zu", sample_index);
	switch (event.kind) {
	case V34_PHASE2_TONE_NO_EVENT:
		break;
	case V34_PHASE2_TONE_STARTED:
		CHECK(event.reversal_delay_samples == 0,
		      "tone-start event carried reversal latency");
		(*started_count)++;
		break;
	case V34_PHASE2_TONE_PHASE_REVERSED:
		CHECK(event.reversal_delay_samples != 0,
		      "phase-reversal event omitted detector latency");
		(*reversal_count)++;
		*last_reversal = event;
		break;
	case V34_PHASE2_TONE_ENDED:
		CHECK(event.reversal_delay_samples == 0,
		      "tone-end event carried reversal latency");
		(*ended_count)++;
		break;
	}
	return true;
}

static bool check_detector(
	enum v34_station_role transmit_role,
	enum test_codec codec)
{
	enum v34_station_role local_role =
		transmit_role == V34_CALLING_STATION ?
			V34_ANSWERING_STATION :
			V34_CALLING_STATION;
	struct v34_phase2_tone_transmitter transmitter;
	struct v34_phase2_tone_detector detector;
	struct v34_phase2_tone_measurement measurement;
	struct v34_phase2_tone_event reversal = { 0 };
	size_t started_count = 0;
	size_t reversal_count = 0;
	size_t ended_count = 0;
	size_t sample_index = 0;
	size_t edge_sample;
	size_t event_sample = 0;

	CHECK(v34_phase2_tone_transmitter_init(
		      &transmitter, transmit_role, TEST_AMPLITUDE) ==
		      V34_PHASE2_TONE_OK &&
		      v34_phase2_tone_detector_init(
			      &detector, local_role) ==
			      V34_PHASE2_TONE_OK,
	      "tone detection fixture did not initialize");
	for (; sample_index < 23; ++sample_index)
		CHECK(feed_sample(
			      &detector, 0, codec, sample_index,
			      &started_count, &reversal_count, &ended_count,
			      &reversal),
		      "leading silence failed");
	for (size_t count = 0; count < 400; ++count, ++sample_index) {
		int16_t sample;

		CHECK(v34_phase2_tone_transmitter_sample(
			      &transmitter, &sample) ==
			      V34_PHASE2_TONE_OK &&
		      feed_sample(
			      &detector, sample, codec, sample_index,
			      &started_count, &reversal_count, &ended_count,
			      &reversal),
		      "steady tone detection failed");
	}
	measurement = v34_phase2_tone_detector_measure(&detector);
	CHECK(started_count == 1 && reversal_count == 0 && ended_count == 0 &&
		      measurement.ready && measurement.tone_present &&
		      measurement.line_power > 1000000.0,
	      "detector did not acquire one steady remote tone");
	if (transmit_role == V34_ANSWERING_STATION)
		CHECK(measurement.carrier_share > 0.35 &&
			      measurement.carrier_share < 0.55,
		      "Tone A carrier/guard measurement changed");
	else
		CHECK(measurement.carrier_share > 0.90 &&
			      measurement.carrier_share < 1.05,
		      "Tone B carrier measurement changed");

	edge_sample = sample_index;
	CHECK(v34_phase2_tone_transmitter_reverse(&transmitter) ==
		      V34_PHASE2_TONE_OK,
	      "remote tone did not reverse");
	for (size_t count = 0; count < 160; ++count, ++sample_index) {
		int16_t sample;
		size_t before = reversal_count;

		CHECK(v34_phase2_tone_transmitter_sample(
			      &transmitter, &sample) ==
			      V34_PHASE2_TONE_OK &&
		      feed_sample(
			      &detector, sample, codec, sample_index,
			      &started_count, &reversal_count, &ended_count,
			      &reversal),
		      "reversed tone detection failed");
		if (reversal_count != before)
			event_sample = sample_index;
	}
	CHECK(reversal_count == 1 &&
		      event_sample >= reversal.reversal_delay_samples &&
		      event_sample - reversal.reversal_delay_samples + 2u >=
			      edge_sample &&
		      event_sample - reversal.reversal_delay_samples <=
			      edge_sample + 2u,
	      "reversal edge estimate moved away from the line edge");
	for (size_t count = 0; count < 200; ++count, ++sample_index)
		CHECK(feed_sample(
			      &detector, 0, codec, sample_index,
			      &started_count, &reversal_count, &ended_count,
			      &reversal),
		      "tone release failed");
	measurement = v34_phase2_tone_detector_measure(&detector);
	CHECK(started_count == 1 && reversal_count == 1 && ended_count == 1 &&
		      !measurement.tone_present,
	      "tone detector did not emit one bounded lifecycle");
	return true;
}

static bool test_detection_and_reversal(void)
{
	for (enum test_codec codec = TEST_CODEC_LINEAR;
	     codec <= TEST_CODEC_PCMA; ++codec)
		CHECK(check_detector(
			      V34_CALLING_STATION, codec) &&
		      check_detector(
			      V34_ANSWERING_STATION, codec),
		      "bidirectional tone detection failed for codec %d",
		      codec);
	return true;
}

static bool test_wrong_tone_rejection(void)
{
	struct v34_phase2_tone_transmitter transmitter;
	struct v34_phase2_tone_detector detector;
	struct v34_phase2_tone_event event;

	CHECK(v34_phase2_tone_transmitter_init(
		      &transmitter, V34_CALLING_STATION,
		      TEST_AMPLITUDE) == V34_PHASE2_TONE_OK &&
		      v34_phase2_tone_detector_init(
			      &detector,
			      V34_CALLING_STATION) ==
			      V34_PHASE2_TONE_OK,
	      "wrong-tone fixture did not initialize");
	for (size_t index = 0; index < TEST_SAMPLES; ++index) {
		int16_t sample;

		CHECK(v34_phase2_tone_transmitter_sample(
			      &transmitter, &sample) ==
			      V34_PHASE2_TONE_OK &&
		      v34_phase2_tone_detector_sample(
			      &detector, sample, &event) ==
			      V34_PHASE2_TONE_OK &&
		      event.kind == V34_PHASE2_TONE_NO_EVENT,
		      "detector accepted the local station's carrier at %zu",
		      index);
	}
	CHECK(!v34_phase2_tone_detector_measure(&detector)
		       .tone_present,
	      "wrong carrier left the detector locked");
	return true;
}

static bool check_frequency_offset(
	enum v34_station_role transmit_role,
	double frequency_scale)
{
	enum v34_station_role local_role =
		transmit_role == V34_CALLING_STATION ?
			V34_ANSWERING_STATION :
			V34_CALLING_STATION;
	struct v34_phase2_tone_transmitter levels;
	struct v34_phase2_tone_detector detector;
	const size_t edge_sample = 4803;
	double carrier_phase = 0.37;
	double guard_phase = 1.11;
	double carrier_step;
	double guard_step;
	size_t started_count = 0;
	size_t reversal_count = 0;
	size_t estimated_edge = 0;
	bool inverted = false;

	CHECK(v34_phase2_tone_transmitter_init(
		      &levels, transmit_role, TEST_AMPLITUDE) ==
		      V34_PHASE2_TONE_OK &&
		      v34_phase2_tone_detector_init(
			      &detector, local_role) ==
			      V34_PHASE2_TONE_OK,
	      "frequency-offset fixture did not initialize");
	carrier_step = 2.0 * TEST_PI * frequency_scale *
			(transmit_role == V34_CALLING_STATION ?
				 V34_PHASE2_CALLING_CARRIER_HZ :
				 V34_PHASE2_ANSWERING_CARRIER_HZ) /
			V34_PHASE2_SAMPLE_RATE;
	guard_step = 2.0 * TEST_PI * frequency_scale *
		     V34_PHASE2_ANSWERING_GUARD_HZ /
		     V34_PHASE2_SAMPLE_RATE;
	for (size_t index = 0;
	     index < V34_PHASE2_SAMPLE_RATE; ++index) {
		struct v34_phase2_tone_event event;
		long sample;

		if (index == edge_sample)
			inverted = true;
		sample = lround(
			(double)levels.carrier_amplitude * sin(carrier_phase));
		if (inverted)
			sample = -sample;
		if (levels.guard_amplitude != 0)
			sample += lround(
				(double)levels.guard_amplitude * sin(guard_phase));
		carrier_phase += carrier_step;
		guard_phase += guard_step;
		CHECK(v34_phase2_tone_detector_sample(
			      &detector, (int16_t)sample, &event) ==
			      V34_PHASE2_TONE_OK,
		      "offset tone failed at sample %zu", index);
		if (event.kind == V34_PHASE2_TONE_STARTED)
			started_count++;
		if (event.kind == V34_PHASE2_TONE_PHASE_REVERSED) {
			reversal_count++;
			CHECK(index >= event.reversal_delay_samples,
			      "offset reversal reported impossible latency");
			estimated_edge = index - event.reversal_delay_samples;
		}
	}
	CHECK(started_count == 1 && reversal_count == 1 &&
		      estimated_edge + 3u >= edge_sample &&
		      estimated_edge <= edge_sample + 3u,
	      "detector lost a %.4f-frequency-scale reversal",
	      frequency_scale);
	return true;
}

static bool test_frequency_tolerance(void)
{
	static const double scales[] = { 0.9999, 1.0001 };

	for (size_t index = 0; index < ARRAY_LENGTH(scales); ++index)
		CHECK(check_frequency_offset(
			      V34_CALLING_STATION,
			      scales[index]) &&
		      check_frequency_offset(
			      V34_ANSWERING_STATION,
			      scales[index]),
		      "Phase 2 tone offset failed at scale %.4f",
		      scales[index]);
	return true;
}

static bool feed_retrain_sample(
	struct v34_retrain_detector *detector, int16_t sample,
	enum test_codec codec, bool *seen)
{
	bool detected;

	CHECK(v34_retrain_detector_sample(
		      detector, codec_round_trip(sample, codec),
		      &detected) == V34_PHASE2_TONE_OK,
	      "full-retrain detector rejected a sample");
	*seen |= detected;
	return true;
}

static bool arm_retrain_silence(
	struct v34_retrain_detector *detector, enum test_codec codec)
{
	bool seen = false;

	for (size_t index = 0;
	     index < V34_PHASE2_TONE_WINDOW_SAMPLES +
			     V34_RETRAIN_MIN_SILENCE_SAMPLES;
	     ++index)
		CHECK(feed_retrain_sample(detector, 0, codec, &seen),
		      "full-retrain silence failed at %zu", index);
	return !seen && detector->silence_samples ==
			V34_RETRAIN_MIN_SILENCE_SAMPLES;
}

static bool check_full_retrain_request(
	enum v34_station_role local_role, enum test_codec codec)
{
	enum v34_station_role remote_role =
		local_role == V34_CALLING_STATION ?
			V34_ANSWERING_STATION : V34_CALLING_STATION;
	struct v34_phase2_tone_transmitter tone;
	struct v34_retrain_detector detector;
	struct v34_phase2_tone_event reversal = { 0 };
	size_t reversal_count = 0;
	size_t reversal_sample = 0;
	bool seen = false;

	CHECK(v34_retrain_detector_init(&detector, local_role) ==
		      V34_PHASE2_TONE_OK &&
	      v34_phase2_tone_transmitter_init(
		      &tone, remote_role, TEST_AMPLITUDE) ==
		      V34_PHASE2_TONE_OK,
	      "full-retrain fixture did not initialize");
	for (size_t index = 0;
	     index < V34_RETRAIN_TONE_SAMPLES +
			     V34_RETRAIN_EDGE_SAMPLES;
	     ++index) {
		int16_t sample;

		CHECK(v34_phase2_tone_transmitter_sample(&tone, &sample) ==
			      V34_PHASE2_TONE_OK &&
		      feed_retrain_sample(&detector, sample, codec, &seen),
		      "unarmed retrain tone failed at %zu", index);
	}
	CHECK(!seen && detector.silence_samples == 0,
	      "tone without preceding silence requested retraining");

	CHECK(v34_retrain_detector_reset(&detector) ==
		      V34_PHASE2_TONE_OK &&
	      arm_retrain_silence(&detector, codec),
	      "full-retrain silence did not arm the detector");
	if (local_role == V34_CALLING_STATION) {
		/* A bare 2400 Hz carrier omits Tone A's continuous answer guard. */
		for (size_t index = 0;
		     index < V34_RETRAIN_TONE_SAMPLES +
				     V34_RETRAIN_EDGE_SAMPLES;
		     ++index) {
			double angle = 2.0 * TEST_PI *
				       V34_PHASE2_ANSWERING_CARRIER_HZ *
				       (double)index /
				       V34_PHASE2_SAMPLE_RATE;
			int16_t sample = (int16_t)lround(
				(double)TEST_AMPLITUDE * 0.8912509381337456 *
				sin(angle));

			CHECK(feed_retrain_sample(
				      &detector, sample, codec, &seen),
			      "unguarded Tone A failed at %zu", index);
		}
		CHECK(!seen,
		      "Tone A without its answer guard requested retraining");
	}

	seen = false;
	CHECK(v34_retrain_detector_reset(&detector) ==
		      V34_PHASE2_TONE_OK &&
	      arm_retrain_silence(&detector, codec) &&
	      v34_phase2_tone_transmitter_init(
		      &tone, remote_role, TEST_AMPLITUDE) ==
		      V34_PHASE2_TONE_OK,
	      "transitioned retrain request did not reinitialize");
	for (size_t index = 0;
	     index < V34_PHASE2_TONE_WINDOW_SAMPLES * 2; ++index) {
		double angle = 2.0 * TEST_PI *
			       V34_PHASE2_ANSWERING_GUARD_HZ *
			       (double)index / V34_PHASE2_SAMPLE_RATE;
		int16_t sample = (int16_t)lround(
			(double)TEST_AMPLITUDE * sin(angle));

		CHECK(feed_retrain_sample(
			      &detector, sample, codec, &seen),
		      "retrain onset transition failed at %zu", index);
	}
	for (size_t index = 0;
	     index < V34_RETRAIN_TONE_SAMPLES +
			     V34_RETRAIN_EDGE_SAMPLES && !seen;
	     ++index) {
		int16_t sample;

		CHECK(v34_phase2_tone_transmitter_sample(&tone, &sample) ==
			      V34_PHASE2_TONE_OK &&
		      feed_retrain_sample(&detector, sample, codec, &seen),
		      "transitioned retrain tone failed at %zu", index);
	}
	CHECK(seen,
	      "valid retrain tone was lost after its onset transition");

	seen = false;
	CHECK(v34_retrain_detector_reset(&detector) ==
		      V34_PHASE2_TONE_OK &&
	      arm_retrain_silence(&detector, codec) &&
	      v34_phase2_tone_transmitter_init(
		      &tone, remote_role, TEST_AMPLITUDE) ==
		      V34_PHASE2_TONE_OK,
	      "qualified retrain request did not reinitialize");
	for (size_t index = 0;
	     index < V34_RETRAIN_TONE_SAMPLES +
			     V34_RETRAIN_EDGE_SAMPLES && !seen;
	     ++index) {
		int16_t sample;

		CHECK(v34_phase2_tone_transmitter_sample(&tone, &sample) ==
			      V34_PHASE2_TONE_OK &&
		      feed_retrain_sample(&detector, sample, codec, &seen),
		      "qualified retrain tone failed at %zu", index);
	}
	CHECK(seen && detector.silence_samples == 0 &&
	      detector.tone_samples == 0 && detector.edge_samples == 0,
	      "standards-shaped retrain request was not emitted once");
	CHECK(v34_phase2_tone_detector_measure(&detector.tone).tone_present &&
	      v34_phase2_tone_transmitter_reverse(&tone) ==
		      V34_PHASE2_TONE_OK,
	      "qualified retrain request lost its peer tone reference");
	for (size_t index = 0; index < 160; ++index) {
		struct v34_phase2_tone_event event;
		int16_t sample;

		CHECK(v34_phase2_tone_transmitter_sample(&tone, &sample) ==
			      V34_PHASE2_TONE_OK &&
		      v34_phase2_tone_detector_sample(
			      &detector.tone,
			      codec_round_trip(sample, codec), &event) ==
			      V34_PHASE2_TONE_OK,
		      "retained retrain tone failed at %zu", index);
		if (event.kind == V34_PHASE2_TONE_PHASE_REVERSED) {
			reversal = event;
			reversal_sample = index;
			reversal_count++;
		}
	}
	CHECK(reversal_count == 1 &&
	      reversal_sample + 2u >= reversal.reversal_delay_samples &&
	      reversal_sample <= reversal.reversal_delay_samples + 2u,
	      "retained retrain tone reversal count=%zu sample=%zu delay=%u",
	      reversal_count, reversal_sample,
	      reversal.reversal_delay_samples);
	return true;
}

static bool test_full_retrain_request(void)
{
	for (enum test_codec codec = TEST_CODEC_LINEAR;
	     codec <= TEST_CODEC_PCMA; ++codec)
		CHECK(check_full_retrain_request(
			      V34_CALLING_STATION, codec) &&
		      check_full_retrain_request(
			      V34_ANSWERING_STATION, codec),
		      "full-retrain request failed for codec %d", codec);
	return true;
}

static bool test_validation(void)
{
	struct v34_phase2_tone_transmitter transmitter;
	struct v34_phase2_tone_transmitter before;
	struct v34_phase2_tone_detector detector;
	struct v34_phase2_tone_detector detector_before;
	struct v34_retrain_detector retrain;
	struct v34_retrain_detector retrain_before;
	struct v34_phase2_tone_event event = {
		.kind = V34_PHASE2_TONE_ENDED,
		.reversal_delay_samples = 99,
	};
	int16_t sample = 1234;

	memset(&transmitter, 0xa5, sizeof(transmitter));
	before = transmitter;
	CHECK(v34_phase2_tone_transmitter_init(
		      &transmitter, V34_ANSWERING_STATION,
		      20000) == V34_PHASE2_TONE_INVALID_ARGUMENT &&
		      memcmp(&transmitter, &before, sizeof(transmitter)) == 0,
	      "invalid Tone A level changed caller-owned state");
	CHECK(v34_phase2_tone_transmitter_init(
		      &transmitter, V34_CALLING_STATION,
		      TEST_AMPLITUDE) == V34_PHASE2_TONE_OK,
	      "validation fixture did not initialize");
	transmitter.carrier_phase_step++;
	before = transmitter;
	CHECK(v34_phase2_tone_transmitter_sample(
		      &transmitter, &sample) ==
		      V34_PHASE2_TONE_INVALID_ARGUMENT &&
		      sample == 1234 &&
		      memcmp(&transmitter, &before, sizeof(transmitter)) == 0,
	      "corrupt tone state was not rejected transactionally");
	CHECK(v34_phase2_tone_detector_init(
		      &detector, V34_CALLING_STATION) ==
		      V34_PHASE2_TONE_OK,
	      "tone detector validation fixture did not initialize");
	detector.line_power = NAN;
	detector_before = detector;
	CHECK(v34_phase2_tone_detector_sample(
		      &detector, 0, &event) ==
		      V34_PHASE2_TONE_INVALID_ARGUMENT &&
		      memcmp(&detector, &detector_before, sizeof(detector)) == 0 &&
		      event.kind == V34_PHASE2_TONE_ENDED &&
		      event.reversal_delay_samples == 99,
	      "corrupt detector state was not rejected transactionally");
	CHECK(v34_phase2_tone_detector_init(
		      &detector, V34_CALLING_STATION) ==
		      V34_PHASE2_TONE_OK &&
		      v34_phase2_tone_detector_sample(
			      &detector, INT16_MIN, &event) ==
			      V34_PHASE2_TONE_OK &&
		      v34_phase2_tone_detector_reset(&detector) ==
			      V34_PHASE2_TONE_OK &&
		      !v34_phase2_tone_detector_measure(&detector)
			       .ready,
	      "tone detector reset retained observations");
	CHECK(v34_retrain_detector_init(
		      &retrain, V34_CALLING_STATION) ==
		      V34_PHASE2_TONE_OK,
	      "full-retrain validation fixture did not initialize");
	retrain.silence_samples = V34_RETRAIN_MIN_SILENCE_SAMPLES + 1u;
	retrain_before = retrain;
	{
		bool detected = true;

		CHECK(v34_retrain_detector_sample(
			      &retrain, 0, &detected) ==
			      V34_PHASE2_TONE_INVALID_ARGUMENT &&
		      memcmp(&retrain, &retrain_before, sizeof(retrain)) == 0 &&
		      detected,
		      "corrupt full-retrain state was not transactional");
	}
	CHECK(strcmp(
		      v34_phase2_tone_result_name(
			      V34_PHASE2_TONE_OK),
		      "ok") == 0 &&
		      strcmp(
			      v34_phase2_tone_result_name(
				      (enum v34_phase2_tone_result)99),
			      "unknown") == 0,
	      "tone result names changed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "tone levels", test_tone_levels },
		{ "phase reversal", test_phase_reversal },
		{ "detection and reversal", test_detection_and_reversal },
		{ "wrong tone rejection", test_wrong_tone_rejection },
		{ "frequency tolerance", test_frequency_tolerance },
		{ "full-retrain request", test_full_retrain_request },
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
