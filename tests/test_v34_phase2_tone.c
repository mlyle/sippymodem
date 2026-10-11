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
	TEST_HANDOFF_DELAY_SAMPLES = 52,
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

	CHECK(v34_phase2_carrier_hz(V34_CALLING_STATION) ==
		      V34_PHASE2_CALLING_CARRIER_HZ &&
	      v34_phase2_carrier_hz(V34_ANSWERING_STATION) ==
		      V34_PHASE2_ANSWERING_CARRIER_HZ &&
	      v34_phase2_carrier_hz((enum v34_station_role)99) == 0u &&
	      render(
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

static bool test_nominal_level_handoff(void)
{
	const enum v34_station_role roles[] = {
		V34_CALLING_STATION, V34_ANSWERING_STATION,
	};

	for (size_t r = 0; r < ARRAY_LENGTH(roles); ++r) {
		/* Cover every positive representable level accepted by each role. */
		for (int level = 1; level <= INT16_MAX; ++level) {
			struct v34_phase2_tone_transmitter tone;
			int16_t sample;

			if (v34_phase2_tone_transmitter_init(&tone, roles[r], level) !=
			    V34_PHASE2_TONE_OK) {
				CHECK(roles[r] == V34_ANSWERING_STATION && level > 16000,
				      "unexpected tone rejection for role %d level %d", roles[r], level);
				continue;
			}
			CHECK(v34_phase2_tone_nominal_amplitude(&tone) == level &&
			      v34_phase2_tone_transmitter_sample(&tone, &sample) ==
				      V34_PHASE2_TONE_OK &&
			      v34_phase2_tone_transmitter_reverse(&tone) ==
				      V34_PHASE2_TONE_OK &&
			      v34_phase2_tone_transmitter_sample(&tone, &sample) ==
				      V34_PHASE2_TONE_OK &&
			      v34_phase2_tone_nominal_amplitude(&tone) == level,
			      "nominal handoff changed for role %d level %d", roles[r], level);
		}
	}
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

static size_t absolute_difference(size_t left, size_t right)
{
	return left > right ? left - right : right - left;
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
		v34_station_peer_role(transmit_role);
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

static bool check_interrupted_reference(
	enum v34_station_role transmit_role, enum test_codec codec)
{
	enum v34_station_role local_role =
		v34_station_peer_role(transmit_role);
	struct v34_phase2_tone_transmitter transmitter;
	struct v34_phase2_tone_detector detector;
	struct v34_phase2_tone_event last_reversal = { 0 };
	size_t started_count = 0;
	size_t reversal_count = 0;
	size_t ended_count = 0;
	size_t sample_index = 0;
	size_t resumed_at;
	size_t event_at = SIZE_MAX;

	CHECK(v34_phase2_tone_transmitter_init(
		      &transmitter, transmit_role, TEST_AMPLITUDE) ==
		      V34_PHASE2_TONE_OK &&
	      v34_phase2_tone_detector_init(&detector, local_role) ==
		      V34_PHASE2_TONE_OK,
	      "interrupted-tone fixture did not initialize");
	for (; sample_index < 400; ++sample_index) {
		int16_t sample;

		CHECK(v34_phase2_tone_transmitter_sample(
			      &transmitter, &sample) == V34_PHASE2_TONE_OK &&
		      feed_sample(
			      &detector, sample, codec, sample_index,
			      &started_count, &reversal_count, &ended_count,
			      &last_reversal),
		      "interrupted-tone acquisition failed");
	}
	CHECK(started_count == 1,
	      "interrupted detector did not qualify its initial tone");
	for (size_t count = 0; count < 640; ++count, ++sample_index) {
		int16_t discarded;

		CHECK(v34_phase2_tone_transmitter_sample(
			      &transmitter, &discarded) == V34_PHASE2_TONE_OK &&
		      v34_phase2_tone_detector_suppress_sample(
			      &detector) ==
			      V34_PHASE2_TONE_OK,
		      "interrupted tone did not preserve oscillator time");
	}
	/* The INFO interval may cross a channel-phase change which is neither
	 * known to nor clocked by the local oscillator.  Classification still
	 * uses the retained phase, while the following tone becomes the new
	 * physical reference. */
	transmitter.carrier_phase += UINT32_C(0x33333333);
	CHECK(v34_phase2_tone_transmitter_reverse(&transmitter) ==
		      V34_PHASE2_TONE_OK,
	      "interrupted remote tone did not reverse");
	CHECK(v34_phase2_tone_detector_resume(
		      &detector, TEST_HANDOFF_DELAY_SAMPLES) ==
		      V34_PHASE2_TONE_OK,
	      "interrupted detector did not resume");
	resumed_at = sample_index;
	for (size_t count = 0; count < 240; ++count, ++sample_index) {
		int16_t sample;
		size_t before = reversal_count;

		CHECK(v34_phase2_tone_transmitter_sample(
			      &transmitter, &sample) == V34_PHASE2_TONE_OK &&
		      feed_sample(
			      &detector, sample, codec, sample_index,
			      &started_count, &reversal_count, &ended_count,
			      &last_reversal),
		      "interrupted following tone did not reacquire");
		if (reversal_count != before)
			event_at = sample_index;
	}
	CHECK(started_count == 1 && reversal_count == 1 &&
	      event_at != SIZE_MAX &&
	      event_at >= last_reversal.reversal_delay_samples &&
	      absolute_difference(
		      event_at - last_reversal.reversal_delay_samples,
		      resumed_at - TEST_HANDOFF_DELAY_SAMPLES) <= 2u,
	      "interrupted reversal edge moved from the following tone: resume=%zu event=%zu delay=%u",
	      resumed_at, event_at, last_reversal.reversal_delay_samples);
	CHECK(v34_phase2_tone_transmitter_reverse(&transmitter) ==
		      V34_PHASE2_TONE_OK,
	      "reacquired remote tone did not reverse again");
	for (size_t count = 0; count < 240; ++count, ++sample_index) {
		int16_t sample;

		CHECK(v34_phase2_tone_transmitter_sample(
			      &transmitter, &sample) == V34_PHASE2_TONE_OK &&
		      feed_sample(
			      &detector, sample, codec, sample_index,
			      &started_count, &reversal_count, &ended_count,
			      &last_reversal),
		      "post-interruption reversal failed");
	}
	CHECK(started_count == 1 && reversal_count == 2 && ended_count == 0 &&
	      detector.carrier.tone_present,
	      "reacquired carrier did not establish a fresh phase reference");
	return true;
}

static bool test_interrupted_reference(void)
{
	for (enum test_codec codec = TEST_CODEC_LINEAR;
	     codec <= TEST_CODEC_PCMA; ++codec)
		for (enum v34_station_role role = V34_CALLING_STATION;
		     role <= V34_ANSWERING_STATION; ++role)
			CHECK(check_interrupted_reference(role, codec),
			      "interrupted tone failed for role %d codec %d",
			      role, codec);
	return true;
}

static bool check_rejected_reversal(
	enum v34_station_role transmit_role, enum test_codec codec,
	bool opposite_after_info)
{
	enum v34_station_role local_role =
		v34_station_peer_role(transmit_role);
	struct v34_phase2_tone_transmitter transmitter;
	struct v34_phase2_tone_detector detector;
	double committed_in_phase = 0.0;
	double committed_quadrature = 0.0;
	size_t early_events = 0;
	size_t later_events = 0;
	size_t later_starts = 0;
	uint16_t later_delay = 0;

	CHECK(v34_phase2_tone_transmitter_init(
		      &transmitter, transmit_role, TEST_AMPLITUDE) ==
		      V34_PHASE2_TONE_OK &&
	      v34_phase2_tone_detector_init(&detector, local_role) ==
		      V34_PHASE2_TONE_OK,
	      "rejected-reversal fixture did not initialize");
	for (size_t index = 0; index < 400; ++index) {
		struct v34_phase2_tone_event event;
		int16_t sample;

		CHECK(v34_phase2_tone_transmitter_sample(
			      &transmitter, &sample) == V34_PHASE2_TONE_OK &&
		      v34_phase2_tone_detector_sample(
			      &detector, codec_round_trip(sample, codec),
			      &event) == V34_PHASE2_TONE_OK,
		      "rejected-reversal acquisition failed at %zu", index);
	}
	CHECK(detector.carrier.tone_present &&
	      v34_phase2_tone_transmitter_reverse(&transmitter) ==
		      V34_PHASE2_TONE_OK,
	      "rejected-reversal fixture did not establish its edge");
	for (size_t index = 0; index < 240 && early_events == 0; ++index) {
		struct v34_phase2_tone_event event;
		int16_t sample;

		CHECK(v34_phase2_tone_transmitter_sample(
			      &transmitter, &sample) == V34_PHASE2_TONE_OK &&
		      v34_phase2_tone_detector_sample(
			      &detector, codec_round_trip(sample, codec),
			      &event) == V34_PHASE2_TONE_OK,
		      "early reversal failed at %zu", index);
		if (event.kind == V34_PHASE2_TONE_PHASE_REVERSED) {
			early_events++;
			committed_in_phase = detector.carrier.reference_in_phase;
			committed_quadrature = detector.carrier.reference_quadrature;
			CHECK(v34_phase2_tone_detector_reject_reversal(
				      &detector, &event) == V34_PHASE2_TONE_OK,
			      "early reversal was not rejected");
		}
	}
	CHECK(early_events == 1 &&
	      detector.carrier.rejected_reversal_active &&
	      detector.carrier.reference_in_phase == -committed_in_phase &&
	      detector.carrier.reference_quadrature == -committed_quadrature,
	      "rejection did not restore the retained phase");
	for (size_t index = 0; index < 240; ++index) {
		struct v34_phase2_tone_event event;
		int16_t sample;

		CHECK(v34_phase2_tone_transmitter_sample(
			      &transmitter, &sample) == V34_PHASE2_TONE_OK &&
		      v34_phase2_tone_detector_sample(
			      &detector, codec_round_trip(sample, codec),
			      &event) == V34_PHASE2_TONE_OK &&
		      event.kind != V34_PHASE2_TONE_PHASE_REVERSED,
		      "stable rejected phase was emitted again at %zu", index);
	}
	for (size_t index = 0; index < 640; ++index) {
		int16_t discarded;

		CHECK(v34_phase2_tone_transmitter_sample(
			      &transmitter, &discarded) == V34_PHASE2_TONE_OK &&
		      v34_phase2_tone_detector_suppress_sample(&detector) ==
			      V34_PHASE2_TONE_OK,
		      "INFO suppression failed at %zu", index);
	}
	if (!opposite_after_info)
		CHECK(v34_phase2_tone_transmitter_reverse(&transmitter) ==
			      V34_PHASE2_TONE_OK,
		      "same-phase post-INFO tone did not restore its phase");
	CHECK(!detector.carrier.rejected_reversal_active &&
	      v34_phase2_tone_detector_resume(&detector, 0) ==
		      V34_PHASE2_TONE_OK,
	      "rejected INFO boundary did not reset the phase gate");
	for (size_t index = 0; index < 240; ++index) {
		struct v34_phase2_tone_event event;
		int16_t sample;

		CHECK(v34_phase2_tone_transmitter_sample(
			      &transmitter, &sample) == V34_PHASE2_TONE_OK &&
		      v34_phase2_tone_detector_sample(
			      &detector, codec_round_trip(sample, codec),
			      &event) == V34_PHASE2_TONE_OK,
		      "post-INFO reversal failed at %zu", index);
		if (event.kind == V34_PHASE2_TONE_PHASE_REVERSED) {
			later_events++;
			later_delay = event.reversal_delay_samples;
		}
		later_starts += event.kind == V34_PHASE2_TONE_STARTED;
	}
	CHECK(!detector.carrier.rejected_reversal_active &&
	      later_events == (opposite_after_info ? 1u : 0u) &&
	      later_starts == (opposite_after_info ? 0u : 1u) &&
	      (!opposite_after_info ||
	       later_delay == V34_PHASE2_TONE_WINDOW_SAMPLES +
			      V34_PHASE2_TONE_ACQUIRE_SAMPLES - 2u),
	      "rejected-frame handoff changed for %d: reversed=%zu started=%zu delay=%u",
	      opposite_after_info, later_events, later_starts, later_delay);
	return true;
}

static bool test_rejected_reversal(void)
{
	for (enum test_codec codec = TEST_CODEC_LINEAR;
	     codec <= TEST_CODEC_PCMA; ++codec)
		for (enum v34_station_role role = V34_CALLING_STATION;
		     role <= V34_ANSWERING_STATION; ++role)
			for (unsigned int opposite = 0; opposite < 2; ++opposite)
				CHECK(check_rejected_reversal(
					      role, codec, opposite != 0),
				      "rejected reversal failed for role %d codec %d phase %u",
				      role, codec, opposite);
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
		v34_station_peer_role(transmit_role);
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
	     index < V34_RETRAIN_MIN_SILENCE_SAMPLES - 1u; ++index) {
		/* This remains a quiet 64-sample energy window, but exceeds the
		 * former 40-sample threshold until its onset ages out. */
		int16_t sample = index < 3 ? 1500 : 0;

		CHECK(feed_retrain_sample(detector, sample, codec, &seen),
		      "full-retrain silence failed at %zu", index);
	}
	CHECK(!seen && detector->silence_samples ==
		      V34_RETRAIN_MIN_SILENCE_SAMPLES - 1u,
	      "full-retrain silence armed before its sampled-edge guard");
	CHECK(feed_retrain_sample(detector, 0, codec, &seen),
	      "full-retrain silence failed at its sampled-edge guard");
	return !seen && detector->silence_samples ==
			V34_RETRAIN_MIN_SILENCE_SAMPLES;
}

static bool check_retrain_silence_glitch(enum test_codec codec)
{
	struct v34_retrain_detector detector;
	bool seen = false;

	CHECK(v34_retrain_detector_init(
		      &detector, V34_CALLING_STATION) ==
		      V34_PHASE2_TONE_OK,
	      "silence-glitch fixture did not initialize");
	for (size_t index = 0; index < 128; ++index)
		CHECK(feed_retrain_sample(&detector, 0, codec, &seen),
		      "silence-glitch prefix failed at %zu", index);
	CHECK(detector.silence_samples == 128,
	      "silence-glitch prefix count=%u", detector.silence_samples);
	for (size_t index = 0; index < 16; ++index)
		CHECK(feed_retrain_sample(&detector, 8000, codec, &seen),
		      "bounded silence impulse failed at %zu", index);
	for (size_t index = 0; index < 63; ++index)
		CHECK(feed_retrain_sample(&detector, 0, codec, &seen),
		      "bounded silence recovery failed at %zu", index);
	CHECK(detector.silence_samples == 128 &&
	      detector.silence_glitch_samples == 79,
	      "bounded silence impulse was not retained");
	CHECK(feed_retrain_sample(&detector, 0, codec, &seen) &&
	      detector.silence_samples == 129 &&
	      detector.silence_glitch_samples == 0,
	      "bounded silence impulse did not recover");

	CHECK(v34_retrain_detector_reset(&detector) ==
		      V34_PHASE2_TONE_OK,
	      "long silence-glitch fixture did not reset");
	for (size_t index = 0; index < 128; ++index)
		CHECK(feed_retrain_sample(&detector, 0, codec, &seen),
		      "long silence-glitch prefix failed at %zu", index);
	for (size_t index = 0; index < 18; ++index)
		CHECK(feed_retrain_sample(&detector, 8000, codec, &seen),
		      "long silence impulse failed at %zu", index);
	for (size_t index = 0; index < 63; ++index)
		CHECK(feed_retrain_sample(&detector, 0, codec, &seen),
		      "long silence recovery failed at %zu", index);
	CHECK(!seen && detector.silence_samples == 0 &&
	      detector.silence_glitch_samples == 0,
	      "overlong silence impulse did not restart qualification");
	return true;
}

static bool check_full_retrain_request(
	enum v34_station_role local_role, enum test_codec codec)
{
	enum v34_station_role remote_role =
		v34_station_peer_role(local_role);
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

static bool check_retrain_quiet_tolerance(
	enum v34_station_role local_role, enum test_codec codec)
{
	struct v34_phase2_tone_transmitter tone;
	struct v34_retrain_detector detector;
	bool seen = false;
	const size_t quiet_60ms = V34_PHASE2_SAMPLE_RATE * 60u / 1000u;
	const size_t quiet_59ms = V34_PHASE2_SAMPLE_RATE * 59u / 1000u;

	for (unsigned shortened = 0; shortened < 2; ++shortened) {
		size_t quiet = shortened ? quiet_59ms : quiet_60ms;
		CHECK(v34_retrain_detector_init(&detector, local_role) ==
			      V34_PHASE2_TONE_OK &&
		      v34_phase2_tone_transmitter_init(&tone,
			      v34_station_peer_role(local_role), TEST_AMPLITUDE) ==
			      V34_PHASE2_TONE_OK,
		      "short-preamble fixture did not initialize");
		seen = false;
		/* Seed the sliding window with active signal before quiet begins. */
		for (size_t index = 0; index < 128; ++index)
			CHECK(feed_retrain_sample(&detector, (index & 1u) ? 8000 : -8000, codec, &seen),
			      "active prefix failed");
		for (size_t index = 0; index < quiet; ++index)
			CHECK(feed_retrain_sample(&detector, 0, codec, &seen),
			      "short quiet failed");
		for (size_t index = 0;
		     index < V34_RETRAIN_TONE_SAMPLES + V34_RETRAIN_EDGE_SAMPLES;
		     ++index) {
			int16_t sample;
			bool before = seen;
			CHECK(v34_phase2_tone_transmitter_sample(&tone, &sample) ==
				      V34_PHASE2_TONE_OK &&
			      feed_retrain_sample(&detector, sample, codec, &seen),
			      "short-preamble tone failed");
			CHECK(!seen || before || index + 2u >= V34_RETRAIN_TONE_SAMPLES,
			      "short preamble weakened continuous tone duration");
		}
		CHECK(seen == !shortened,
		      "%zu quiet samples: expected %s, got %s",
		      quiet, shortened ? "rejection" : "detection",
		      seen ? "detection" : "rejection");
	}

	CHECK(v34_retrain_detector_init(&detector, local_role) ==
		      V34_PHASE2_TONE_OK &&
	      v34_phase2_tone_transmitter_init(&tone,
		      v34_station_peer_role(local_role), TEST_AMPLITUDE) ==
		      V34_PHASE2_TONE_OK,
	      "broken-tone fixture did not initialize");
	seen = false;
	for (size_t index = 0; index < quiet_60ms; ++index)
		CHECK(feed_retrain_sample(&detector, 0, codec, &seen),
		      "broken-tone quiet failed");
	for (size_t index = 0; index < V34_PHASE2_SAMPLE_RATE * 55u / 1000u; ++index) {
		int16_t sample;
		CHECK(v34_phase2_tone_transmitter_sample(&tone, &sample) ==
			      V34_PHASE2_TONE_OK &&
		      feed_retrain_sample(&detector, sample, codec, &seen),
		      "short correct tone failed");
	}
	CHECK(!seen, "55 ms tone passed the retained 80 ms guard");
	for (size_t index = 0; index < V34_PHASE2_SAMPLE_RATE / 50u; ++index)
		CHECK(feed_retrain_sample(&detector, (index & 1u) ? 8000 : -8000, codec, &seen),
		      "tone interruption failed");
	for (size_t index = 0;
	     index < V34_RETRAIN_TONE_SAMPLES + V34_RETRAIN_EDGE_SAMPLES; ++index) {
		int16_t sample;
		CHECK(v34_phase2_tone_transmitter_sample(&tone, &sample) ==
			      V34_PHASE2_TONE_OK &&
		      feed_retrain_sample(&detector, sample, codec, &seen),
		      "tone after interruption failed");
	}
	CHECK(!seen, "broken tone fragments accumulated into a retrain");

	CHECK(v34_retrain_detector_init(&detector, local_role) ==
		      V34_PHASE2_TONE_OK,
	      "quiet-to-data fixture did not initialize");
	seen = false;
	for (size_t index = 0; index < quiet_60ms; ++index)
		CHECK(feed_retrain_sample(&detector, 0, codec, &seen),
		      "quiet-to-data preamble failed");
	uint32_t random = UINT32_C(0x42f7139b);
	for (size_t index = 0; index < 8000; ++index) {
		random = random * UINT32_C(1664525) + UINT32_C(1013904223);
		int16_t sample = (int16_t)((int32_t)(random >> 16) % 16001 - 8000);
		CHECK(feed_retrain_sample(&detector, sample, codec, &seen),
		      "quiet-to-data sample failed");
	}
	CHECK(!seen, "quiet followed by ordinary signal requested retraining");
	return true;
}

static bool test_retrain_quiet_tolerance(void)
{
	for (enum test_codec codec = TEST_CODEC_LINEAR; codec <= TEST_CODEC_PCMA; ++codec)
		for (enum v34_station_role role = V34_CALLING_STATION;
		     role <= V34_ANSWERING_STATION; ++role)
			CHECK(check_retrain_quiet_tolerance(role, codec),
			      "quiet tolerance failed for role %d codec %d", role, codec);
	return true;
}

static bool test_full_retrain_request(void)
{
	for (enum test_codec codec = TEST_CODEC_LINEAR;
	     codec <= TEST_CODEC_PCMA; ++codec)
		CHECK(check_retrain_silence_glitch(codec) &&
		      check_full_retrain_request(
			      V34_CALLING_STATION, codec) &&
		      check_full_retrain_request(
			      V34_ANSWERING_STATION, codec),
		      "full-retrain request failed for codec %d", codec);
	return true;
}

static bool test_receiver_silence_ceiling(void)
{
	const uint32_t ceiling = V34_RETRAIN_SILENCE_MAX_WINDOW_ENERGY / 256u;

	for (enum test_codec codec = TEST_CODEC_LINEAR;
	     codec <= TEST_CODEC_PCMA; ++codec) {
		struct v34_retrain_detector trained;
		struct v34_retrain_detector nominal;
		struct v34_retrain_detector explicit_nominal;
		struct v34_phase2_tone_transmitter tone;
		bool detected = false;
		bool nominal_detected = false;
		bool explicit_detected = false;

		CHECK(v34_retrain_detector_init(&trained, V34_ANSWERING_STATION) ==
			      V34_PHASE2_TONE_OK,
		      "trained silence fixture did not initialize");
		nominal = trained;
		explicit_nominal = nominal;
		for (size_t index = 0;
		     index < V34_RETRAIN_MIN_SILENCE_SAMPLES + 1600u; ++index) {
			/* Quiet arms both detectors. This following weak non-tone
			 * stream must expire, not indefinitely preserve, that guard. */
			int16_t sample = index < V34_RETRAIN_MIN_SILENCE_SAMPLES ?
				0 : (index & 1u) != 0u ? 200 : -200;

			sample = codec_round_trip(sample, codec);
			CHECK(v34_retrain_detector_sample_with_power_limit(
				      &trained, sample, ceiling, &detected) ==
				      V34_PHASE2_TONE_OK && !detected &&
			      v34_retrain_detector_sample(
				      &nominal, sample, &nominal_detected) ==
				      V34_PHASE2_TONE_OK && !nominal_detected &&
			      v34_retrain_detector_sample_with_power_limit(
				      &explicit_nominal, sample,
				      V34_RETRAIN_SILENCE_MAX_WINDOW_ENERGY,
				      &explicit_detected) == V34_PHASE2_TONE_OK &&
			      explicit_detected == nominal_detected &&
			      memcmp(&nominal, &explicit_nominal, sizeof(nominal)) == 0,
			      "silence ceiling changed defaults or invented a tone");
			if (index + 1u == V34_RETRAIN_MIN_SILENCE_SAMPLES)
				CHECK(trained.silence_samples ==
					      V34_RETRAIN_MIN_SILENCE_SAMPLES,
				      "trained silence guard did not arm on real quiet");
		}
		CHECK(trained.silence_samples == 0u && trained.tone_samples == 0u &&
		      nominal.silence_samples == V34_RETRAIN_MIN_SILENCE_SAMPLES,
		      "weak input retained an armed trained silence guard");
		for (size_t index = 0;
		     index < V34_RETRAIN_MIN_SILENCE_SAMPLES +
			     V34_RETRAIN_SILENCE_WINDOW_SAMPLES; ++index)
			CHECK(v34_retrain_detector_sample_with_power_limit(
				      &trained, codec_round_trip(0, codec), ceiling,
				      &detected) == V34_PHASE2_TONE_OK && !detected,
			      "trained guard did not observe subsequent real quiet");
		CHECK(trained.silence_samples == V34_RETRAIN_MIN_SILENCE_SAMPLES &&
		      v34_phase2_tone_transmitter_init(
			      &tone, V34_CALLING_STATION, 200) == V34_PHASE2_TONE_OK,
		      "weak Tone B fixture did not initialize after real quiet");
		for (size_t index = 0; !detected &&
		     index < V34_RETRAIN_TONE_SAMPLES + V34_RETRAIN_EDGE_SAMPLES;
		     ++index) {
			int16_t sample;

			CHECK(v34_phase2_tone_transmitter_sample(&tone, &sample) ==
				      V34_PHASE2_TONE_OK &&
			      v34_retrain_detector_sample_with_power_limit(
				      &trained, codec_round_trip(sample, codec), ceiling,
				      &detected) == V34_PHASE2_TONE_OK,
			      "trained ceiling rejected a genuine raw tone");
		}
		CHECK(detected, "trained silence ceiling hid genuine retraining");
		{
			struct v34_full_retrain response = { .detector = trained };
			struct v34_phase2_tone_detector resumed;
			double minimum_power = trained.tone.carrier.profile.minimum_line_power;
			unsigned int reversals = 0u;

			CHECK(minimum_power == 256.0 &&
			      v34_full_retrain_begin_response(&response, true) ==
				      V34_PHASE2_TONE_OK,
			      "weak request did not retain its raw tone level");
			for (size_t index = 0;
			     index < V34_FULL_RETRAIN_RESPONSE_SILENCE_SAMPLES; ++index) {
				int16_t sample;
				enum v34_full_retrain_response_event event;

				CHECK(v34_phase2_tone_transmitter_sample(&tone, &sample) ==
					      V34_PHASE2_TONE_OK &&
				      v34_full_retrain_sample_response(
					      &response, codec_round_trip(sample, codec), &event) ==
					      V34_PHASE2_TONE_OK &&
				      event == (index + 1u == V34_FULL_RETRAIN_RESPONSE_SILENCE_SAMPLES ?
					      V34_FULL_RETRAIN_RESPONSE_COMPLETES_WITH_PEER_TONE :
					      V34_FULL_RETRAIN_RESPONSE_CONTINUES),
				      "weak tone lost its 70-ms response ownership");
			}
			resumed = response.detector.tone;
			CHECK(resumed.carrier.profile.minimum_line_power == minimum_power &&
			      v34_phase2_tone_transmitter_reverse(&tone) == V34_PHASE2_TONE_OK,
			      "weak Phase 2 handoff changed its raw amplitude domain");
			for (size_t index = 0; index < 160u; ++index) {
				int16_t sample;
				struct v34_phase2_tone_event event;

				CHECK(v34_phase2_tone_transmitter_sample(&tone, &sample) ==
					      V34_PHASE2_TONE_OK &&
				      v34_phase2_tone_detector_sample(
					      &resumed, codec_round_trip(sample, codec), &event) ==
					      V34_PHASE2_TONE_OK,
				      "weak retained phase did not continue");
				reversals += event.kind == V34_PHASE2_TONE_PHASE_REVERSED;
			}
			CHECK(reversals == 1u &&
			      v34_phase2_tone_detector_reset(&resumed) == V34_PHASE2_TONE_OK &&
			      resumed.carrier.profile.minimum_line_power == minimum_power &&
			      !v34_phase2_tone_detector_measure(&resumed).ready &&
			      v34_retrain_detector_reset(&trained) == V34_PHASE2_TONE_OK &&
			      trained.tone.carrier.profile.minimum_line_power == minimum_power &&
			      v34_retrain_detector_sample(&trained, 0, &detected) ==
				      V34_PHASE2_TONE_OK &&
			      trained.tone.carrier.profile.minimum_line_power ==
				      nominal.tone.carrier.profile.minimum_line_power,
			      "weak receive-level reset/default rules changed");
		}
	}
	return true;
}

static bool check_full_retrain_response(
	enum v34_station_role local_role, enum test_codec codec,
	size_t dropout_start)
{
	struct v34_full_retrain retrain;
	struct v34_phase2_tone_transmitter peer_tone;
	struct v34_phase2_tone_detector retained;
	bool detected = false;
	bool tone_lost = false;

	CHECK(v34_full_retrain_init(&retrain, local_role) ==
		      V34_PHASE2_TONE_OK &&
	      v34_phase2_tone_transmitter_init(
		      &peer_tone, v34_station_peer_role(local_role),
		      TEST_AMPLITUDE) == V34_PHASE2_TONE_OK &&
	      arm_retrain_silence(&retrain.detector, codec),
	      "peer full-retrain response fixture did not initialize");
	for (size_t index = 0;
	     index < V34_RETRAIN_TONE_SAMPLES + V34_RETRAIN_EDGE_SAMPLES &&
	     !detected;
	     ++index) {
		int16_t sample;

		CHECK(v34_phase2_tone_transmitter_sample(
			      &peer_tone, &sample) == V34_PHASE2_TONE_OK &&
		      feed_retrain_sample(
			      &retrain.detector, sample, codec, &detected),
		      "peer full-retrain request stopped at %zu", index);
	}
	CHECK(detected &&
	      v34_full_retrain_begin_response(&retrain, true) ==
		      V34_PHASE2_TONE_OK &&
	      v34_full_retrain_response_is_active(&retrain),
	      "peer full-retrain response did not begin");
	retained = retrain.detector.tone;
	for (size_t index = 0;
	     index < V34_FULL_RETRAIN_RESPONSE_SILENCE_SAMPLES; ++index) {
		struct v34_phase2_tone_event tone_event;
		enum v34_full_retrain_response_event event;
		int16_t sample;
		int16_t received;
		enum v34_full_retrain_response_event expected;

		CHECK(v34_phase2_tone_transmitter_sample(
			      &peer_tone, &sample) == V34_PHASE2_TONE_OK,
		      "peer retrain tone stopped during response at %zu", index);
		/* Keep the peer's oscillator running through a bounded loss. An
		 * early loss returns before the response ends; a late one tests
		 * loss close to the Phase 2 handoff. Neither may retain stale phase. */
		received = codec_round_trip(
			index >= dropout_start &&
			index - dropout_start < 3u * V34_PHASE2_TONE_LOSS_SAMPLES ?
				0 : sample, codec);
		CHECK(v34_phase2_tone_detector_sample(
			      &retained, received, &tone_event) ==
			      V34_PHASE2_TONE_OK,
		      "retained response observer failed at %zu", index);
		tone_lost |= tone_event.kind == V34_PHASE2_TONE_ENDED;
		expected = index + 1u == V34_FULL_RETRAIN_RESPONSE_SILENCE_SAMPLES ?
			(tone_lost ? V34_FULL_RETRAIN_RESPONSE_COMPLETES :
			 V34_FULL_RETRAIN_RESPONSE_COMPLETES_WITH_PEER_TONE) :
			V34_FULL_RETRAIN_RESPONSE_CONTINUES;
		CHECK(v34_full_retrain_sample_response(
			      &retrain, received, &event) == V34_PHASE2_TONE_OK &&
		      event == expected &&
		      retrain.response_silence_samples_remaining ==
			V34_FULL_RETRAIN_RESPONSE_SILENCE_SAMPLES - index - 1u &&
		      retrain.peer_tone_retained ==
			(!tone_lost && event == V34_FULL_RETRAIN_RESPONSE_CONTINUES),
		      "peer full-retrain response failed at %zu", index);
	}
	CHECK(v34_full_retrain_is_valid(&retrain) &&
	      !v34_full_retrain_response_is_active(&retrain) &&
	      !retrain.peer_tone_retained &&
	      (tone_lost ?
		!v34_phase2_tone_detector_measure(&retrain.detector.tone).tone_present :
		memcmp(&retrain.detector.tone, &retained, sizeof(retained)) == 0) &&
	      tone_lost == (dropout_start != SIZE_MAX),
	      "peer full-retrain response lost retained tone phase");

	CHECK(v34_full_retrain_init(&retrain, local_role) ==
		      V34_PHASE2_TONE_OK,
	      "local full-retrain response fixture did not initialize");
	detected = false;
	for (size_t index = 0; index < 80; ++index)
		CHECK(feed_retrain_sample(
			      &retrain.detector, 0, codec, &detected),
		      "local response detector prefix failed at %zu", index);
	CHECK(v34_full_retrain_begin_response(&retrain, false) ==
		      V34_PHASE2_TONE_OK,
	      "local full-retrain response did not begin");
	for (size_t index = 0;
	     index < V34_FULL_RETRAIN_RESPONSE_SILENCE_SAMPLES; ++index) {
		enum v34_full_retrain_response_event event;
		enum v34_full_retrain_response_event expected =
			index + 1u ==
					V34_FULL_RETRAIN_RESPONSE_SILENCE_SAMPLES ?
				V34_FULL_RETRAIN_RESPONSE_COMPLETES :
				V34_FULL_RETRAIN_RESPONSE_CONTINUES;

		CHECK(v34_full_retrain_sample_response(
			      &retrain, INT16_MAX, &event) ==
			      V34_PHASE2_TONE_OK &&
		      event == expected,
		      "local full-retrain response failed at %zu", index);
	}
	return v34_full_retrain_is_valid(&retrain) &&
	       !v34_full_retrain_response_is_active(&retrain) &&
	       !retrain.peer_tone_retained &&
	       retrain.detector.silence_samples == 0u;
}

static bool test_attenuated_retrain_guard(void)
{
	static const struct {
		double guard_gain;
		unsigned int guard_hz;
		bool detected;
	} cases[] = {
		{ 0.3981071705534972, 1800, true }, /* Eight dB guard loss. */
		{ 0.1, 1800, false }, /* Effectively bare carrier. */
		{ 0.3981071705534972, 1300, false }, /* Unrelated energy. */
	};

	for (enum test_codec codec = TEST_CODEC_LINEAR;
	     codec <= TEST_CODEC_PCMA; ++codec) {
		for (size_t test = 0; test < ARRAY_LENGTH(cases); ++test) {
			struct v34_retrain_detector detector;
			bool seen = false;

			CHECK(v34_retrain_detector_init(
				      &detector, V34_CALLING_STATION) ==
				      V34_PHASE2_TONE_OK &&
			      arm_retrain_silence(&detector, codec),
			      "attenuated retrain guard did not initialize");
			for (size_t index = 0;
			     index < V34_RETRAIN_TONE_SAMPLES +
					     V34_RETRAIN_EDGE_SAMPLES && !seen;
			     ++index) {
				double phase = 2.0 * TEST_PI * index /
					V34_PHASE2_SAMPLE_RATE;
				int16_t sample = (int16_t)lround(TEST_AMPLITUDE *
					(0.8912509381337456 * sin(2400 * phase) +
					 cases[test].guard_gain *
					 sin(cases[test].guard_hz * phase)));

				CHECK(feed_retrain_sample(
					      &detector, sample, codec, &seen),
				      "attenuated guard failed at %zu", index);
			}
			CHECK(seen == cases[test].detected,
			      "retrain guard case=%zu codec=%d detected=%d",
			      test, codec, seen);
		}
	}
	return true;
}

static bool test_full_retrain_response(void)
{
	for (enum test_codec codec = TEST_CODEC_LINEAR;
	     codec <= TEST_CODEC_PCMA; ++codec)
		for (enum v34_station_role role = V34_CALLING_STATION;
		     role <= V34_ANSWERING_STATION; ++role)
			CHECK(check_full_retrain_response(role, codec, SIZE_MAX) &&
			      check_full_retrain_response(role, codec, 0u) &&
			      check_full_retrain_response(role, codec,
				V34_FULL_RETRAIN_RESPONSE_SILENCE_SAMPLES -
				2u * V34_PHASE2_TONE_LOSS_SAMPLES),
			      "full-retrain response failed role=%d codec=%d",
			      role, codec);
	return true;
}

static bool test_receive_gain_handoff(void)
{
	static const int16_t gains[] = { 1, 512, 1024, 8192, INT16_MAX };

	for (enum v34_station_role role = V34_CALLING_STATION;
	     role <= V34_ANSWERING_STATION; ++role) {
		struct v34_phase2_tone_detector detector;
		struct v34_phase2_tone_detector before;
		struct v34_phase2_tone_event event;

		CHECK(v34_phase2_tone_detector_init(&detector, role) ==
			V34_PHASE2_TONE_OK, "gain fixture did not initialize");
		for (size_t i = 0; i < 99; ++i)
			CHECK(v34_phase2_tone_detector_sample(
				&detector, (int16_t)(i * 19), &event) ==
				V34_PHASE2_TONE_OK, "gain fixture lost sample");
		before = detector;
		for (size_t i = 0; i < ARRAY_LENGTH(gains); ++i) {
			struct v34_phase2_tone_detector expected = before;
			double gain = gains[i] > 1024 ? gains[i] / 1024.0 : 1.0;

			detector = before;
			expected.carrier.profile.minimum_line_power /= gain * gain;
			CHECK(v34_phase2_tone_detector_set_receive_gain(
				&detector, gains[i]) == V34_PHASE2_TONE_OK &&
			      memcmp(&detector, &expected, sizeof(detector)) == 0 &&
			      v34_phase2_tone_detector_is_valid(&detector),
			      "gain %d changed phase/history or tone contract",
			      gains[i]);
			CHECK(v34_phase2_tone_detector_reset(&detector) ==
				V34_PHASE2_TONE_OK &&
			      detector.carrier.profile.minimum_line_power ==
				expected.carrier.profile.minimum_line_power,
			      "reset dropped qualified receive level");
		}
		detector = before;
		CHECK(v34_phase2_tone_detector_set_receive_gain(&detector, 0) ==
			V34_PHASE2_TONE_INVALID_ARGUMENT &&
		      v34_phase2_tone_detector_set_receive_gain(&detector, -1) ==
			V34_PHASE2_TONE_INVALID_ARGUMENT &&
		      v34_phase2_tone_detector_set_receive_gain(NULL, 1024) ==
			V34_PHASE2_TONE_INVALID_ARGUMENT &&
		      memcmp(&detector, &before, sizeof(detector)) == 0,
		      "invalid receive gain changed detector");
		CHECK(v34_phase2_tone_detector_set_receive_gain(&detector, 8192) ==
			V34_PHASE2_TONE_OK &&
		      v34_phase2_tone_detector_init(&detector, role) ==
			V34_PHASE2_TONE_OK &&
		      detector.carrier.profile.minimum_line_power ==
			before.carrier.profile.minimum_line_power,
		      "fresh call inherited old receive level");
	}
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
	struct v34_full_retrain full_retrain;
	struct v34_full_retrain full_retrain_before;
	enum v34_full_retrain_response_event response_event =
		V34_FULL_RETRAIN_RESPONSE_COMPLETES_WITH_PEER_TONE;
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
	detector_before = detector;
	CHECK(v34_phase2_tone_detector_reject_reversal(
		      &detector, &event) == V34_PHASE2_TONE_INVALID_ARGUMENT &&
	      memcmp(&detector, &detector_before, sizeof(detector)) == 0,
	      "invalid reversal rejection mutated detector state");
	detector.carrier.line_energy = NAN;
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
	retrain_before = retrain;
	{
		bool detected = true;

		CHECK(v34_retrain_detector_sample_with_power_limit(
			      &retrain, 0, 0u, &detected) ==
			      V34_PHASE2_TONE_INVALID_ARGUMENT &&
		      v34_retrain_detector_sample_with_power_limit(
			      &retrain, 0,
			      V34_RETRAIN_SILENCE_MAX_WINDOW_ENERGY + 1u,
			      &detected) == V34_PHASE2_TONE_INVALID_ARGUMENT &&
		      memcmp(&retrain, &retrain_before, sizeof(retrain)) == 0 &&
		      detected,
		      "invalid retrain silence ceiling was not transactional");
	}
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
	CHECK(v34_retrain_detector_init(
		      &retrain, V34_CALLING_STATION) ==
		      V34_PHASE2_TONE_OK,
	      "silence-history validation fixture did not initialize");
	retrain.silence_sum = INT32_MAX;
	retrain_before = retrain;
	{
		bool detected = true;

		CHECK(v34_retrain_detector_sample(
			      &retrain, 0, &detected) ==
			      V34_PHASE2_TONE_INVALID_ARGUMENT &&
		      memcmp(&retrain, &retrain_before, sizeof(retrain)) == 0 &&
		      detected,
		      "corrupt silence history was not transactional");
	}
	CHECK(v34_full_retrain_init(
		      &full_retrain, V34_CALLING_STATION) ==
		      V34_PHASE2_TONE_OK &&
	      !v34_full_retrain_response_is_active(&full_retrain),
	      "full-retrain response validation fixture did not initialize");
	full_retrain_before = full_retrain;
	CHECK(v34_full_retrain_begin_response(&full_retrain, true) ==
		      V34_PHASE2_TONE_INVALID_ARGUMENT &&
	      memcmp(&full_retrain, &full_retrain_before,
		     sizeof(full_retrain)) == 0,
	      "response without a peer tone changed caller-owned state");
	CHECK(v34_full_retrain_begin_response(&full_retrain, false) ==
		      V34_PHASE2_TONE_OK,
	      "local full-retrain response validation did not begin");
	full_retrain_before = full_retrain;
	CHECK(v34_full_retrain_begin_response(&full_retrain, false) ==
		      V34_PHASE2_TONE_INVALID_ARGUMENT &&
	      memcmp(&full_retrain, &full_retrain_before,
		     sizeof(full_retrain)) == 0,
	      "nested full-retrain response changed caller-owned state");
	full_retrain.response_silence_samples_remaining =
		V34_FULL_RETRAIN_RESPONSE_SILENCE_SAMPLES + 1u;
	full_retrain_before = full_retrain;
	CHECK(v34_full_retrain_sample_response(
		      &full_retrain, 0, &response_event) ==
		      V34_PHASE2_TONE_INVALID_ARGUMENT &&
	      memcmp(&full_retrain, &full_retrain_before,
		     sizeof(full_retrain)) == 0 &&
	      response_event ==
		      V34_FULL_RETRAIN_RESPONSE_COMPLETES_WITH_PEER_TONE,
	      "corrupt response state was not rejected transactionally");
	return true;
}

/* Quiet remains quiet with a DC offset; a tone or ordinary signal does not.
 * Preserve the same silence duration, role-specific tone and answer guard. */
static bool check_retrain_dc_offset(enum v34_station_role role,
	enum test_codec codec, int offset, bool quiet)
{
	struct v34_retrain_detector detector;
	struct v34_phase2_tone_transmitter tone;
	bool seen = false;

	CHECK(v34_retrain_detector_init(&detector, role) == V34_PHASE2_TONE_OK &&
	      v34_phase2_tone_transmitter_init(&tone, v34_station_peer_role(role),
		      4000) == V34_PHASE2_TONE_OK, "DC retrain fixture failed");
	for (size_t i = 0; i < 128; ++i)
		CHECK(feed_retrain_sample(&detector, (i & 1u) ? 8000 : -8000, codec, &seen),
		      "DC active prefix failed");
	if (quiet)
		for (size_t i = 0; i < 560; ++i)
			CHECK(feed_retrain_sample(&detector,
				offset + ((i & 1u) ? 100 : -100), codec, &seen),
			      "DC quiet preamble failed");
	for (size_t i = 0; i < 1600; ++i) {
		int16_t sample;
		CHECK(v34_phase2_tone_transmitter_sample(&tone, &sample) == V34_PHASE2_TONE_OK &&
		      feed_retrain_sample(&detector, sample + offset, codec, &seen),
		      "DC retrain tone failed");
	}
	CHECK(seen == quiet, "DC retrain role=%d codec=%d offset=%d quiet=%d detected=%d",
	      role, codec, offset, quiet, seen);
	return true;
}

static bool test_retrain_dc_offset(void)
{
	for (enum test_codec codec = TEST_CODEC_LINEAR; codec <= TEST_CODEC_PCMA; ++codec)
		for (enum v34_station_role role = V34_CALLING_STATION; role <= V34_ANSWERING_STATION; ++role)
			for (int offset = -600; offset <= 600; offset += 1200)
				for (unsigned quiet = 0; quiet < 2; ++quiet)
					CHECK(check_retrain_dc_offset(role, codec, offset, quiet != 0),
					      "DC quiet qualification failed");
	return true;
}

int main(void)
{
	static const struct {
		const char *name;
		bool (*run)(void);
	} tests[] = {
		{ "DC retrain quiet guard", test_retrain_dc_offset },
		{ "tone levels", test_tone_levels },
		{ "nominal level handoff", test_nominal_level_handoff },
		{ "phase reversal", test_phase_reversal },
		{ "detection and reversal", test_detection_and_reversal },
		{ "interrupted phase reference", test_interrupted_reference },
		{ "rejected reversal", test_rejected_reversal },
		{ "wrong tone rejection", test_wrong_tone_rejection },
		{ "frequency tolerance", test_frequency_tolerance },
		{ "full-retrain request", test_full_retrain_request },
		{ "attenuated retrain guard", test_attenuated_retrain_guard },
		{ "short retrain quiet guard", test_retrain_quiet_tolerance },
		{ "trained receiver silence ceiling", test_receiver_silence_ceiling },
		{ "full-retrain response", test_full_retrain_response },
		{ "receive gain handoff", test_receive_gain_handoff },
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
